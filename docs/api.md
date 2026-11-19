# API 与扩展接口

本页记录设备侧 Provider、Root 代理协议、桌面端执行接口、插件接口和漏洞库接口。

## 1. 设备侧 ContentProvider

固定包名：`com.phonetoolbox.agent`  
Authority：`com.phonetoolbox.agent.agentinfo`  
调用者限制：`uid == 2000`（adb shell）或 `uid == 0`（root）。

```bash
adb shell content query --uri content://com.phonetoolbox.agent.agentinfo/agent
```

### `/agent`

返回两列：

| 列 | 类型 | 含义 |
|----|------|------|
| `port` | int | agent 监听端口（0 = 未启动） |
| `paired` | int | 是否已有配对电脑（1/0） |

### `/audit`

返回一列 `audit`：hash-chain 审计日志文本，最多读取**最后** 256 KiB。文件位于 APK 私有目录 `getFilesDir()/audit.log`（0600；旧版本的外部目录文件仅作迁移回退）。

记录格式：

```
prev_hex timestamp peer_hex op argv_sha256 exit entry_hex
```

`peer_hex` 是已认证会话的 Noise 静态公钥（64 hex）；明文开发模式回退为 `stage1`。

### `/apps`

返回一列 `data`，每行是一个应用的 5 个字段，以 `\t` 分隔：

```
packageName \t label \t versionName \t system(1/0) \t sourceDir
```

示例：

```bash
adb shell content query --uri content://com.phonetoolbox.agent.agentinfo/apps
```

注意：

- 需要 `android.permission.QUERY_ALL_PACKAGES`；
- HyperOS / MIUI 13+ 额外受 `com.android.permission.GET_INSTALLED_APPS`（MIUI AppOps `10022`）控制：
  ```bash
  adb shell appops set com.phonetoolbox.agent 10022 allow
  ```
  只执行 `pm grant` 在部分 HyperOS 上不够；
- Provider 不返回配对码、私钥、peers 公钥或任意文件内容。

## 2. Root 代理协议

### APK / agent

- 固定包名 `com.phonetoolbox.agent`，APK 启动后自动拉起 native agent；
- agent 只监听 `127.0.0.1`，端口随机；
- 桌面端通过 `adb forward` 访问；
- 桌面端可通过广播后台拉起 agent（不打开 Activity）：
  ```bash
  adb shell am broadcast -a com.local.service.START_AGENT \
    -n com.phonetoolbox.agent/com.local.service.AgentBootReceiver
  ```
- Android 1.0–4.4：使用 `android/legacy-agent` 的 legacy APK，没有 Activity/Provider，固定监听 `127.0.0.1:32145`。桌面端广播传入配对码，由 APK 写入启动脚本：
  ```bash
  adb shell am broadcast -a com.phonetoolbox.agent.START -e pair_code 123456
  ```

### 传输与配对

- 会话：`Noise_IK_25519_ChaChaPoly_SHA256`，prologue `PhoneToolbox/root-agent/1`；
- 首次配对：CPace + APK 界面显示的 6 位码（legacy APK 由桌面端生成并传入）；
- 帧类型：`0x01` JSON、`0x02` 二进制块、`0x10` 配对控制、`0x11` Noise 握手、`0x12` Noise 传输；
- 配对后 peers 保存在 APK 私有目录 `peers.txt`；每台电脑一行；
- 会话握手会从 msg1 读取真实发起方公钥，必须命中 `peers.txt`；
- 每条命令前检查 peer 仍在 `peers.txt`，删除配对会立即吊销现有会话；
- **配对即完整授权**：配对成功后，这台电脑可以以 root 身份执行任意命令、读写任意文件，没有按应用、按命令或按操作的细粒度权限隔离；只与可信电脑配对；
- 配对码、CPace 密钥和确认 MAC 不写入日志；失败只记录 `pair: confirm MAC mismatch`。

### 控制帧 JSON op

| op | 主要字段 | 说明 |
|----|----------|------|
| `ping` | `id` | 返回 `pong` |
| `exec` | `id`、`argv` 或 `shell`+`command`、`timeout_ms` | 执行命令，返回 `stdout` / `stderr` / `exit` |
| `push` | `path`、`size`、`mode` | 上传文件，随后发二进制帧 |
| `pull` | `path` | 下载文件，先返回 `pull_meta`，再发二进制帧 |
| `stat` | `path` | 返回文件状态 |
| `shutdown` | — | 停止 agent |

### agent CLI

```text
phone_agent --port 0 --state-dir DIR --audit PATH --pair-code CODE \
  [--io-timeout-ms N]
phone_agent --list-peers --state-dir DIR
phone_agent --remove-peer HEX --state-dir DIR
```

`--io-timeout-ms` 限制单个连接的读/写等待（默认 10000ms）；未认证连接和空闲会话不会无限占用线程。

`--list-peers` 每行输出：

```
PEER=<64 hex>\t<name>\t<capabilities>
```

`--remove-peer` 删除指定公钥；成功输出 `REMOVED=1`。

### 配置项

- `--state-dir`：`identity.key` / `peers.txt` 目录；
- `--audit`：审计日志路径；
- `--pair-code`：配对码；
- `--allow-plaintext` / `--token`：仅开发测试，生产 APK 不使用。

## 3. 桌面端 C++ 接口

### RootAgentClient

```cpp
RootAgentClient client;
client.setSerial(serial);
client.setIdentity(clientPrivateHex, agentPublicHex);
client.startForward(adbPath, devicePort, &error);
client.connectToAgent();

client.exec({"id"}, 5000);
client.execShell("id", 5000);
client.detectRoot();
```

信号：

- `connected()` / `disconnected()` / `errorOccurred(QString)`
- `execFinished(int exitCode, QString stdout, QString stderr, bool timedOut)`
- `rootDetected(bool isRoot, QString detail)`
- `pairingFinished(bool ok, QString clientPrivateHex, QString agentPublicHex, QString error)`

### RootShell

统一 root 执行入口：

```cpp
RootShellResult result =
    RootShell::instance().run(serial, "id", 5000);
```

- 有已连接代理：走 Noise 会话；
- 没有代理：回退 `adb shell su -c`；
- `usedAgent` 表示本次是否走代理；
- 代理连接但执行失败时不会偷偷回退 adb su。

面板通过 `registerExecutor()` 注册代理执行器；其它模块只需要调用 `run()`。

### RootAgentInstaller

```cpp
RootAgentInstaller installer;
installer.start(serial, adbPath);
```

安装固定包名 `com.phonetoolbox.agent` 的已签名模板 APK，然后打开一次 APK 显示配对码。不再重新生成随机包名，也不依赖 Gradle/apktool。

## 4. 插件 API

插件是 Qt `QPluginLoader` 加载的 `ProtocolPlugin` 实现。

### 接口

`src/plugins/plugin_interface.h`：

```cpp
class ProtocolPlugin : public QObject {
public:
    virtual QString name() const = 0;
    virtual QString description() const = 0;
    virtual QStringList capabilities() const = 0;
    virtual bool execute(const QString &capability,
                         const QVariantMap &params,
                         QString *error) = 0;
};
```

IID：`com.phonetoolbox.ProtocolPlugin/1.0`

### 最小插件

```cpp
class MyPlugin : public ProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ProtocolPlugin_iid)
    Q_INTERFACES(ProtocolPlugin)
public:
    QString name() const override { return "My Plugin"; }
    QString description() const override { return "..."; }
    QStringList capabilities() const override { return {"my.capability"}; }
    bool execute(const QString &capability, const QVariantMap &params,
                 QString *error) override;
};
```

CMake 构建为 `MODULE`：

```cmake
add_library(my_plugin MODULE my_plugin.cpp)
target_link_libraries(my_plugin PRIVATE Qt6::Core)
set_target_properties(my_plugin PROPERTIES AUTOMOC ON)
```

### 加载目录

启动时按顺序查找，找到第一个存在的目录即停止：

1. `$PHONETOOLBOX_PLUGIN_DIR`
2. `<exe>/plugins`
3. `<exe>/../lib/phonetoolbox/plugins`
4. `<exe>/../lib64/phonetoolbox/plugins`

`PluginManager::scanPlugins()` 扫描 `.so` / `.dll` / `.dylib`，`records()` 会保留加载失败的文件和原因。插件面板只展示加载状态，不提供通用“执行”按钮——能力由对应业务流程调用。

## 5. 漏洞库 API

漏洞库是本地 JSON，格式见 [vulndb.md](vulndb.md)。对应 C++ 结构：

- `VulnEntry`：`id`、`summary`、`severity`、`cvssScore`、`affected`、`exploit`、`uiHtml`、`uiLua`；
- `AffectedRange`：`androidVersionMin/Max`、`patchLevelBefore`、`sdkMin/Max`、`platforms`；
- `ExploitScript`：`detect` / `exploit` / `verify` 三段 shell 脚本、`requiresRoot`、`requiresFilePush`、`needsReboot`、`timeoutSeconds`、`payloadFiles`；
- `ExploitEngine::runFull()`：检测 → 利用 → 验证；
- `ExploitEngine::runDetectionBatch()`：批量检测；
- 结果状态：`UNKNOWN` / `VULNERABLE` / `NOT_VULNERABLE` / `EXPLOITED` / `FAILED`。

`requiresRoot` 由设备 Root 状态门控；脚本通过 ADB shell 执行。BROM 等非 CVE 条目使用明确名称，并在 UI 中标注“非 CVE”。

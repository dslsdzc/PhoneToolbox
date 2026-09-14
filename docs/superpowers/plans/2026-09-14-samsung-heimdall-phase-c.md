# 三星 Heimdall(Odin) 集成 Phase C 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** PhoneToolbox 能对三星机型按 PIT 刷写真实固件包（BL/AP/CP/CSC 的 `.tar.md5`）：离线可验的 PIT 解析 + 计划层全绿，设备侧握手/读 PIT/逐分区写入写到「代码就绪 + mock 验证」，真机验证归持机人。

**Architecture:** 六层分离（与 Phase B 同构）——`pit`（纯解析）/ `samsung_plan`（纯计划）/ `odin_protocol`（纯帧构造与响应解析）/ `odin_session`（顺序、时机、字节）/ `IOdinTransport`（唯一设备缝）/ `odin_libusb_transport`（真机 libusb）。协议模块只见 `IOdinTransport`，真机依赖被关在一个文件里；除传输层外全部可用单测钉死。

**Tech Stack:** C++17 / Qt6（Core + Widgets + Test）/ CMake + Ninja / libusb-1.0 / 复用既有 `src/image_engine/tar_image.{h,cpp}`。

**Spec:** `docs/superpowers/specs/2026-09-14-samsung-heimdall-design.md`（已批准）
**事实依据:** `docs/superpowers/specs/samsung-odin-facts.md`（参照路径/行号/URL/sha256 逐条带出处）

---

## Global Constraints

**范围与诚实边界（spec §6/§8，写进交付物与功能清单）**
- **真机全链未验证**：USB 时序、CDC_DATA 类匹配的实际枚举、真实 ACK、bootloader 是否接受未签名镜像 —— 一律不得声称已验证。
- **设备侧读 PIT（dump）无真机可验** —— 只到「代码就绪 + mock」。
- **repartition 不做**（破坏性，需专门设计）；**不实现 PIT 回写**（`0x65/0x00` 上传路径：Heimdall 与 odin4 的包结构互相不兼容，见事实报告 §B；本期既然不做 repartition，就不碰）。
- **`attributes` / `updateAttributes` 不做语义解释**（三方解读不一致；真数据里 `updateAttributes` 出现 `5`，与 Heimdall 的 `fota=1/secure=2` 也对不上）→ 只透传原值。
- **`deviceType` 不裁枚举**：`0..3` 老枚举与 `8`(UFS) 一律按数值透传。
- **FU**S 下载/解密不做（`.enc2`/`.enc4` 属 samloader 那族）。

**参照与许可**
- 主参照 `reference/heimdall`（MIT，v1.4.2）；第二参照 `reference/odin4-llucs`（Apache-2.0，在用实现）；第三参照 `reference/thor`（MPL-2.0）。
- **自研 + 参照核对，不整文件复制**；每个常量/布局都要在注释里带出处（文件:行号）。
- `reference/` 全目录 **gitignored、不进构建、不进提交**；真样本 `reference/samsung-samples/` 同样不进提交。

**三方冲突裁定表（本计划**强制约束**，实现必须照此，注释必须并列标注分歧）**

| # | 分歧点 | Heimdall | odin4 | Thor | 本期采用 | 依据 |
|---|---|---|---|---|---|---|
| D1 | 控制包长度 | 1024（`OutboundPacket(1024)` 全量发） | 1024 | 1024 | **1024 零填充** | 三方一致 |
| D2 | 握手读长 | 读 4 字节 | 读 ≤512，取前 4 判 `LOKE` | 读 4 字节 | **读 4 字节**（随后一次非阻塞 drain） | 2:1；drain 防端点残留（odin4 同款防御） |
| D3 | `0x64/0x00` payload@8 | 无（全零） | `0x7FFFFFFF` | `int.MaxValue` | **`0x7FFFFFFF`** | 2:1 |
| D4 | `0x64/0x00` 应答语义 | `code != 0` → 协商 | `version = (code>>16)&0x7FFF`；bit15=压缩 | 同 odin4 | **按 version 分支** | 2:1；Heimdall 的「非零即协商」在 version≤1 的设备上会错发 1 MiB |
| D5 | 总字节宽度 | u32 | u64 | u64 | **u64** | 2:1（三者对 <4GiB 字节兼容，u64 覆盖更大包） |
| D6 | 控制包后的空传输（ZLP） | 默认每包后 + 数据段前 | 无 | 无 | **不发** | 2:1 |
| D7 | 结束序列命令前的空传输 | before+after | before | 无 | **发 before，不发 after** | 2:1（before）；after 仅 Heimdall |
| D8 | 结束序列命令后的空读 | 不读 | 读（仅告警） | 不读 | **不读** | 2:1 |
| D9 | 数据分片末片 | 零填充到整包 | 零填充 | 零填充 | **零填充到整包** | 三方一致 |
| D10 | 结束序列 payload@8.. | phone: dest,size,unknown1(0),dt,id,isLast；modem: dest,size,unknown1,dest,dt,isLast | @16=binaryType；modem isLast@28 | phone 同 odin4；modem isLast@24 | **合并布局**（见 Task 5 的 `frameEndSequence`）：8=dest、12=realSize、16=binaryType、20=deviceType、24=(phone?id:isLast)、28=(phone?isLast:0)、32=efsClear、36=bootUpdate | phone 三方一致；modem 的 @16 取 2:1（binaryType），@24 取 2:1（isLast = Heimdall+Thor） |
| D11 | ACK 语义 | 仅比对 8 字节类型 | id/code；`0xFFFFFFFF`=失败；`code<0` 失败（`-7..-2` 为可放行进度码） | `buf[0]==0xFF` 失败 + 同款 code 文案 | **id 回显 + `0xFFFFFFFF` 失败 + `code<0` 失败；结束序列允许 `-7..-2`** | 2:1（odin4+Thor）；Thor 的「首字节 0xFF」更宽，注释并列 |
| D12 | PIT 分片 | 500 | 500 | 500 | **500** | 三方一致 |
| D13 | `0x64/0x01` | 用于 info（`RequestDeviceType`） | 机型查询（≥12B）**且** ResetFlashCount 同码 | ResetFlashCount（刷完发） | **仅做一次 best-effort 机型查询（只记原始值），刷完不发重置** | 语义冲突（odin4 自己两次调用含义不同）；冲突本身记入文档，交持机人裁定 |
| D14 | 大分区 512 对齐 | 无 | `realSize` 上取整到 512 | 无 | **不做** | 2:1 |
| D15 | `lu_count`（头 24..25） | 当 padding | 读出 | 读出 | **读出但只记不拒** | 真数据：`J1POP3G.pit` = 0、`SM-Q7MQ` = 4 → **不得**断言「恒非 0」 |

**工程约定（沿用既有惯例，违反会被审查打回）**
- 命名空间 `odin`（与 `edl` 平级）；纯函数签名一律 `bool f(...) + QString *error`。
- 遍历 Qt 容器防 detach 用 **`std::as_const`**（`#include <utility>`）—— **不要用 `qAsConst`**：Qt 6.6 起弃用（实测每处 1 条 `-Wdeprecated-declarations`），且本仓既有风格是 `std::as_const`（`src/ui/fs_browser_dialog.cpp:252`、`src/ui/image_tool_panel.cpp:794`）。
- **传输层是纯字节管道**：不补 ZLP、不做协议判断 —— 每条规则只在一个地方负责（Phase B 教训：命令帧 ZLP 曾两处注释互相推诿）。
- 新增 `src/core/odin/*.{h,cpp}` 由 `GLOB_RECURSE src/core/*.cpp` **自动**编入主程序（CMakeLists.txt:107-112）→ 主程序侧**不需要**改 CMake。
- 单测：每个测试源独立可执行；新测试要**显式**加进 `CMakeLists.txt` 的 `IMAGE_TEST_SOURCES`，并在 `foreach` 里按 `_test_stem` 追加 `_test_extra_sources`（放被测源；`src/core` 不在任何静态库内）。
- 真样本路径经**编译期宏** `ODIN_SAMPLES_DIR` 传入（其值来自 **CACHE 变量** `PT_ODIN_SAMPLES_DIR`，便于做"指向不存在目录"的负控制），用例内 **`QSKIP` 兜底**（默认）—— **绝不**把 `reference/` 内容提交，也不让用例硬编码相对路径。
  ⚠️ **`QSKIP` 不改变退出码 → ctest 照样报绿**：真样本"最强的离线证据"会在缺 `reference/` 的机器（新克隆/CI）上**静默蒸发**。故加开关 **`ODIN_SAMPLES_REQUIRED=ON`** → 缺失/不足时 `QFAIL` 而非 `QSKIP`；**本计划的验证跑一律带 `-DODIN_SAMPLES_REQUIRED=ON`**，且看输出里 `0 skipped`（不要只看 ctest 绿灯）。Task 4 的 `test_samsung_plan` 用同一套机制。
- 提交：**只 `git add` 具体文件路径**。仓库有 4 个遗留 tracked 文件在 `build/` 下，`git add -A` / `-u` / `commit -a` 一律禁止。
- 测试输出必须干净；全量 `ctest` 不得回归（当前基线 **39/39**）。

**真样本事实（本计划多处断言依据，已由控制方逐字节复核）**
- 10 个真 PIT：`reference/samsung-samples/*.pit`（9 个）+ `reference/samsung-samples/sm-j110h/J1POP3G.pit`（从 CSC 包内解出，5012B，30 条目，尾部 1024B，`lu_count=0`，`cpu_bl_id='SPRD8735'`）。
- 3 个真 tar.md5（`reference/samsung-samples/sm-j110h/`，条目名已核实）：
  - `BL_…_REV02_user_low_ship.tar.md5` → `spl.img`(32768) `sboot.bin`(1573888) `sboot2.bin`(1573888) `param.lfs`(634880)；footer = `<hex>␣␣BL_…`
  - `CSC_ODD_…_REV02_user_low_ship.tar.md5` → `J1POP3G.pit`(5012) `cache.img`(18903312) `hidden.img`(8057072)；footer = `<hex>␣␣CSC_…`
  - `MODEM_…_REV00.tar.md5` → `SPRDCP.img`(8388608) `SPRDDSP.img`(2097152) `nvitem.bin`(181868)；footer = **`<hex>␣*MODEM_…`**（二进制模式）
- `J1POP3G.pit` 条目与包内镜像逐条对应：`BOOT/spl.img`、`BOOT2/spl2.img`、`SBOOT/sboot.bin`、`SBOOT2/sboot2.bin`、`WDSP/SPRDDSP.img`、`MODEM/SPRDCP.img`、`wfixnv2/nvitem.bin`、`PARAM/param.lfs`、`CSC/cache.img`、`HIDDEN/hidden.img`、`USERDATA/userdata.img`(fota=`remained`)；`deviceType` 全 = 2(MMC)；`attributes` = 0x5（BOOT/BOOT2 为 0x2）；`PIT` 条目的 `flashFilename` = **`J1POP3G_LTN_OPEN.pit`**（与包内 `J1POP3G.pit` 不等 —— 已知反例）。
- `samsung-sm-j110h-j1xlte-LSI3475.pit`（3732B，26 条目，尾部 272B）：`flashFilename` 出现字面量 **`-`**（表示无镜像文件），`USERDATA.fotaFilename` = `b'remained\r\n'`（**真 CRLF**）。

---

## 文件结构

```
新增
  src/core/odin/pit.{h,cpp}                  PIT 解析（纯函数）+ 字符串清洗
  src/core/odin/samsung_plan.{h,cpp}         计划层：tar.md5 索引 + PIT → SamsungPlan
  src/core/odin/odin_protocol.{h,cpp}        协议常量/帧构造/响应解析（纯函数）
  src/core/odin/odin_transport.h             IOdinTransport（唯一设备缝）
  src/core/odin/odin_session.{h,cpp}         会话编排（握手→读PIT→对账→逐条目写入）
  src/core/odin/odin_libusb_transport.{h,cpp}真机传输（CDC_DATA 类匹配 + PID 兜底）
  src/ui/plan_preview_widget.{h,cpp}         计划预览通用控件（从 FlashPlanDialog 抽出）
  src/ui/samsung_plan_dialog.{h,cpp}         三星计划预览对话框（同族）
  tests/mock_odin_transport.h                mock 传输（脚本化响应 + 写字节全记录）
  tests/odin_test_helpers.h                  共享夹具（合成 PIT / 合成 tar）
  tests/test_pit.cpp
  tests/test_samsung_plan.cpp
  tests/test_odin_protocol.cpp
  tests/test_odin_session.cpp
  tests/test_odin_libusb_transport.cpp
修改
  src/image_engine/tar_image.{h,cpp}         Task 1 footer 两变体修复；Task 2 流式条目索引
  tests/test_tar.cpp                         Task 1/2 用例
  src/core/device_detector.{h,cpp}           MODE_SAMSUNG_ODIN + 检测
  src/core/flash_tool.{h,cpp}                第 5 通道 samsung-odin
  src/ui/flash_panel.cpp                     模式分支 + 「刷入」通道分派
  src/ui/flash_plan_dialog.{h,cpp}           改用 PlanPreviewWidget（公开 API 不变）
  tests/test_pipeline.cpp                    通道映射断言
  tests/test_flash_plan_dialog.cpp           回归（抽出控件后行为不变）
  CMakeLists.txt                             测试注册 + ODIN_SAMPLES_DIR 宏
  README.md / 功能清单.txt / docs/superpowers/specs/samsung-odin-facts.md   文档与诚实边界
```

**任务依赖**：Task 1 → 2 → 4；3 → 4；5 → 6；6 + 7 → 8 → 9；全部 → 10。**严格串行**（同一文件被多任务触碰，且 SDD 一次只派一个实现者）。

---

### Task 1: `tar_image` footer 两变体（既有静默缺口，前置修复）

**Files:**
- Modify: `src/image_engine/tar_image.cpp`（`scanMd5Footer` 约 :124-126；`verifyMd5Footer` 约 :265-267）
- Test: `tests/test_tar.cpp`（新增两个用例 + 注册）

**Interfaces:**
- Consumes: 无（既有 `imgtar` 命名空间）
- Produces: 行为变化 —— `<32hex>␣*<name>` 形态的校验行**从"未识别"变为"识别并校验"**。函数签名与返回值语义**一律不变**。

**背景（真包实测，控制方逐字节复核）**：`reference/samsung-samples/sm-j110h/` 三个真包，BL/CSC 尾部是 `<hex>␣␣<name>`（md5sum 文本模式），**MODEM 是 `<hex>␣*<name>`（二进制模式）**。当前两处扫描都硬要求 `[i+32]==' ' && [i+33]==' '`，于是 MODEM 走「无校验行 → 跳过校验」分支 —— **MD5 校验静默失效，损坏的 MODEM 包会被接受**。

- [ ] **Step 1: 写失败用例**

在 `tests/test_tar.cpp` 的类声明里加两行（紧跟 `badSizeRejected();` 之后）：

```cpp
    void md5FooterBinaryVariant();       // ␣* 分隔符（真 MODEM 包形态）
    void md5FooterBinaryVariantRejects(); // ␣* 形态下篡改必须被拒
```

在 `void TestTar::md5FooterWithTrailingNewline()` 之后追加：

```cpp
// 真包实证（reference/samsung-samples/sm-j110h/MODEM_*.tar.md5 尾部逐字节）：
// 校验行分隔符是 `␣*`（md5sum 二进制模式），BL/CSC 是 `␣␣`（文本模式）。
// 只认 `␣␣` 会让 MODEM 包落到"无校验行 → 跳过校验"分支（校验静默失效）。
void TestTar::md5FooterBinaryVariant()
{
    const QByteArray tar = buildTar();
    const QByteArray hex = QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex();
    const QByteArray withFooter = tar + hex + " *" + QByteArray("MODEM_J110HDDU0AQF1.tar") + '\n';

    // 整读接口：识别 + 校验通过
    QVERIFY(imgtar::verifyMd5Footer(withFooter));

    // 流式接口：hasFooter=true（**不是**"无校验行"）+ 校验通过
    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/modem.tar.md5");
    QVERIFY(writeFileBytes(p, withFooter));
    bool hasFooter = false;
    QString err;
    QVERIFY2(imgtar::verifyMd5FooterStream(p, &hasFooter, &err), qPrintable(err));
    QVERIFY(hasFooter);

    // 解包自动校验：␣* 形态同样走"校验通过 → 正常解包"（不是跳过校验）
    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(QDir().mkpath(outDir));
    QVERIFY2(imgtar::extractTarStream(p, outDir, {}, &err), qPrintable(err));
    QCOMPARE(readFileBytes(outDir + QStringLiteral("/test.txt")), QByteArray("hello"));

    // 判别力：同一形态下改名/改数据 → 必须被拒（证明真的在校验，不是"识别了但没算"）
    QByteArray renamed = withFooter;
    renamed[renamed.size() - 20] = char(renamed[renamed.size() - 20] ^ 0x01); // 只改校验行里的文件名
    const QString p2 = dir.path() + QStringLiteral("/modem2.tar.md5");
    QVERIFY(writeFileBytes(p2, renamed));
    QVERIFY(imgtar::verifyMd5FooterStream(p2, &hasFooter, &err)); // 文件名不参与 MD5 → 仍通过
}

void TestTar::md5FooterBinaryVariantRejects()
{
    const QByteArray tar = buildTar();
    const QByteArray hex = QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex();
    QByteArray tampered = tar;
    tampered[100] = char(tampered[100] ^ 0x01);              // 改归档数据 → 校验行不再匹配
    const QByteArray withFooter = tampered + hex + " *" + QByteArray("MODEM.tar") + '\n';

    QVERIFY(!imgtar::verifyMd5Footer(withFooter));           // 整读接口拒绝

    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/bad.tar.md5");
    QVERIFY(writeFileBytes(p, withFooter));
    bool hasFooter = false;
    QString err;
    QVERIFY(!imgtar::verifyMd5FooterStream(p, &hasFooter, &err));   // 流式接口拒绝
    QVERIFY(hasFooter);                                            // 且确实识别成了校验行
    QVERIFY(err.contains(QStringLiteral("MD5")));

    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(QDir().mkpath(outDir));
    err.clear();
    QVERIFY(!imgtar::extractTarStream(p, outDir, {}, &err));        // 解包自动校验拒绝
    QVERIFY(err.contains(QStringLiteral("MD5")));
}
```

- [ ] **Step 2: 跑一遍，确认失败**

```bash
cmake --build build --target image_engine_tests_test_tar
./build/image_engine_tests_test_tar
```
预期：`md5FooterBinaryVariant` FAIL（`verifyMd5Footer` 返回 false / `hasFooter` 为 false），`md5FooterBinaryVariantRejects` 亦 FAIL（`hasFooter` 为 false —— 说明根本没识别成校验行）。

- [ ] **Step 3: 改两处扫描**

`src/image_engine/tar_image.cpp`，`scanMd5Footer()` 内（约 :124）：

```cpp
        // 分隔符两变体（真包逐字节实证，reference/samsung-samples/sm-j110h/）：
        //   md5sum 文本模式 -> "<hex>␣␣<name>"（BL_/CSC_ 包）
        //   md5sum 二进制模式 -> "<hex>␣*<name>"（MODEM_ 包）
        // 只认 "␣␣" 会让 MODEM 包落到"无校验行 → 跳过校验"分支 —— 损坏包被静默接受。
        // 校验行名字区仍从 i+34 起（分隔符恒为 2 字节），下面的可打印 ASCII / 行尾判定不变。
        if (i + 34 > tailLen || tail.at(int(i + 32)) != ' ' ||
            (tail.at(int(i + 33)) != ' ' && tail.at(int(i + 33)) != '*'))
            continue;
```

同一文件 `verifyMd5Footer()` 内（约 :266）：

```cpp
        // 与 scanMd5Footer 同款两变体（"␣␣" / "␣*"），理由见该函数注释。
        if (i + 34 > s.size() || s[i + 32] != ' ' || (s[i + 33] != ' ' && s[i + 33] != '*'))
            continue;
```

`verifyMd5Footer` 顶部的注释块同步加一行（原文第 2 行是 `[tar]\n[32hex]  name`）：

```cpp
    // 分隔符两种：md5sum 文本模式 "␣␣" 与二进制模式 "␣*"（MODEM_*.tar.md5 实测）。
```

- [ ] **Step 4: 跑测试，确认通过**

```bash
cmake --build build --target image_engine_tests_test_tar && ./build/image_engine_tests_test_tar
```
预期：全绿（原有用例一并回归：`md5Footer` 里 `!verifyMd5Footer(tar)` —— 无校验行的裸 tar 仍必须返回 false）。

- [ ] **Step 5: 判别力自证（回滚证明，必须做并在报告里贴输出）**

把 `scanMd5Footer` 里刚加的 `&& tail.at(int(i + 33)) != '*'` 临时删掉 → 重编译 → **`md5FooterBinaryVariant` 必须 FAIL**（且失败点应是 `hasFooter` 为 false，即"根本没识别成校验行"）。贴出失败输出后**改回来**，重编译确认全绿。

- [ ] **Step 6: 提交**

```bash
git add src/image_engine/tar_image.cpp tests/test_tar.cpp
git commit -m "fix(tar): 三星 .tar.md5 校验行接受 ␣* 分隔符（真 MODEM 包曾静默跳过校验）"
```

---

### Task 2: `tar_image` 流式条目索引 `indexTarStream`

**Files:**
- Modify: `src/image_engine/tar_image.h`（新增类型与声明）、`src/image_engine/tar_image.cpp`（在 `scanMd5Footer` 所在的**同一匿名命名空间之外**实现，以便调用它）
- Test: `tests/test_tar.cpp`

**Interfaces:**
- Consumes: `scanMd5Footer(QFile&, qint64, qint64&, QString*)`（同文件匿名命名空间，返回 1/0/-1/-2）
- Produces:
  ```cpp
  struct TarIndexEntry { QString name; quint64 offset = 0; quint64 size = 0; bool isDir = false; };
  bool indexTarStream(const QString &tarPath, QList<TarIndexEntry> &out,
                      quint64 *tarEnd = nullptr, QString *error = nullptr);
  ```
  `offset` = **数据区在文件内的绝对偏移**（目录/符号链接/零长度条目也为该值）；多个任务依赖它做流式读取（Task 4 计划层、Task 6 会话数据面）。

**为什么需要**：`.tar.md5` 里含 GB 级镜像，`extractTar` 整读入内存不可用；计划层要"名字 → (偏移, 大小)"，会话数据面要按偏移分片读。**不重写 tar 解析** —— 头部解析规则与 `extractTar` 一致，仅改为"读头、跳数据"。

- [ ] **Step 1: 写失败用例**

类声明加：

```cpp
    void indexTarStreamNamesOffsetsSizes();  // 名字/偏移/大小 + tarEnd
    void indexTarStreamRejectsBadInput();    // 坏 size / 截断 / 不存在
```

追加实现：

```cpp
// 流式索引：名字 → (数据区绝对偏移, 字节数)；tarEnd = 归档区结束（不含 .tar.md5 校验行）
void TestTar::indexTarStreamNamesOffsetsSizes()
{
    QList<imgtar::TarEntry> in;
    imgtar::TarEntry a; a.name = QStringLiteral("spl.img");   a.data = pattern(3000, 11);
    imgtar::TarEntry d; d.name = QStringLiteral("sub/");      d.isDir = true;
    imgtar::TarEntry b; b.name = QStringLiteral("sboot.bin"); b.data = pattern(512, 12); // 512 整数倍
    in << a << d << b;
    const QByteArray tar = imgtar::buildTar(in);
    const QByteArray withFooter = imgtar::appendMd5Footer(tar);

    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/idx.tar.md5");
    QVERIFY(writeFileBytes(p, withFooter));

    QList<imgtar::TarIndexEntry> idx;
    quint64 tarEnd = 0;
    QString err;
    QVERIFY2(imgtar::indexTarStream(p, idx, &tarEnd, &err), qPrintable(err));
    QCOMPARE(idx.size(), 3);
    QCOMPARE(idx[0].name, QStringLiteral("spl.img"));
    QCOMPARE(idx[0].size, quint64(3000));
    QCOMPARE(idx[0].isDir, false);
    QCOMPARE(idx[1].name, QStringLiteral("sub"));
    QVERIFY(idx[1].isDir);
    QCOMPARE(idx[2].name, QStringLiteral("sboot.bin"));
    QCOMPARE(idx[2].size, quint64(512));
    // 偏移自洽：按偏移读回文件，内容与构造一致（证明偏移是"数据区起点"而不是"头块起点"）
    QCOMPARE(readFileBytes(p).mid(int(idx[0].offset), 3000), pattern(3000, 11));
    QCOMPARE(readFileBytes(p).mid(int(idx[2].offset), 512), pattern(512, 12));
    // tarEnd = 归档区结束（不含校验行）→ 其前 1024 字节是两个空块
    QCOMPARE(tarEnd, quint64(tar.size()));
    QCOMPARE(readFileBytes(p).mid(int(tarEnd) - 1024, 1024), QByteArray(1024, 0));

    // 无校验行的裸 tar：tarEnd = 文件大小
    const QString p2 = dir.path() + QStringLiteral("/idx.tar");
    QVERIFY(writeFileBytes(p2, tar));
    QList<imgtar::TarIndexEntry> idx2;
    quint64 tarEnd2 = 0;
    QVERIFY2(imgtar::indexTarStream(p2, idx2, &tarEnd2, &err), qPrintable(err));
    QCOMPARE(idx2.size(), 3);
    QCOMPARE(tarEnd2, quint64(tar.size()));

    // 校验行**不符**的包：索引照建（位置已定），完整性判定归 verifyMd5FooterStream ——
    // 这条分离是"用户刷改包"不被索引层拦死的前提（计划层报 verifyOk=false，不拒刷）
    QByteArray tampered = tar;
    tampered[600] = char(tampered[600] ^ 0x01);
    const QByteArray bad = tampered + QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex()
                           + QByteArray("  bad.tar\n");     // 校验行按**未篡改**数据算 → 必然不符
    const QString p3 = dir.path() + QStringLiteral("/bad.tar.md5");
    QVERIFY(writeFileBytes(p3, bad));
    QList<imgtar::TarIndexEntry> idx3;
    quint64 tarEnd3 = 0;
    QVERIFY2(imgtar::indexTarStream(p3, idx3, &tarEnd3, &err), qPrintable(err));
    QCOMPARE(idx3.size(), 3);
    QCOMPARE(tarEnd3, quint64(tar.size()));
    // 而完整性检查必须报"不符"
    bool hasFooter = false;
    QVERIFY(!imgtar::verifyMd5FooterStream(p3, &hasFooter, &err));
    QVERIFY(hasFooter);
}

void TestTar::indexTarStreamRejectsBadInput()
{
    QTemporaryDir dir;
    QList<imgtar::TarIndexEntry> idx;
    quint64 tarEnd = 0;
    QString err;

    // 不存在
    QVERIFY(!imgtar::indexTarStream(dir.path() + QStringLiteral("/nope.tar"), idx, &tarEnd, &err));
    QVERIFY(!err.isEmpty());

    // size 非八进制
    const QString bad = dir.path() + QStringLiteral("/bad.tar");
    QVERIFY(writeFileBytes(bad, buildTarBadSize()));
    err.clear();
    QVERIFY(!imgtar::indexTarStream(bad, idx, &tarEnd, &err));
    QVERIFY(!err.isEmpty());

    // 数据区越界（截断）
    const QByteArray whole = buildTar();
    const QString trunc = dir.path() + QStringLiteral("/trunc.tar");
    QVERIFY(writeFileBytes(trunc, whole.left(600)));         // 头块 512 + 88 字节数据
    err.clear();
    QVERIFY(!imgtar::indexTarStream(trunc, idx, &tarEnd, &err));
    QVERIFY(!err.isEmpty());
}
```

- [ ] **Step 2: 跑一遍，确认失败**

```bash
cmake --build build --target image_engine_tests_test_tar
```
预期：**编译失败**（`indexTarStream` / `TarIndexEntry` 未声明）—— 这正是"先红"。

- [ ] **Step 3: 实现**

`src/image_engine/tar_image.h`：在 `struct TarEntry` 之后加：

```cpp
// 流式条目索引（不读数据区，内存 O(1)）：offset = 该条目数据区在文件内的**绝对偏移**。
// 目录/符号链接/零长度条目的 offset 无意义（等于其数据区起点，size=0）。
struct TarIndexEntry {
    QString name;
    quint64 offset = 0;
    quint64 size = 0;
    bool isDir = false;
};

// 只读 tar 头块建立索引（`.tar.md5` 的校验行不计入；`tarEnd` 出参 = 归档区结束偏移）。
// 名字规则与 extractTar 一致（ustar 前缀字段非空时拼成 "prefix/name"，尾部 '/' 去掉）。
// 坏 size 字段 / 数据区越界 / 文件打不开 → false + 中文 *error（不返回半个索引）。
bool indexTarStream(const QString &tarPath, QList<TarIndexEntry> &out,
                    quint64 *tarEnd = nullptr, QString *error = nullptr);
```

`src/image_engine/tar_image.cpp`：在**文件末尾**（`} // namespace imgtar` 之前）实现：

```cpp
bool indexTarStream(const QString &tarPath, QList<TarIndexEntry> &out,
                    quint64 *tarEnd, QString *error)
{
    out.clear();
    QFile f(tarPath);
    if (!f.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开文件：%1").arg(tarPath));
        return false;
    }
    const qint64 fileSize = f.size();
    // 先用与解包同一条 footer 扫描确定归档区结束（同为 ␣␣/␣* 两变体，Task 1）
    qint64 archiveEnd = fileSize;
    const int scan = scanMd5Footer(f, fileSize, archiveEnd, error);
    if (scan == -2)
        return false;                    // IO/内存错误
    // scan == -1（有校验行但校验不符）**不**在此拒绝：本函数只要校验行的**位置**（据此算出归档区结束），
    // 「完整性是否通过」是 verifyMd5FooterStream 的职责（计划层的「校验」列）。这条职责分离是必须的 ——
    // 否则"用户故意刷改包"（本场景常见）会被索引层直接拦死。scanMd5Footer 在 -1 路径上同样已置好 tarEnd。
    if (tarEnd)
        *tarEnd = quint64(archiveEnd);

    qint64 pos = 0;
    QByteArray hdr;
    if (!allocFixed(512, hdr, error))
        return false;
    while (pos + 512 <= archiveEnd) {
        if (!f.seek(pos) || !readExact(&f, hdr.data(), 512)) {
            setErr(error, QStringLiteral("读取 tar 头块失败：偏移 %1").arg(pos));
            return false;
        }
        if (hdr == QByteArray(512, 0))
            break;                       // 结束块
        QByteArray nameField = hdr.left(100).split('\0').first();
        if (nameField.isEmpty())
            break;
        bool sizeOk = false;
        const qint64 size = hdr.mid(124, 12).trimmed().toLongLong(&sizeOk, 8);
        if (!sizeOk || size < 0) {
            setErr(error, QStringLiteral("tar 条目 size 字段非法（偏移 %1）").arg(pos));
            return false;
        }
        const char type = hdr[156];
        const bool isDir = (type == '5');
        // ustar 前缀字段（257 处魔数 "ustar" 且 345 处前缀非空）→ "prefix/name"
        QByteArray name = nameField;
        if (hdr.mid(257, 5) == QByteArray("ustar", 5)) {
            const QByteArray prefix = hdr.mid(345, 155).split('\0').first();
            if (!prefix.isEmpty())
                name = prefix + '/' + name;
        }
        const qint64 dataStart = pos + 512;
        if (!isDir && !(type == '2')) {           // 普通文件：数据区必须在归档区内
            if (dataStart + size > archiveEnd) {
                setErr(error, QStringLiteral("tar 条目数据越界（%1：需要 %2 字节，归档剩余 %3）")
                                  .arg(QString::fromLatin1(name)).arg(size).arg(archiveEnd - dataStart));
                return false;
            }
        }
        TarIndexEntry e;
        e.name = QString::fromLatin1(name);
        while (e.name.endsWith(QLatin1Char('/')))   // 与 extractTar 的目录名口径一致
            e.name.chop(1);
        e.offset = quint64(dataStart);
        e.size = isDir ? 0 : quint64(size);
        e.isDir = isDir;
        out.append(e);
        pos = dataStart + ((size + 511) / 512) * 512;
    }
    return true;
}
```

注意（实现者必读）：
- `scanMd5Footer` 在**同一文件的匿名命名空间**里，返回 `-1`（校验不符）时**继续**（位置已置好）—— 理由见上面代码里的注释。
- `readExact` / `allocFixed` / `setErr` 都是本文件既有的匿名命名空间助手（见文件头部），直接用。

- [ ] **Step 4: 跑测试，确认通过**

```bash
cmake --build build --target image_engine_tests_test_tar && ./build/image_engine_tests_test_tar
```
预期：全绿。

- [ ] **Step 5: 提交**

```bash
git add src/image_engine/tar_image.h src/image_engine/tar_image.cpp tests/test_tar.cpp
git commit -m "feat(tar): 流式条目索引 indexTarStream（名字/偏移/大小 + tarEnd）"
```

---

### Task 3: PIT 解析 `src/core/odin/pit.{h,cpp}`

**Files:**
- Create: `src/core/odin/pit.h`, `src/core/odin/pit.cpp`
- Create: `tests/odin_test_helpers.h`（合成 PIT 夹具；Task 4/6 复用）
- Create: `tests/test_pit.cpp`
- Modify: `CMakeLists.txt`（注册测试 + `ODIN_SAMPLES_DIR` 宏）

**Interfaces:**
- Consumes: 无（纯模块）
- Produces: `odin::PitEntry` / `odin::PitTable` / `odin::parsePit(const QByteArray&, PitTable&, QString*)` / `odin::parsePitFile(const QString&, PitTable&, QString*)` / `odin::cleanPitString(const QByteArray&)` / `PitEntry::partitionBytes()` / `PitEntry::hasImageName()` / `PitTable::findByName()` / `PitTable::indexOfName()`

**为什么是"内存字节"版为核心**：真 PIT 既可能来自磁盘文件（用户指定），也可能来自包内 `.pit` 条目（Task 4）或**设备 dump**（Task 6）—— 后两者没有独立文件。故 `parsePit(data)` 是核心纯函数，`parsePitFile` 只是读文件的外壳（spec §4 写的是 path 版签名，此处按其"纯函数可离线测"的意图下沉一层）。

- [ ] **Step 1: 写共享夹具 `tests/odin_test_helpers.h`**

```cpp
// tests/odin_test_helpers.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "core/odin/pit.h"

// 合成 PIT 夹具（共享：test_pit / test_samsung_plan / test_odin_session 都用它，勿各写一份）。
// 字段默认值取真样本 J1POP3G.pit 的 BOOT 条目形态（dt=2 MMC / attr=0x5 / upd=1）。
namespace odintest {

inline void putU32(QByteArray &d, int off, quint32 v)
{
    d[off]     = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
    d[off + 2] = char((v >> 16) & 0xFF);
    d[off + 3] = char((v >> 24) & 0xFF);
}

inline void putU16(QByteArray &d, int off, quint16 v)
{
    d[off]     = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}

inline quint32 rdU32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

struct PitSpec {
    QByteArray name;                    // 分区名（≤32；内部会 NUL 填满）
    quint32 binaryType = 0;
    quint32 deviceType = 2;
    quint32 identifier = 0;
    quint32 attributes = 0x5;
    quint32 updateAttributes = 1;
    quint32 blockSizeOrOffset = 0;
    quint32 blockCount = 1024;
    quint32 fileOffset = 0;
    quint32 fileSize = 0;
    QByteArray flashFilename;           // 可含 '\r\n'（用例故意构造）
    QByteArray fotaFilename;
};

inline QByteArray field32(const QByteArray &s)
{
    QByteArray f(32, '\0');
    const QByteArray cut = s.left(32);
    for (int i = 0; i < cut.size(); ++i)
        f[i] = cut.at(i);
    return f;
}

// 28 字节头 + N×132 字节条目 + trailing（默认 1024 字节非零，模拟真实尾部签名块）
inline QByteArray buildPit(const QList<PitSpec> &entries,
                           const QByteArray &comTar2 = QByteArray("COM_TAR2"),
                           const QByteArray &cpuBlId = QByteArray("MSM8974"),
                           quint16 luCount = 0,
                           const QByteArray &trailing = QByteArray(1024, '\x5A'))
{
    QByteArray d(28 + entries.size() * 132, '\0');
    putU32(d, 0, 0x12349876);
    putU32(d, 4, quint32(entries.size()));
    const QByteArray com = field32(comTar2);
    for (int i = 0; i < 8; ++i) d[8 + i] = com.at(i);
    const QByteArray cpu = field32(cpuBlId);
    for (int i = 0; i < 8; ++i) d[16 + i] = cpu.at(i);
    putU16(d, 24, luCount);
    putU16(d, 26, 0);
    for (int i = 0; i < entries.size(); ++i) {
        const PitSpec &s = entries.at(i);
        const int o = 28 + i * 132;
        putU32(d, o + 0, s.binaryType);
        putU32(d, o + 4, s.deviceType);
        putU32(d, o + 8, s.identifier);
        putU32(d, o + 12, s.attributes);
        putU32(d, o + 16, s.updateAttributes);
        putU32(d, o + 20, s.blockSizeOrOffset);
        putU32(d, o + 24, s.blockCount);
        putU32(d, o + 28, s.fileOffset);
        putU32(d, o + 32, s.fileSize);
        const QByteArray nm = field32(s.name);
        const QByteArray ff = field32(s.flashFilename);
        const QByteArray fa = field32(s.fotaFilename);
        for (int k = 0; k < 32; ++k) {
            d[o + 36 + k]  = nm.at(k);
            d[o + 68 + k]  = ff.at(k);
            d[o + 100 + k] = fa.at(k);
        }
    }
    return d + trailing;
}

} // namespace odintest
```

- [ ] **Step 2: 写失败用例 `tests/test_pit.cpp`**

```cpp
// tests/test_pit.cpp
//
// PIT 解析：合成夹具（始终跑）+ 真样本硬断言（reference/ 存在时才跑，否则 QSKIP）。
// 真样本事实依据：docs/superpowers/specs/samsung-odin-facts.md 第 3 节（控制方逐字节复核）。
#ifndef ODIN_SAMPLES_DIR
#define ODIN_SAMPLES_DIR ""
#endif

#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>

#include "core/odin/pit.h"
#include "odin_test_helpers.h"

class TestPit : public QObject
{
    Q_OBJECT
private slots:
    void parsesHeaderAndEntries();
    void cleansStringsWithCrlfAndNul();
    void passthroughAttributesAndDeviceTypeUntouched();
    void partitionBytesByDeviceType();
    void hasImageNameRules();
    void findByNameIsCaseInsensitive();
    void keepsTrailingSignature();
    void rejectsBadInput();
    // 真样本（reference/samsung-samples/，gitignored → 缺失时 QSKIP）
    void realSamplesParseWithKnownFacts();
};

using namespace odintest;

static QList<PitSpec> bootSbootNv()
{
    QList<PitSpec> es;
    PitSpec boot; boot.name = QByteArray("BOOT");  boot.identifier = 80; boot.flashFilename = QByteArray("spl.img");
    boot.blockCount = 1024; es << boot;
    PitSpec sboot; sboot.name = QByteArray("SBOOT"); sboot.identifier = 1; sboot.flashFilename = QByteArray("sboot.bin");
    sboot.blockCount = 4096; es << sboot;
    PitSpec nv; nv.name = QByteArray("wfixnv2"); nv.identifier = 4; nv.flashFilename = QByteArray("nvitem.bin");
    nv.blockCount = 2048; es << nv;
    return es;
}

void TestPit::parsesHeaderAndEntries()
{
    const QByteArray raw = buildPit(bootSbootNv());
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(raw, t, &err), qPrintable(err));
    QCOMPARE(t.comTar2, QByteArray("COM_TAR2"));
    QCOMPARE(t.cpuBlId, QByteArray("MSM8974"));
    QCOMPARE(t.entries.size(), 3);
    QCOMPARE(t.entries[0].partitionName, QStringLiteral("BOOT"));
    QCOMPARE(t.entries[0].identifier, quint32(80));
    QCOMPARE(t.entries[0].flashFilename, QStringLiteral("spl.img"));
    QCOMPARE(t.entries[1].partitionName, QStringLiteral("SBOOT"));
    QCOMPARE(t.entries[1].blockCount, quint32(4096));
    QCOMPARE(t.entries[2].flashFilename, QStringLiteral("nvitem.bin"));
    QCOMPARE(t.trailingBytes, quint64(1024));
    // 头部 u16 字段：与写入值一致（不是自造的默认值）
    QCOMPARE(t.luCount, quint16(0));
    odin::PitTable t2;
    QVERIFY(odin::parsePit(buildPit(bootSbootNv(), "COM_TAR2", "SM8750", 4), t2, &err));
    QCOMPARE(t2.luCount, quint16(4));
}

void TestPit::cleansStringsWithCrlfAndNul()
{
    pitSpec: ;
    QList<PitSpec> es = bootSbootNv();
    es[0].fotaFilename = QByteArray("remained\r\n");       // 真样本 j1xlte 的 USERDATA 形态
    es[1].flashFilename = QByteArray("sboot.bin\r\n");
    es[2].name = QByteArray(" wfixnv2 ");                  // 首尾空白
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QCOMPARE(t.entries[0].fotaFilename, QStringLiteral("remained"));
    QCOMPARE(t.entries[1].flashFilename, QStringLiteral("sboot.bin"));
    QCOMPARE(t.entries[2].partitionName, QStringLiteral("wfixnv2"));
    // 清洗函数本身：NUL 截断 + CR/LF 去除 + 首尾空白
    QCOMPARE(odin::cleanPitString(QByteArray("abc\r\n\0XYZ", 9)), QStringLiteral("abc"));
    QCOMPARE(odin::cleanPitString(QByteArray("\0", 1)), QString());
    QCOMPARE(odin::cleanPitString(QByteArray("  x\t", 4)), QStringLiteral("x"));
}

void TestPit::passthroughAttributesAndDeviceTypeUntouched()
{
    // 三方对 attributes 的解读互不一致 → 只透传原值（含高位）；deviceType 不裁枚举（8 = UFS 透传）
    QList<PitSpec> es = bootSbootNv();
    es[0].attributes = 0x80000001u;
    es[0].updateAttributes = 5;                            // 真数据里出现 5（Heimdall 的 fota=1/secure=2 解释不了）
    es[0].deviceType = 8;                                  // UFS（Heimdall 枚举只到 3）
    es[0].blockSizeOrOffset = 0xDEADBEEFu;
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QCOMPARE(t.entries[0].attributes, 0x80000001u);
    QCOMPARE(t.entries[0].updateAttributes, quint32(5));
    QCOMPARE(t.entries[0].deviceType, quint32(8));
    QCOMPARE(t.entries[0].blockSizeOrOffset, 0xDEADBEEFu);
}

void TestPit::partitionBytesByDeviceType()
{
    odin::PitTable t;
    QString err;
    QList<PitSpec> es = bootSbootNv();
    es[0].blockCount = 2048; es[0].deviceType = 2;         // MMC → 512B/扇区
    es[1].blockCount = 2048; es[1].deviceType = 8;         // UFS → 4096B/扇区（samloader-rs pit/src/lib.rs:155-160）
    es[2].blockCount = 0;                                  // 未声明（真样本 USERDATA 就是这样）
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QCOMPARE(t.entries[0].partitionBytes(), quint64(2048) * 512);
    QCOMPARE(t.entries[1].partitionBytes(), quint64(2048) * 4096);
    QCOMPARE(t.entries[2].partitionBytes(), quint64(0));
}

void TestPit::hasImageNameRules()
{
    odin::PitTable t;
    QString err;
    QList<PitSpec> es = bootSbootNv();
    es[1].flashFilename = QByteArray();                    // 空 = 未声明
    es[2].flashFilename = QByteArray("-");                 // 真样本 j1xlte 的字面量占位符
    es[0].name = QByteArray();                             // 无名条目：isFlashable() == false（libpit.h:107-110）
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QVERIFY(!t.entries[0].isFlashable());
    QVERIFY(t.entries[0].hasImageName());                  // 有文件名但没有分区名 → 仍需调用方自己判断
    QVERIFY(t.entries[1].isFlashable());
    QVERIFY(!t.entries[1].hasImageName());
    QVERIFY(!t.entries[2].hasImageName());                 // "-" 不是文件名
}

void TestPit::findByNameIsCaseInsensitive()
{
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(bootSbootNv()), t, &err), qPrintable(err));
    QVERIFY(t.findByName(QStringLiteral("boot")) != nullptr);
    QCOMPARE(t.findByName(QStringLiteral("boot"))->identifier, quint32(80));
    QCOMPARE(t.indexOfName(QStringLiteral("SBOOT")), 1);
    QCOMPARE(t.indexOfName(QStringLiteral("nope")), -1);
    QVERIFY(t.findByName(QStringLiteral("nope")) == nullptr);
}

void TestPit::keepsTrailingSignature()
{
    // 尾部签名长度不定（真样本 256/272/512/652/1024B）→ 只记长度、不解释、不影响解析
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(bootSbootNv(), "COM_TAR2", "MSM8974", 0,
                                    QByteArray("\x01\x02\x03", 3)), t, &err), qPrintable(err));
    QCOMPARE(t.trailingBytes, quint64(3));
    // 恰好等长（无尾）也算合法：0 字节尾部
    odin::PitTable t2;
    QVERIFY2(odin::parsePit(buildPit(bootSbootNv(), "COM_TAR2", "MSM8974", 0, QByteArray()), t2, &err),
             qPrintable(err));
    QCOMPARE(t2.trailingBytes, quint64(0));
}

void TestPit::rejectsBadInput()
{
    QString err;
    odin::PitTable t;

    // 太短（< 28 字节头）
    QVERIFY(!odin::parsePit(QByteArray(20, '\0'), t, &err));
    QVERIFY(!err.isEmpty());

    // 魔数不符
    QByteArray bad = buildPit(bootSbootNv());
    bad[0] = char(bad[0] ^ 0xFF);
    err.clear();
    QVERIFY(!odin::parsePit(bad, t, &err));
    QVERIFY(err.contains(QStringLiteral("魔数")));

    // 条目数声称 100 但文件只够 3 条（截断）
    QByteArray trunc = buildPit(bootSbootNv());
    odintest::putU32(trunc, 4, 100);
    err.clear();
    QVERIFY(!odin::parsePit(trunc, t, &err));
    QVERIFY(!err.isEmpty());

    // 条目数 0 → 拒（空计划由计划层再拦一道，但空 PIT 本身无意义）
    err.clear();
    QVERIFY(!odin::parsePit(buildPit({}), t, &err));
    QVERIFY(!err.isEmpty());

    // 文件不存在
    err.clear();
    QVERIFY(!odin::parsePitFile(QStringLiteral("/nonexistent/x.pit"), t, &err));
    QVERIFY(!err.isEmpty());
}

// ---- 真样本硬断言（本计划最强的离线证据）----
// 逐条依据：reference/samsung-samples/<file>（sha256 与来源 URL 见事实报告 §3.1/§3.2）。
void TestPit::realSamplesParseWithKnownFacts()
{
    const QString root = QString::fromLatin1(ODIN_SAMPLES_DIR);
    QDir dir(root);
    if (root.isEmpty() || !dir.exists())
        QSKIP("真样本目录不存在（reference/ 为 gitignored；见 spec §2）");

    QStringList pits;
    QDirIterator it(root, {QStringLiteral("*.pit")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        pits << it.next();
    pits.sort();
    QVERIFY2(pits.size() >= 9, qPrintable(QStringLiteral("真 PIT 少于 9 个：%1").arg(pits.size())));

    for (const QString &p : std::as_const(pits)) {
        odin::PitTable t;
        QString err;
        QVERIFY2(odin::parsePitFile(p, t, &err), qPrintable(QFileInfo(p).fileName() + ": " + err));
        QVERIFY(!t.entries.isEmpty());
        QVERIFY(!t.comTar2.isEmpty());
        // 尾部签名块：10/10 样本都非空（256..1024B）—— 要求 filesize == 28+count*132 的解析器会全灭
        QVERIFY2(t.trailingBytes > 0, qPrintable(QFileInfo(p).fileName()));
        // 每个分区名都清洗过（无 CR/LF/NUL）
        for (const odin::PitEntry &e : std::as_const(t.entries)) {
            QVERIFY(!e.partitionName.contains(QLatin1Char('\r')));
            QVERIFY(!e.partitionName.contains(QLatin1Char('\n')));
        }
        // 至少一个条目声明了文件名
        bool anyName = false;
        for (const odin::PitEntry &e : std::as_const(t.entries))
            anyName = anyName || e.hasImageName();
        QVERIFY(anyName);
    }

    // —— 具体样本的硬断言（值由控制方逐字节复核，不是转述）——
    odin::PitTable j1;
    QString err;
    QVERIFY2(odin::parsePitFile(root + QStringLiteral("/sm-j110h/J1POP3G.pit"), j1, &err), qPrintable(err));
    QCOMPARE(j1.entries.size(), 30);
    QCOMPARE(j1.cpuBlId, QByteArray("SPRD8735"));
    QCOMPARE(j1.trailingBytes, quint64(1024));
    QCOMPARE(j1.luCount, quint16(0));                       // 不是 padding，但也不保证非 0（见 SM-Q7MQ）
    QCOMPARE(j1.entries[0].partitionName, QStringLiteral("BOOT"));
    QCOMPARE(j1.entries[0].flashFilename, QStringLiteral("spl.img"));
    QCOMPARE(j1.entries[0].identifier, quint32(80));
    QCOMPARE(j1.entries[0].deviceType, quint32(2));
    QCOMPARE(j1.entries[0].attributes, quint32(0x2));
    // 已知反例：PIT 条目声明的文件名是 J1POP3G_LTN_OPEN.pit，而 CSC 包内是 J1POP3G.pit
    QCOMPARE(j1.entries[2].partitionName, QStringLiteral("PIT"));
    QCOMPARE(j1.entries[2].flashFilename, QStringLiteral("J1POP3G_LTN_OPEN.pit"));
    // USERDATA：blockCount=0（未声明）+ fotaFilename="remained"
    QCOMPARE(j1.entries[29].partitionName, QStringLiteral("USERDATA"));
    QCOMPARE(j1.entries[29].blockCount, quint32(0));
    QCOMPARE(j1.entries[29].partitionBytes(), quint64(0));
    QCOMPARE(j1.entries[29].fotaFilename, QStringLiteral("remained"));

    // SM8750（UFS）：lu_count=4、全部条目 deviceType=8 —— "lu_count 恒 0""deviceType 只到 3" 都不成立
    odin::PitTable q7;
    QVERIFY2(odin::parsePitFile(root + QStringLiteral("/samsung-sm-q7mq-eur-openx-SM8750.pit"), q7, &err),
             qPrintable(err));
    QCOMPARE(q7.entries.size(), 136);
    QCOMPARE(q7.luCount, quint16(4));
    for (const odin::PitEntry &e : std::as_const(q7.entries))
        QCOMPARE(e.deviceType, quint32(8));

    // CR/LF 清洗的真证据：j1xlte 样本的 USERDATA.fotaFilename 在**文件里**是 "remained\r\n"
    const QByteArray raw = [&] {
        QFile f(root + QStringLiteral("/samsung-sm-j110h-j1xlte-LSI3475.pit"));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }();
    QVERIFY(raw.contains(QByteArray("remained\r\n")));      // 原始字节确有 CRLF
    odin::PitTable j1x;
    QVERIFY2(odin::parsePitFile(root + QStringLiteral("/samsung-sm-j110h-j1xlte-LSI3475.pit"), j1x, &err),
             qPrintable(err));
    QCOMPARE(j1x.entries.last().fotaFilename, QStringLiteral("remained"));  // 解析后已清洗
    QCOMPARE(j1x.entries[1].flashFilename, QStringLiteral("-"));            // 字面占位符
    QVERIFY(!j1x.entries[1].hasImageName());
}

QTEST_APPLESS_MAIN(TestPit)
#include "test_pit.moc"
```

注意：`cleansStringsWithCrlfAndNul()` 里第一行 `pitSpec: ;` 是**笔误占位**，实现时删掉该行（保留其余）。

- [ ] **Step 3: 跑一遍，确认失败**

先注册测试（见 Step 5 的 CMake 两处改动），然后：

```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_pit
```
预期：**编译失败**（`core/odin/pit.h` 不存在）。

- [ ] **Step 4: 实现 `src/core/odin/pit.h` / `pit.cpp`**

`src/core/odin/pit.h`：

```cpp
// src/core/odin/pit.h
//
// PIT（Partition Information Table，三星 Odin 的分区表）解析模型。
// 布局依据（逐条带出处）：
//   * reference/heimdall/libpit/source/libpit.h:43-303 —— 28 字节头 / 132 字节条目 / 小端
//   * reference/thor/TheAirBlow.Thor.Library/PIT/PitData.cs:19-54 —— 头部字段语义（COM_TAR2 / 平台名）
//   * reference/samloader-rs/pit/src/lib.rs:27-33,79-93,155-160 —— UFS 枚举（deviceType=8）与扇区换算
// 真样本实证：docs/superpowers/specs/samsung-odin-facts.md §3（9+1 个真 PIT）
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace odin {

// PIT 条目（132 字节）。**三方解读不一致的字段一律只透传原值、不做语义解释**。
struct PitEntry {
    quint32 binaryType = 0;         // +0   0=AP / 1=CP（libpit.h:55-80；Heimdall FlashAction.cpp:253 按它选 phone/modem）
    quint32 deviceType = 0;         // +4   0=OneNand/1=File(FAT)/2=MMC/3=All（libpit.h:66-72）；**8=UFS** 仅 samloader-rs
                                    //      有（pit/src/lib.rs:79-93）→ 按数值透传，**不裁枚举**
    quint32 identifier = 0;         // +8   分区 ID（结束序列包要用；Heimdall FlashAction.cpp:274）
    quint32 attributes = 0;         // +12  **不做位域解释**：Heimdall(1=Write/2=STL)/Thor 枚举下标/libmagic(attr&2) 互不一致
    quint32 updateAttributes = 0;   // +16  同上：Heimdall 说 fota=1/secure=2，真数据却出现 5 → 只透传
    quint32 blockSizeOrOffset = 0;  // +20  libpit.h:162："不同设备的不同 Loke 版本解释不同" → 不解释
    quint32 blockCount = 0;         // +24  分区扇区数（**0 = 未声明**，真样本 J1POP3G 的 USERDATA 即如此）
    quint32 fileOffset = 0;         // +28  Obsolete（libpit.h:93）
    quint32 fileSize = 0;           // +32  Obsolete（libpit.h:94）
    QString partitionName;          // +36  char[32]
    QString flashFilename;          // +68  char[32] "USB flash filename"；真数据含字面量 "-"（= 无镜像）
    QString fotaFilename;           // +100 char[32] FOTA；真数据 "remained" / "remained\r\n"

    // 分区字节数：扇区单位按 deviceType 换算（samloader-rs pit/src/lib.rs:155-160：UFS→4096B、其余→512B）。
    // blockCount == 0 时返回 0（分区大小未声明 —— 真样本存在这种条目）。
    quint64 partitionBytes() const;
    // libpit.h:107-110：有分区名才算"可刷写条目"。
    bool isFlashable() const { return !partitionName.isEmpty(); }
    // 是否声明了"要写的镜像文件"：flashFilename 非空且不是占位符 "-"。
    bool hasImageName() const;
};

struct PitTable {
    QByteArray comTar2;        // 头 8..15：ASCII "COM_TAR2"（Thor PitData.cs:24-26）
    QByteArray cpuBlId;        // 头 16..23：ASCII 平台名，NUL 填充（真数据 "MSM8974"/"SPRD8735"/"LSI3475"…）
    quint16 luCount = 0;       // 头 24..25 —— **不当 padding**（Heimdall 读成 unknown7 的半个 u16）。
                               // 真数据 J1POP3G=0 / SM-Q7MQ(SM8750)=4 ⇒ **只记不拒**，不得断言恒非 0。
    quint16 reserved = 0;      // 头 26..27
    quint64 trailingBytes = 0; // 28+count*132 之后的尾部长度（签名块，长度不定；**原样收不解释**，只记长度）
    QList<PitEntry> entries;

    // 按分区名查（**大小写不敏感**，返回首个命中）；会话对账用（设备 PIT 优先）。
    const PitEntry *findByName(const QString &name) const;
    int indexOfName(const QString &name) const;    // -1 = 无
};

// 清洗 PIT 的 char[32] 字段：截首个 NUL + 去 CR/LF + 去首尾空白。
// 真数据含字面 CR/LF（j1xlte 的 fotaFilename = b'remained\r\n'）与 NUL 填充。
QString cleanPitString(const QByteArray &field);

// 纯函数：从**内存字节**解析 PIT（真 PIT 也可能来自包内 .pit 条目或设备 dump —— 后两者没有独立文件，
// 故核心是字节版，读文件只是外壳）。成功时清空并填充 out。
// 失败（太短 / 魔数不符 / 条目数 0 / 长度不足声明条目数）→ false + 中文 *error。
bool parsePit(const QByteArray &data, PitTable &out, QString *error);
// 读文件 + parsePit（用户显式指定 PIT 文件时用）。
bool parsePitFile(const QString &path, PitTable &out, QString *error);

} // namespace odin
```

`pit.cpp`：

```cpp
// src/core/odin/pit.cpp
//
// PIT 解析（纯函数，无 IO 依赖；parsePitFile 只是薄外壳）。
// 布局依据：reference/heimdall/libpit/source/libpit.h:43-303（头 28B / 条目 132B / 小端）、
// reference/thor/TheAirBlow.Thor.Library/PIT/PitData.cs:19-54（头部字段语义）、
// reference/samloader-rs/pit/src/lib.rs:27-33,79-93,155-160（UFS 枚举与扇区换算）。
// 真样本实证：docs/superpowers/specs/samsung-odin-facts.md §3.3（9+1 个 PIT 的魔数/条目数/尾部/CRLF）。
#include "pit.h"

#include <QFile>

namespace odin {
namespace {

constexpr quint32 kPitMagic = 0x12349876u;
constexpr int kHeaderSize = 28;
constexpr int kEntrySize = 132;

quint32 rdU32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off)))
         | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16)
         | (quint32(quint8(d.at(off + 3))) << 24);
}

quint16 rdU16(const QByteArray &d, int off)
{
    return quint16(quint8(d.at(off)) | (quint16(quint8(d.at(off + 1))) << 8));
}

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

QString hex32(quint32 v)
{
    return QStringLiteral("0x%1").arg(v, 8, 16, QLatin1Char('0'));
}

} // namespace

QString cleanPitString(const QByteArray &field)
{
    // 真数据形态（事实报告 §3.3.9）：NUL 填充 + 字面 CR/LF（"remained\r\n"）。
    QByteArray cut = field;
    const int nul = cut.indexOf('\0');
    if (nul >= 0)
        cut.truncate(nul);
    cut.remove('\r');
    cut.remove('\n');
    return QString::fromLatin1(cut).trimmed();
}

quint64 PitEntry::partitionBytes() const
{
    // 扇区单位按 deviceType 换算（samloader-rs pit/src/lib.rs:155-160：MMC→512、UFS→4096）；
    // deviceType 只到 3 的老枚举一律按 512（Heimdall 的 OneNand/File/MMC/All 都是这块老语境）。
    const quint64 unit = (deviceType == 8) ? 4096 : 512;
    return quint64(blockCount) * unit;
}

bool PitEntry::hasImageName() const
{
    // 真数据里 "-" 是占位符（samsung-sm-j110h-j1xlte-LSI3475.pit 的 PIT/MD5HDR/BOTA0/BOTA1/OTA/RESERVED2）
    if (flashFilename.isEmpty())
        return false;
    return flashFilename != QLatin1String("-");
}

const PitEntry *PitTable::findByName(const QString &name) const
{
    const int i = indexOfName(name);
    return i < 0 ? nullptr : &entries.at(i);
}

int PitTable::indexOfName(const QString &name) const
{
    for (int i = 0; i < entries.size(); ++i)
        if (entries.at(i).partitionName.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

bool parsePit(const QByteArray &data, PitTable &out, QString *error)
{
    out = PitTable{};
    if (data.size() < kHeaderSize) {
        setErr(error, QStringLiteral("PIT 数据太短（%1 字节，头部需要 %2）").arg(data.size()).arg(kHeaderSize));
        return false;
    }
    const quint32 magic = rdU32(data, 0);
    if (magic != kPitMagic) {
        setErr(error, QStringLiteral("PIT 魔数不符：读到 %1，期望 0x12349876").arg(hex32(magic)));
        return false;
    }
    const quint32 count = rdU32(data, 4);
    if (count == 0) {
        setErr(error, QStringLiteral("PIT 条目数为 0（无意义，拒绝）"));
        return false;
    }
    // 用 64 位算所需长度：count 是攻击者可控值，32 位会溢出
    const quint64 need = quint64(kHeaderSize) + quint64(count) * quint64(kEntrySize);
    if (quint64(data.size()) < need) {
        setErr(error, QStringLiteral("PIT 截断：声明 %1 条目（需要 %2 字节），实际只有 %3 字节")
                          .arg(count).arg(need).arg(data.size()));
        return false;
    }

    out.comTar2 = data.mid(8, 8);
    while (out.comTar2.endsWith('\0'))
        out.comTar2.chop(1);
    out.cpuBlId = data.mid(16, 8);
    while (out.cpuBlId.endsWith('\0'))
        out.cpuBlId.chop(1);
    // 头 24..25 = lu_count、26..27 = reserved —— **不当 padding**（Heimdall 把它们读成 unknown7/unknown8，
    // 见事实报告 §1.3.1）。真数据 J1POP3G=0 / SM-Q7MQ=4 → 只记不拒。
    out.luCount = rdU16(data, 24);
    out.reserved = rdU16(data, 26);
    // 尾部签名块：长度不定（真样本 256..1024B），**原样收、不解释**，只记长度。
    // 要求 filesize == 28+count*132 或尾部全零的解析器会在全部真样本上失败（事实报告 §3.3）。
    out.trailingBytes = quint64(data.size()) - need;

    out.entries.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        const int o = kHeaderSize + int(i) * kEntrySize;
        PitEntry e;
        e.binaryType        = rdU32(data, o + 0);
        e.deviceType        = rdU32(data, o + 4);
        e.identifier        = rdU32(data, o + 8);
        e.attributes        = rdU32(data, o + 12);
        e.updateAttributes  = rdU32(data, o + 16);
        e.blockSizeOrOffset = rdU32(data, o + 20);
        e.blockCount        = rdU32(data, o + 24);
        e.fileOffset        = rdU32(data, o + 28);
        e.fileSize          = rdU32(data, o + 32);
        e.partitionName     = cleanPitString(data.mid(o + 36, 32));
        e.flashFilename     = cleanPitString(data.mid(o + 68, 32));
        e.fotaFilename      = cleanPitString(data.mid(o + 100, 32));
        out.entries.append(e);
    }
    return true;
}

bool parsePitFile(const QString &path, PitTable &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开 PIT 文件：%1").arg(path));
        return false;
    }
    const QByteArray data = f.readAll();
    if (!parsePit(data, out, error)) {
        if (error)
            *error = QStringLiteral("%1：%2").arg(path, *error);
        return false;
    }
    return true;
}

} // namespace odin
```

- [ ] **Step 5: 注册测试（CMakeLists.txt，两处）**

① `IMAGE_TEST_SOURCES` 列表**最后一行之后**追加一行：

```cmake
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pit.cpp
```

② `foreach(_test_src IN LISTS IMAGE_TEST_SOURCES)` 里，最后一个 `elseif(_test_stem ...)` 分支之后、`endif()` 之前追加：

```cmake
                elseif(_test_stem STREQUAL "test_pit")
                    # Phase C：PIT 解析纯模块（src/core/odin 已 GLOB 编入主程序，不属任何静态库）
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp)
```

③ 真样本路径用编译期宏传入（同段 `foreach` 内，跟在 ② 之后）：

```cmake
                if(_test_stem STREQUAL "test_pit" OR _test_stem STREQUAL "test_samsung_plan")
                    # 真样本在 reference/（gitignored）：只把**路径**编进来，用例内 QSKIP 兜底
                    target_compile_definitions(${_test_target} PRIVATE
                        ODIN_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference/samsung-samples")
                endif()
```

- [ ] **Step 6: 跑测试，确认通过**

```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_pit
./build/image_engine_tests_test_pit
ctest --test-dir build --output-on-failure
```
预期：`test_pit` 全绿（真样本用例**必须真的跑**，不能 skip —— 输出里不得出现 `SKIP`）；全量 `ctest` 40/40（原 39 + 新 1）。

- [ ] **Step 7: 提交**

```bash
git add src/core/odin/pit.h src/core/odin/pit.cpp tests/odin_test_helpers.h tests/test_pit.cpp CMakeLists.txt
git commit -m "feat(odin): PIT 解析（28B 头 + 132B 条目 + 尾部签名容忍）+ 真样本硬断言"
```

---

### Task 4: 计划层 `src/core/odin/samsung_plan.{h,cpp}`

**Files:**
- Create: `src/core/odin/samsung_plan.h`, `src/core/odin/samsung_plan.cpp`
- Create: `tests/test_samsung_plan.cpp`
- Modify: `CMakeLists.txt`（注册 + 已含 `ODIN_SAMPLES_DIR` 宏）

**Interfaces:**
- Consumes: `odin::PitTable` / `odin::PitEntry`（Task 3）；`imgtar::indexTarStream` / `imgtar::verifyMd5FooterStream` / `imgtar::TarIndexEntry`（Task 1/2）
- Produces:
  ```cpp
  struct SamsungPlanFile { QString path; quint64 sizeBytes; bool md5HasFooter; bool verifyOk; QStringList entryNames; };
  struct SamsungPlanEntry { QString partition; QString imageFile; quint64 sizeBytes; quint64 sourceOffset;
                            int fileIndex; QString matchRule; PitEntry pit; };
  struct SamsungPlan { QString pitSource; QList<SamsungPlanFile> files; QList<SamsungPlanEntry> entries;
                       QStringList warnings; quint64 totalBytes; };
  bool buildSamsungPlan(const QStringList &tarMd5Files, const PitTable &pit, SamsungPlan &plan,
                        QString *error, const QString &pitSource = QString());
  bool loadPitFromPackage(const QStringList &tarMd5Files, PitTable &out, QString *pitPathOut, QString *error);
  ```
  `sourceOffset` = 镜像数据在 `files[fileIndex].path` 内的**绝对偏移**（会话数据面按它流式读）。

**与 spec §4 的两处结构差异（有意，实施时写进头文件注释）**
1. `verifyOk` 放在 `SamsungPlanFile`（每包一个结论）而不是 `SamsungPlanEntry` —— MD5 校验行的对象是**整个 tar.md5 文件**，逐条目复制同一个 bool 会误导读者以为"每条镜像各自校验过"。
2. `SamsungPlan` 用 `files`（含路径/大小/校验结论/条目名）而不是 spec 的 `tarMd5Files`(QStringList) —— 是前者的信息超集，预览对话框要显示"来源包"与校验结论。

**匹配规则（spec §4；规则本身要在注释里逐条写清）**
1. **以 PIT 条目自带的 `flashFilename` 为准**（不靠扩展名猜、不靠"分区名 + .img"推断）——真包 J1POP3G 的条目成对给出「分区名 / 文件名」且与 tar 内镜像名逐个吻合。
2. `hasImageName() == false`（空 或 字面 `-`）→ 该条目**不参与匹配、不报"缺镜像"**（它不是"缺"，是没声明）。
3. 精确匹配分区大小写不敏感。
4. 精确匹配失败且 `flashFilename` 以 `.pit` 结尾 → 在所选包内找唯一一个 `.pit` 条目（**已知反例**：PIT 条目声明 `J1POP3G_LTN_OPEN.pit`，包内是 `J1POP3G.pit`）→ 命中则记 warning 说明用了哪条规则；0 个或多个候选 → 不匹配 + 专门文案。
5. 未匹配的**有文件名**条目 → warning「PIT 条目 X 声明的镜像 Y 不在所选包内（跳过）」。
6. 包内未被任何 PIT 条目认领的条目 → warning「包内镜像 X 未出现在 PIT 中（跳过）」。
7. 大小：`镜像 > 分区` → warning（**严重：写不下**）；`镜像 < 分区` → 只计数，最后出一条汇总 warning（真数据 10 条匹配里 9 条都是"小于"——逐条报会淹没预览）；分区 `blockCount == 0`（未声明，如 J1POP3G 的 USERDATA）→ 若匹配到镜像则 warning「分区大小未声明，无法核对」。
   > ⚠️ 这条是对 spec §4「分区大小与镜像大小不符 → warning」的**收窄解释**：按字面执行会在真包上产生 9 条无意义告警（真值已复核：SPRDCP.img 恰好等于分区 8MiB，其余 8 条镜像都小于分区、属正常）。实现者在报告里**必须**写明这处收窄及理由。
8. 排序：按**包内 PIT 的条目顺序**（bootloader 在前，与 Odin 惯例一致；写入顺序不影响正确性 —— 真正决定落盘位置的是设备侧 PIT 的 identifier）。
9. 一条都没匹配上 → 失败（fail-closed，绝不放行空计划）。

- [ ] **Step 1: 写失败用例 `tests/test_samsung_plan.cpp`**

```cpp
// tests/test_samsung_plan.cpp
#ifndef ODIN_SAMPLES_DIR
#define ODIN_SAMPLES_DIR ""
#endif
#ifndef ODIN_SAMPLES_REQUIRED
#define ODIN_SAMPLES_REQUIRED 0
#endif

#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>

#include "core/odin/samsung_plan.h"
#include "image_engine/tar_image.h"
#include "odin_test_helpers.h"

class TestSamsungPlan : public QObject
{
    Q_OBJECT
private slots:
    void matchesByPitFilename();
    void skipsEntriesWithoutImageName();
    void fallsBackToUniquePitEntry();
    void warnsBothMismatchDirections();
    void warnsWhenImageLargerThanPartition();
    void summarisesSmallImages();
    void reportsMd5FooterState();
    void loadsPitFromPackage();
    void refusesEmptyResult();
    // 真包（reference/samsung-samples/sm-j110h/，缺失时 QSKIP）
    void realPackagesBuildPlan();
    void rejectsWhenNoPitInPackage();
};

using namespace odintest;

// 造一个含若干条目的 .tar.md5（磁盘上），返回路径与条目名→(偏移,大小)
static QString writeTarMd5(const QString &dir, const QString &name,
                           const QList<imgtar::TarEntry> &entries, bool withFooter = true)
{
    const QByteArray tar = imgtar::buildTar(entries);
    const QByteArray out = withFooter ? imgtar::appendMd5Footer(tar) : tar;
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size())
        return QString();
    f.close();
    return path;
}

static imgtar::TarEntry tarEntry(const QString &name, const QByteArray &data)
{
    imgtar::TarEntry e;
    e.name = name;
    e.data = data;
    return e;
}

void TestSamsungPlan::matchesByPitFilename()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A')),
                                     tarEntry(QStringLiteral("sboot.bin"), QByteArray(5000, 'B'))});
    QVERIFY(!tar.isEmpty());

    QList<PitSpec> es = bootSbootNv();                       // BOOT/spl.img、SBOOT/sboot.bin、wfixnv2/nvitem.bin
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));

    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err, QStringLiteral("包内 BL.tar.md5 的 PIT")), qPrintable(err));
    QCOMPARE(plan.pitSource, QStringLiteral("包内 BL.tar.md5 的 PIT"));
    QCOMPARE(plan.entries.size(), 2);                        // wfixnv2 的 nvitem.bin 不在包内
    QCOMPARE(plan.entries[0].partition, QStringLiteral("BOOT"));   // 顺序 = PIT 条目顺序
    QCOMPARE(plan.entries[0].imageFile, QStringLiteral("spl.img"));
    QCOMPARE(plan.entries[0].sizeBytes, quint64(3000));
    QCOMPARE(plan.entries[0].pit.identifier, quint32(80));
    QCOMPARE(plan.entries[0].matchRule, QStringLiteral("文件名精确匹配"));
    QCOMPARE(plan.entries[1].partition, QStringLiteral("SBOOT"));
    QCOMPARE(plan.totalBytes, quint64(8000));
    QCOMPARE(plan.files.size(), 1);
    QVERIFY(plan.files[0].verifyOk);
    QVERIFY(plan.files[0].md5HasFooter);
    // 偏移自洽：按 sourceOffset 从文件读回的字节 = 镜像内容
    QFile f(tar);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.seek(qint64(plan.entries[0].sourceOffset)));
    QCOMPARE(f.read(3000), QByteArray(3000, 'A'));
    // 缺镜像 → 有告警且指明了条目与文件名
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || (w.contains(QStringLiteral("wfixnv2")) && w.contains(QStringLiteral("nvitem.bin")));
    QVERIFY(warned);
}

void TestSamsungPlan::skipsEntriesWithoutImageName()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    QList<PitSpec> es = bootSbootNv();
    es[1].flashFilename = QByteArray();                      // 未声明
    es[2].flashFilename = QByteArray("-");                   // 字面占位符
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));

    odin::SamsungPlan plan;
    QVERIFY(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    // 未声明的条目**不得**产生"缺镜像"告警（只有一条汇总）
    for (const QString &w : std::as_const(plan.warnings))
        QVERIFY2(!w.contains(QStringLiteral("SBOOT")), qPrintable(w));
    bool summary = false;
    for (const QString &w : std::as_const(plan.warnings))
        summary = summary || w.contains(QStringLiteral("未声明镜像文件名"));
    QVERIFY(summary);
}

void TestSamsungPlan::fallsBackToUniquePitEntry()
{
    QTemporaryDir dir;
    // 包内 .pit 名与 PIT 条目声明的不一致（真反例：J1POP3G.pit vs J1POP3G_LTN_OPEN.pit）
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("CSC.tar.md5"),
                                    {tarEntry(QStringLiteral("J1POP3G.pit"), QByteArray(5012, 'P'))});
    QList<PitSpec> es;
    PitSpec p; p.name = QByteArray("PIT"); p.identifier = 70; p.flashFilename = QByteArray("J1POP3G_LTN_OPEN.pit");
    es << p;
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));

    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].imageFile, QStringLiteral("J1POP3G.pit"));
    QCOMPARE(plan.entries[0].matchRule, QStringLiteral(".pit 唯一性回退"));
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || w.contains(QStringLiteral("唯一"));
    QVERIFY(warned);

    // 多个 .pit 候选 → 不再回退（不擅自选一个）
    const QString tar2 = writeTarMd5(dir.path(), QStringLiteral("CSC2.tar.md5"),
                                     {tarEntry(QStringLiteral("A.pit"), QByteArray(100, 'x')),
                                      tarEntry(QStringLiteral("B.pit"), QByteArray(100, 'y'))});
    odin::SamsungPlan plan2;
    QString err2;
    QVERIFY(!odin::buildSamsungPlan({tar2}, pit, plan2, &err2));   // 一条都匹配不上 → 失败
    QVERIFY(!err2.isEmpty());
}

void TestSamsungPlan::warnsBothMismatchDirections()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A')),
                                     tarEntry(QStringLiteral("extra.bin"), QByteArray(10, 'E'))});
    QList<PitSpec> es = bootSbootNv();                        // SBOOT/sboot.bin、wfixnv2/nvitem.bin 都不在包内
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    bool missingInPkg = false, missingInPit = false;
    for (const QString &w : std::as_const(plan.warnings)) {
        if (w.contains(QStringLiteral("不在所选包内"))) missingInPkg = true;
        if (w.contains(QStringLiteral("extra.bin"))) missingInPit = true;
    }
    QVERIFY(missingInPkg);
    QVERIFY(missingInPit);
}

void TestSamsungPlan::warnsWhenImageLargerThanPartition()
{
    QTemporaryDir dir;
    // 分区 1024 扇区 × 512 = 512 KiB；镜像 600000 字节 > 512 KiB
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(600000, 'A'))});
    QList<PitSpec> es = bootSbootNv();
    es[0].blockCount = 1024;
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || w.contains(QStringLiteral("放不下"));
    QVERIFY(warned);
}

void TestSamsungPlan::summarisesSmallImages()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    QList<PitSpec> es = bootSbootNv();
    es[0].blockCount = 4096;                                  // 2 MiB 分区 vs 3000 字节镜像 → 小于
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    int perEntry = 0, summary = 0;
    for (const QString &w : std::as_const(plan.warnings)) {
        if (w.contains(QStringLiteral("放不下"))) ++perEntry;
        if (w.contains(QStringLiteral("小于分区"))) ++summary;
    }
    QCOMPARE(perEntry, 0);                                    // 小于 ≠ 逐条告警
    QCOMPARE(summary, 1);                                     // 只出一条汇总
}

void TestSamsungPlan::reportsMd5FooterState()
{
    QTemporaryDir dir;
    // 无校验行的包：verifyOk=false + 明确告警（不静默）
    const QString noFooter = writeTarMd5(dir.path(), QStringLiteral("nofooter.tar"),
                                         {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))}, false);
    QList<PitSpec> es = bootSbootNv();
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY(odin::buildSamsungPlan({noFooter}, pit, plan, &err), qPrintable(err));
    QCOMPARE(plan.files.size(), 1);
    QVERIFY(!plan.files[0].md5HasFooter);
    QVERIFY(!plan.files[0].verifyOk);
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || w.contains(QStringLiteral("校验"));
    QVERIFY(warned);

    // 校验不符的包：verifyOk=false + 告警（**不拒刷** —— 用户可能故意刷改包；门控是预览里的勾选框）
    QByteArray tar = imgtar::buildTar({tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    tar[600] = char(tar[600] ^ 0x01);
    const QByteArray bad = tar + QCryptographicHash::hash(QByteArray(tar.left(0)), QCryptographicHash::Md5).toHex()
                           + "  x.tar\n";
    const QString badPath = dir.path() + QStringLiteral("/bad.tar.md5");
    {
        QFile f(badPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(bad), qint64(bad.size()));
    }
    odin::SamsungPlan plan2;
    QString err2;
    QVERIFY2(odin::buildSamsungPlan({badPath}, pit, plan2, &err2), qPrintable(err2));
    QVERIFY(plan2.files[0].md5HasFooter);
    QVERIFY(!plan2.files[0].verifyOk);
    bool warned2 = false;
    for (const QString &w : std::as_const(plan2.warnings))
        warned2 = warned2 || w.contains(QStringLiteral("MD5"));
    QVERIFY(warned2);
}

void TestSamsungPlan::loadsPitFromPackage()
{
    QTemporaryDir dir;
    const QByteArray pitBytes = buildPit(bootSbootNv());
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("CSC.tar.md5"),
                                    {tarEntry(QStringLiteral("J1POP3G.pit"), pitBytes),
                                     tarEntry(QStringLiteral("cache.img"), QByteArray(4096, 'C'))});
    odin::PitTable pit;
    QString pitPath, err;
    QVERIFY2(odin::loadPitFromPackage({tar}, pit, &pitPath, &err), qPrintable(err));
    QCOMPARE(pit.entries.size(), 3);
    QCOMPARE(pit.entries[0].partitionName, QStringLiteral("BOOT"));
    QVERIFY(pitPath.contains(QStringLiteral("J1POP3G.pit")));
}

void TestSamsungPlan::refusesEmptyResult()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("X.tar.md5"),
                                    {tarEntry(QStringLiteral("nothing.bin"), QByteArray(10, 'x'))});
    QList<PitSpec> es = bootSbootNv();
    odin::PitTable pit;
    QString err;
    QVERIFY(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    err.clear();
    QVERIFY(!odin::buildSamsungPlan({tar}, pit, plan, &err));      // 一条都匹配不上 → 拒
    QVERIFY(!err.isEmpty());
    // 空输入
    err.clear();
    QVERIFY(!odin::buildSamsungPlan({}, pit, plan, &err));
    QVERIFY(!err.isEmpty());
    // 空 PIT
    err.clear();
    odin::PitTable empty;
    QVERIFY(!odin::buildSamsungPlan({tar}, empty, plan, &err));
    QVERIFY(!err.isEmpty());
}

void TestSamsungPlan::rejectsWhenNoPitInPackage()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    odin::PitTable pit;
    QString pitPath, err;
    QVERIFY(!odin::loadPitFromPackage({tar}, pit, &pitPath, &err));
    QVERIFY(!err.isEmpty());
}

// ---- 真包（3 个 SM-J110H 包 + CSC 内的 J1POP3G.pit）----
void TestSamsungPlan::realPackagesBuildPlan()
{
    const QString root = QString::fromLatin1(ODIN_SAMPLES_DIR);
    const QString dir = root + QStringLiteral("/sm-j110h");
    if (root.isEmpty() || !QDir(dir).exists()) {
#if ODIN_SAMPLES_REQUIRED
        QFAIL("真样本目录不存在，但本次构建要求真样本（ODIN_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本目录不存在（reference/ 为 gitignored）");
#endif
    }
    const QString bl = dir + QStringLiteral("/BL_J110HXXU0AQJ1_CL1240844_QB15258762_REV02_user_low_ship.tar.md5");
    const QString csc = dir + QStringLiteral("/CSC_ODD_J110HODD0AQF2_CL1214683_QB14017971_REV02_user_low_ship.tar.md5");
    const QString modem = dir + QStringLiteral("/MODEM_J110HDDU0AQF1_CL2068124_QB6737246_REV00.tar.md5");
    QVERIFY(QFileInfo::exists(bl) && QFileInfo::exists(csc) && QFileInfo::exists(modem));

    // ① PIT 从 CSC 包内取（真包内名 J1POP3G.pit；条目声明的是 J1POP3G_LTN_OPEN.pit → 走 .pit 唯一性回退）
    odin::PitTable pit;
    QString pitPath, err;
    QVERIFY2(odin::loadPitFromPackage({bl, csc, modem}, pit, &pitPath, &err), qPrintable(err));
    QCOMPARE(pit.entries.size(), 30);

    // ② 三个包一起构建计划：10 个镜像全部匹配（4 BL + 3 CSC + 3 MODEM）
    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({bl, csc, modem}, pit, plan, &err,
                                    QStringLiteral("包内 J1POP3G.pit")), qPrintable(err));
    QCOMPARE(plan.entries.size(), 10);
    QCOMPARE(plan.totalBytes, quint64(32768 + 1573888 + 1573888 + 634880
                                     + 5012 + 18903312 + 8057072
                                     + 8388608 + 2097152 + 181868));
    // 三个包都带校验行且校验通过（含 MODEM 的 "␣*" 变体 —— Task 1 的修复在此被真实包验证）
    QCOMPARE(plan.files.size(), 3);
    for (const odin::SamsungPlanFile &f : std::as_const(plan.files)) {
        QVERIFY2(f.md5HasFooter, qPrintable(QFileInfo(f.path).fileName()));
        QVERIFY2(f.verifyOk, qPrintable(QFileInfo(f.path).fileName()));
    }
    // 分区 ↔ 镜像配对逐个核对（真数据；10 条 = 4 BL + 3 CSC + 3 MODEM）
    QMap<QString, QString> expect;
    expect[QStringLiteral("BOOT")]     = QStringLiteral("spl.img");
    expect[QStringLiteral("SBOOT")]    = QStringLiteral("sboot.bin");
    expect[QStringLiteral("SBOOT2")]   = QStringLiteral("sboot2.bin");
    expect[QStringLiteral("PARAM")]    = QStringLiteral("param.lfs");
    expect[QStringLiteral("PIT")]      = QStringLiteral("J1POP3G.pit");   // 反例：条目声明 J1POP3G_LTN_OPEN.pit
    expect[QStringLiteral("CSC")]      = QStringLiteral("cache.img");
    expect[QStringLiteral("HIDDEN")]   = QStringLiteral("hidden.img");
    expect[QStringLiteral("MODEM")]    = QStringLiteral("SPRDCP.img");
    expect[QStringLiteral("WDSP")]     = QStringLiteral("SPRDDSP.img");
    expect[QStringLiteral("wfixnv2")]  = QStringLiteral("nvitem.bin");
    for (const odin::SamsungPlanEntry &e : std::as_const(plan.entries)) {
        QVERIFY2(expect.contains(e.partition), qPrintable(e.partition));
        QCOMPARE(e.imageFile, expect.value(e.partition));
        QVERIFY(e.sourceOffset > 0);
        QCOMPARE(e.pit.partitionName, e.partition);
    }
    // BOOT2/spl2.img 不在任何包内 → 必须有一条"不在所选包内"的告警
    bool boot2Warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        boot2Warned = boot2Warned || w.contains(QStringLiteral("spl2.img"));
    QVERIFY(boot2Warned);
    // CSC 包内的 J1POP3G.pit 自身也被当作镜像（PIT 条目的 flashFilename 是 J1POP3G_LTN_OPEN.pit）
    bool pitMatched = false;
    for (const odin::SamsungPlanEntry &e : std::as_const(plan.entries))
        pitMatched = pitMatched || e.matchRule == QStringLiteral(".pit 唯一性回退");
    QVERIFY(pitMatched);
    // 分区大小核对：真数据里只有 MODEM(SPRDCP.img) 恰好等于分区大小，其余都小于 → 只应有一条汇总告警
    int summary = 0;
    for (const QString &w : std::as_const(plan.warnings))
        summary += w.contains(QStringLiteral("小于分区")) ? 1 : 0;
    QCOMPARE(summary, 1);
    // 镜像 > 分区：真数据里一条都不该有
    for (const QString &w : std::as_const(plan.warnings))
        QVERIFY2(!w.contains(QStringLiteral("放不下")), qPrintable(w));
}

QTEST_APPLESS_MAIN(TestSamsungPlan)
#include "test_samsung_plan.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**：`cmake --build build --target image_engine_tests_test_samsung_plan` → 编译失败（`core/odin/samsung_plan.h` 不存在）。

- [ ] **Step 3: 实现 `samsung_plan.{h,cpp}`**

`samsung_plan.h`：按 Interfaces 写类型与两个函数签名，把**匹配规则 1-9 逐条**写进文件头注释（规则文本见本任务开头）。

`src/core/odin/samsung_plan.cpp`（完整实现）：

```cpp
// src/core/odin/samsung_plan.cpp
//
// 计划层：把「选中的 .tar.md5 集合 + PIT」变成可刷写清单。**只读文件**（流式索引 + 校验行），
// 不解包、不落盘。匹配规则见头文件；真包依据见 docs/superpowers/specs/samsung-odin-facts.md §3。
#include "samsung_plan.h"

#include <QFile>
#include <QFileInfo>

#include "image_engine/tar_image.h"

namespace odin {
namespace {

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

bool endsWithPit(const QString &name)
{
    return name.endsWith(QLatin1String(".pit"), Qt::CaseInsensitive);
}

struct IndexedImage {
    QString name;          // tar 内条目名（已去尾 '/'）
    quint64 offset = 0;
    quint64 size = 0;
    int fileIndex = -1;
};

} // namespace

bool loadPitFromPackage(const QStringList &tarMd5Files, PitTable &out,
                        QString *pitPathOut, QString *error)
{
    // 在所选包内找 .pit 条目。**唯一**才取 —— 多个则拒（不同 CSC 的 PIT 可能不同，不擅自选一个）。
    struct Hit { QString path; QString entryName; quint64 offset = 0; quint64 size = 0; };
    QList<Hit> hits;
    for (const QString &path : tarMd5Files) {
        QList<imgtar::TarIndexEntry> idx;
        QString ierr;
        if (!imgtar::indexTarStream(path, idx, nullptr, &ierr)) {
            setErr(error, QStringLiteral("无法索引固件包 %1：%2").arg(QFileInfo(path).fileName(), ierr));
            return false;
        }
        for (const imgtar::TarIndexEntry &e : std::as_const(idx))
            if (!e.isDir && endsWithPit(e.name))
                hits.append({path, e.name, e.offset, e.size});
    }
    if (hits.isEmpty()) {
        setErr(error, QStringLiteral("所选包内未找到 .pit（请显式指定 PIT 文件）"));
        return false;
    }
    if (hits.size() > 1) {
        QStringList names;
        for (const Hit &h : std::as_const(hits))
            names << QStringLiteral("%1（%2）").arg(h.entryName, QFileInfo(h.path).fileName());
        setErr(error, QStringLiteral("所选包内有 %1 个 .pit，无法确定用哪个：%2")
                          .arg(hits.size()).arg(names.join(QStringLiteral("、"))));
        return false;
    }
    const Hit &hit = hits.first();
    if (hit.size == 0 || hit.size > 1024 * 1024) {      // 真样本 PIT 2924..18492 B；1 MiB 上限防呆
        setErr(error, QStringLiteral("包内 PIT 大小异常（%1 字节）：%2").arg(hit.size).arg(hit.entryName));
        return false;
    }
    QFile f(hit.path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(qint64(hit.offset))) {
        setErr(error, QStringLiteral("无法读取包内 PIT：%1").arg(hit.entryName));
        return false;
    }
    const QByteArray data = f.read(qint64(hit.size));
    if (quint64(data.size()) != hit.size) {
        setErr(error, QStringLiteral("包内 PIT 数据不完整（%1：期望 %2 字节，读到 %3）")
                          .arg(hit.entryName).arg(hit.size).arg(data.size()));
        return false;
    }
    if (!parsePit(data, out, error))
        return false;
    if (pitPathOut)
        *pitPathOut = hit.path + QStringLiteral("（包内 %1）").arg(hit.entryName);
    return true;
}

bool buildSamsungPlan(const QStringList &tarMd5Files, const PitTable &pit,
                      SamsungPlan &plan, QString *error, const QString &pitSource)
{
    plan = SamsungPlan{};
    plan.pitSource = pitSource;
    if (tarMd5Files.isEmpty()) {
        setErr(error, QStringLiteral("未选择固件包（.tar.md5）"));
        return false;
    }
    if (pit.entries.isEmpty()) {
        setErr(error, QStringLiteral("PIT 无条目，无法构建刷写计划"));
        return false;
    }

    // ① 逐包：校验行状态 + 流式索引（重名首个为准 + 告警，不静默丢）
    QList<IndexedImage> images;
    for (int fi = 0; fi < tarMd5Files.size(); ++fi) {
        const QString path = tarMd5Files.at(fi);
        const QString base = QFileInfo(path).fileName();

        SamsungPlanFile f;
        f.path = path;
        f.sizeBytes = quint64(QFileInfo(path).size());

        bool hasFooter = false;
        QString verr;
        const bool verified = imgtar::verifyMd5FooterStream(path, &hasFooter, &verr);
        f.md5HasFooter = hasFooter;
        f.verifyOk = verified && hasFooter;
        if (!verified)
            plan.warnings << QStringLiteral("包校验失败（%1）：%2").arg(base, verr);
        else if (!hasFooter)
            plan.warnings << QStringLiteral("包内无 MD5 校验行，未做完整性校验：%1").arg(base);

        QList<imgtar::TarIndexEntry> idx;
        QString ierr;
        if (!imgtar::indexTarStream(path, idx, nullptr, &ierr)) {
            setErr(error, QStringLiteral("无法索引固件包 %1：%2").arg(base, ierr));
            return false;
        }
        for (const imgtar::TarIndexEntry &e : std::as_const(idx)) {
            if (e.isDir)
                continue;
            f.entryNames << e.name;
            bool dup = false;
            for (const IndexedImage &im : std::as_const(images))
                dup = dup || im.name.compare(e.name, Qt::CaseInsensitive) == 0;
            if (dup) {
                plan.warnings << QStringLiteral("包内条目重名，已忽略后者：%1（%2）").arg(e.name, base);
                continue;
            }
            images.append({e.name, e.offset, e.size, fi});
        }
        plan.files.append(f);
    }

    // ② 逐 PIT 条目匹配（规则 1-4）+ 大小核对（规则 7）
    QList<bool> claimed(images.size(), false);
    int smallImages = 0;
    int noImageName = 0;
    for (const PitEntry &pe : pit.entries) {
        if (!pe.isFlashable())
            continue;                                   // libpit.h:107-110
        if (!pe.hasImageName()) {                       // 规则 2：空 / 字面 "-" → 不是"缺"，是没声明
            ++noImageName;
            continue;
        }
        int hit = -1;
        QString rule;
        for (int i = 0; i < images.size(); ++i) {
            if (images.at(i).name.compare(pe.flashFilename, Qt::CaseInsensitive) == 0) {
                hit = i;
                rule = QStringLiteral("文件名精确匹配");
                break;
            }
        }
        if (hit < 0 && endsWithPit(pe.flashFilename)) {  // 规则 4：已知反例的回退
            QList<int> candidates;
            for (int i = 0; i < images.size(); ++i)
                if (endsWithPit(images.at(i).name))
                    candidates << i;
            if (candidates.size() == 1) {
                hit = candidates.first();
                rule = QStringLiteral(".pit 唯一性回退");
                plan.warnings << QStringLiteral("PIT 条目 %1 声明的文件名（%2）与包内不一致，"
                                                "已按 .pit 唯一性回退匹配到 %3（请核对包与机型是否配套）")
                                     .arg(pe.partitionName, pe.flashFilename, images.at(hit).name);
            } else {
                plan.warnings << QStringLiteral("PIT 条目 %1 声明的镜像 %2 不在所选包内"
                                                "（包内有 %3 个 .pit，无法按唯一性回退）")
                                     .arg(pe.partitionName, pe.flashFilename).arg(candidates.size());
                continue;
            }
        }
        if (hit < 0) {                                  // 规则 5
            plan.warnings << QStringLiteral("PIT 条目 %1 声明的镜像 %2 不在所选包内（跳过）")
                                 .arg(pe.partitionName, pe.flashFilename);
            continue;
        }
        claimed[hit] = true;

        SamsungPlanEntry e;
        e.partition = pe.partitionName;
        e.imageFile = images.at(hit).name;
        e.sizeBytes = images.at(hit).size;
        e.sourceOffset = images.at(hit).offset;
        e.fileIndex = images.at(hit).fileIndex;
        e.matchRule = rule;
        e.pit = pe;

        const quint64 partBytes = pe.partitionBytes();
        if (partBytes == 0) {
            plan.warnings << QStringLiteral("分区 %1 未声明大小（blockCount=0），无法核对镜像 %2 是否放得下")
                                 .arg(pe.partitionName, e.imageFile);
        } else if (e.sizeBytes > partBytes) {
            plan.warnings << QStringLiteral("镜像 %1（%2 字节）大于分区 %3（%4 字节），放不下 —— 请核对包与机型")
                                 .arg(e.imageFile).arg(e.sizeBytes).arg(pe.partitionName).arg(partBytes);
        } else if (e.sizeBytes < partBytes) {
            ++smallImages;                              // 规则 7 的降噪：逐条太吵，最后出一条汇总
        }

        plan.entries.append(e);
        plan.totalBytes += e.sizeBytes;
    }

    // ③ 包内未被认领的条目（规则 6）
    for (int i = 0; i < images.size(); ++i)
        if (!claimed.at(i))
            plan.warnings << QStringLiteral("包内镜像 %1 未出现在 PIT 中（跳过）").arg(images.at(i).name);

    // ④ 汇总告警（规则 2/7 的降噪版；条目顺序仍按 PIT，规则 8）
    if (noImageName > 0)
        plan.warnings << QStringLiteral("PIT 中有 %1 个条目未声明镜像文件名，已跳过").arg(noImageName);
    if (smallImages > 0)
        plan.warnings << QStringLiteral("%1 个条目的镜像小于分区（正常：剩余区域保持原样）").arg(smallImages);

    if (plan.entries.isEmpty()) {                       // 规则 9：fail-closed
        setErr(error, QStringLiteral("所选包内没有任何镜像能在 PIT 中找到对应分区（%1）")
                          .arg(pitSource.isEmpty() ? QStringLiteral("PIT") : pitSource));
        return false;
    }
    return true;
}

} // namespace odin
```

- [ ] **Step 4: 注册测试（CMakeLists 两处）**：同 Task 3 Step 5 的写法，`test_samsung_plan` 追加到列表末尾，分支加：

```cmake
                elseif(_test_stem STREQUAL "test_samsung_plan")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
```
（`ODIN_SAMPLES_DIR` 宏已在 Task 3 的 ③ 里一并覆盖本目标。）

- [ ] **Step 5: 跑测试，确认通过**

```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_samsung_plan
./build/image_engine_tests_test_samsung_plan && ctest --test-dir build --output-on-failure
```
预期：`test_samsung_plan` 全绿（真包用例必须真跑，不得 SKIP）；全量 41/41。

- [ ] **Step 6: 提交**

```bash
git add src/core/odin/samsung_plan.h src/core/odin/samsung_plan.cpp tests/test_samsung_plan.cpp CMakeLists.txt
git commit -m "feat(odin): 刷写计划层（PIT 文件名优先匹配 + .pit 唯一性回退 + 双向告警）"
```

---

### Task 5: 协议帧 `src/core/odin/odin_protocol.{h,cpp}`

**Files:**
- Create: `src/core/odin/odin_protocol.h`, `src/core/odin/odin_protocol.cpp`
- Create: `tests/test_odin_protocol.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: `odin::PitEntry`（Task 3）
- Produces: 全部常量、`TransferProfile` / `profileForVersion` / `parseAck` / `parseBeginSessionAck` / `AckVerdict` / `judgeAckIdOnly` / `judgeAck` / `frame*` 系列 / `alignedSequenceBytes`
  ```cpp
  struct TransferProfile { quint32 packetSize; int sequenceCount; int flashTimeoutMs; };
  struct Ack { quint32 id = 0; quint32 code = 0; };           // 8 字节应答的两个 u32
  struct AckVerdict { bool ok = false; QString reason; };
  struct BeginSessionAck {
      quint32 id = 0;              // 回显 id（期望 0x64；0xFFFFFFFF = BOOTLOADER_FAIL）
      quint32 code = 0;            // ⚠️ **版本号**，不是错误码：version=(code>>16)&0x7FFF、bit31=压缩支持
      quint32 version = 0;
      bool compressedSupported = false;
  };
  ```

> ⚠️ **与 spec §5 的一处不符（参照裁定，必须写进头文件注释）**：spec §5 写"结束帧（**含整包 MD5**）"，
> 但三份参照的结束序列包 32 字节里**没有任何 MD5 字段** —— 逐字段都能对上
> dest / realSize / binaryType / deviceType / identifier / isLast / efsClear / bootUpdate
> （Heimdall `EndFileTransferPacket.h` + `EndPhoneFileTransferPacket.h:79-84`；odin4 `:493-532`；Thor `:371-397`）。
> 三星的整包 MD5 校验发生在**主机侧拆包阶段**（`.tar.md5` 的校验行，Task 1/4），不在协议里。
> 本期按参照实现，**不**自造 MD5 字段；这条差异记入事实报告（Task 10）。

**三方出处（写进 `odin_protocol.cpp` 顶部注释）**
- 控制类型/请求号：`heimdall/source/ControlPacket.h:33-39`、`SessionSetupPacket.h:33-41`、`FileTransferPacket.h:35-40`、`PitFilePacket.h`（同上）、`EndSessionPacket.h:32-36`、`ResponsePacket.h:31-38`；`odin4-llucs/src/usb/odin_protocol.cpp:245-532`；`thor/TheAirBlow.Thor.Library/Protocols/Odin.cs:38-215`。
- 1024 字节零填充控制包：`ControlPacket.h:52`（`OutboundPacket(1024)`）+ `BridgeManager.cpp:711`；odin4 `:269-281`；Thor `:40-46`。
- 应答 8 字节 + `0xFFFFFFFF`/负码失败：odin4 `:328-373`（`thor_protocol.h:122` 定义 `BOOTLOADER_FAIL`）、Thor `Extensions.cs:14-29`。
- PIT 分片 500：Heimdall `ReceiveFilePartPacket.h:33`；odin4 `:640`；Thor `:223`。
- 版本 → 分片/超时：odin4 `:399-406`、Thor `:58-71`。

- [ ] **Step 1: 写失败用例 `tests/test_odin_protocol.cpp`**

```cpp
// tests/test_odin_protocol.cpp
#include <QtTest>
#include <QByteArray>

#include "core/odin/odin_protocol.h"

class TestOdinProtocol : public QObject
{
    Q_OBJECT
private slots:
    void framesAre1024ZeroPadded();
    void beginSessionPutsMaxProto();
    void totalBytesIsEightBytesLittleEndian();
    void pitFrames();
    void endSequenceLayoutPhone();
    void endSequenceLayoutModem();
    void alignedSequenceBytesRoundsUp();
    void profileByVersion();
    void parsesAcksAndFailures();
    void beginSessionAckKeepsHighVersionBits();
};

using namespace odin;

static quint32 leAt(const QByteArray &d, int off)
{
    // 测试侧独立实现（不复用被测模块的原语）：若模块端写反了字节序，这里必须能看出来
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

void TestOdinProtocol::framesAre1024ZeroPadded()
{
    // D1：三方一致 —— 控制包恒 1024 字节、前 8 字节为 type+request，其余全零
    const QByteArray f = frameControl(kControlPitFile, kPitRequestDump);
    QCOMPARE(f.size(), 1024);
    QCOMPARE(leAt(f, 0), quint32(0x65));
    QCOMPARE(leAt(f, 4), quint32(0x01));
    QCOMPARE(f.mid(8), QByteArray(1016, '\0'));

    const QByteArray g = frameControl(kControlSession, kSessionBegin, QByteArray(4, '\0') + QByteArray("\x7f\xff\xff\xff", 4));
    QCOMPARE(g.size(), 1024);
    QCOMPARE(leAt(g, 8), quint32(0x7FFFFFFF));
}

void TestOdinProtocol::beginSessionPutsMaxProto()
{
    // D3：payload@8 = 0x7FFFFFFF（odin4:377-379 / Thor:44-45）；Heimdall 发全零（1:2 未被采纳）
    const QByteArray f = frameBeginSession();
    QCOMPARE(f.size(), 1024);
    QCOMPARE(leAt(f, 0), quint32(0x64));
    QCOMPARE(leAt(f, 4), quint32(0x00));
    QCOMPARE(leAt(f, 8), quint32(0x7FFFFFFF));
    QCOMPARE(f.mid(12), QByteArray(1012, '\0'));
    // 片大小协商帧（D4：只有 version>=2 才发）
    const QByteArray s = frameFilePartSize(1048576);
    QCOMPARE(leAt(s, 0), quint32(0x64));
    QCOMPARE(leAt(s, 4), quint32(0x05));
    QCOMPARE(leAt(s, 8), quint32(1048576));
}

void TestOdinProtocol::totalBytesIsEightBytesLittleEndian()
{
    // D5：u64（odin4:454-458 / Thor:101-106）；Heimdall 的 u32 在 <4GiB 时字节相容
    const QByteArray f = frameTotalBytes(0x123456789ABull);
    QCOMPARE(leAt(f, 8), quint32(0x456789ABu));
    QCOMPARE(leAt(f, 12), quint32(0x123u));
    QCOMPARE(f.size(), 1024);
    QCOMPARE(frameTotalBytes(0x100000000ull).mid(8, 4), QByteArray(4, '\0'));
}

void TestOdinProtocol::pitFrames()
{
    QCOMPARE(leAt(framePitDumpRequest(), 4), quint32(0x01));
    const QByteArray part = framePitPartRequest(7);
    QCOMPARE(leAt(part, 0), quint32(0x65));
    QCOMPARE(leAt(part, 4), quint32(0x02));
    QCOMPARE(leAt(part, 8), quint32(7));
    QCOMPARE(leAt(framePitEndRequest(), 4), quint32(0x03));
    // 无 payload 的帧：8 字节之后全零
    QCOMPARE(framePitEndRequest().mid(8), QByteArray(1016, '\0'));
    QCOMPARE(kControlPacketSize, 1024);
    QCOMPARE(kPitPartSize, 500);
}

void TestOdinProtocol::endSequenceLayoutPhone()
{
    // D10 phone 布局（三方一致）：8=dest(0) 12=realSize 16=binaryType 20=deviceType
    //                           24=identifier 28=isLast 32=efsClear 36=bootUpdate
    PitEntry e;
    e.binaryType = 0;          // AP
    e.deviceType = 2;
    e.identifier = 80;
    const QByteArray f = frameEndSequence(e, 0x1000, true);
    QCOMPARE(f.size(), 1024);
    QCOMPARE(leAt(f, 0), quint32(0x66));
    QCOMPARE(leAt(f, 4), quint32(0x03));
    QCOMPARE(leAt(f, 8), quint32(0));          // kDestinationPhone
    QCOMPARE(leAt(f, 12), quint32(0x1000));
    QCOMPARE(leAt(f, 16), quint32(0));         // binaryType
    QCOMPARE(leAt(f, 20), quint32(2));         // deviceType
    QCOMPARE(leAt(f, 24), quint32(80));        // identifier
    QCOMPARE(leAt(f, 28), quint32(1));         // isLast
    QCOMPARE(leAt(f, 32), quint32(0));         // efsClear（本期恒 0）
    QCOMPARE(leAt(f, 36), quint32(0));         // bootUpdate（本期恒 0）
    QCOMPARE(f.mid(40), QByteArray(1024 - 40, '\0'));
    QCOMPARE(leAt(frameEndSequence(e, 0x1000, false), 28), quint32(0));
}

void TestOdinProtocol::endSequenceLayoutModem()
{
    // D10 modem：16=binaryType(=1) 20=deviceType 24=isLast（Heimdall+Thor 2:1；odin4 写 0 并在 28 写 isLast）
    PitEntry e;
    e.binaryType = 1;          // CP（Heimdall FlashAction.cpp:253 按它选 kDestinationModem）
    e.deviceType = 2;
    e.identifier = 80;         // modem 分支**不使用** identifier（Heimdall SendFile 要求 fileIdentifier=0xFFFFFFFF）
    const QByteArray f = frameEndSequence(e, 0x2000, true);
    QCOMPARE(leAt(f, 8), quint32(1));          // kDestinationModem
    QCOMPARE(leAt(f, 12), quint32(0x2000));
    QCOMPARE(leAt(f, 16), quint32(1));         // binaryType
    QCOMPARE(leAt(f, 20), quint32(2));         // deviceType
    QCOMPARE(leAt(f, 24), quint32(1));         // isLast
    QCOMPARE(leAt(f, 28), quint32(0));
    QCOMPARE(f.mid(32), QByteArray(1024 - 32, '\0'));
}

void TestOdinProtocol::alignedSequenceBytesRoundsUp()
{
    // 三方一致：序列声明值 = 片大小向上取整后的整数倍（Heimdall SendFile 的 sequenceTotalByteCount、
    // odin4 flash_partition_stream 的 aligned_size、Thor Odin.cs:333-335）；**不做 512 对齐**（D14）
    QCOMPARE(alignedSequenceBytes(0, 1048576), quint32(0));
    QCOMPARE(alignedSequenceBytes(1, 1048576), quint32(1048576));
    QCOMPARE(alignedSequenceBytes(1048576, 1048576), quint32(1048576));
    QCOMPARE(alignedSequenceBytes(1048577, 1048576), quint32(2097152));
    QCOMPARE(alignedSequenceBytes(32768, 131072), quint32(131072));
}

void TestOdinProtocol::profileByVersion()
{
    // odin4:399-406 / Thor:58-71：version<=1 → 128 KiB / 240 片 / 30s；>=2 → 1 MiB / 30 片 / 120s
    const TransferProfile small = profileForVersion(1);
    QCOMPARE(small.packetSize, quint32(131072));
    QCOMPARE(small.sequenceCount, 240);
    QCOMPARE(small.flashTimeoutMs, 30000);
    for (quint32 v : {2u, 9u, 0x7FFFu}) {
        const TransferProfile big = profileForVersion(v);
        QCOMPARE(big.packetSize, quint32(1048576));
        QCOMPARE(big.sequenceCount, 30);
        QCOMPARE(big.flashTimeoutMs, 120000);
    }
    QCOMPARE(profileForVersion(0).packetSize, quint32(131072));
}

void TestOdinProtocol::parsesAcksAndFailures()
{
    QString err;
    QByteArray raw(8, '\0');
    raw[0] = char(0x66);
    raw[4] = char(0x03);
    Ack ack;
    QVERIFY2(parseAck(raw, ack, &err), qPrintable(err));
    QCOMPARE(ack.id, quint32(0x66));
    QCOMPARE(ack.code, quint32(3));
    // 短包 → 失败
    err.clear();
    QVERIFY(!parseAck(QByteArray(7, '\0'), ack, &err));
    QVERIFY(!err.isEmpty());

    // 正常
    QVERIFY(judgeAck(ack, 0x66, false).ok);
    // id 回显不符
    QVERIFY(!judgeAck(ack, 0x65, false).ok);
    // 0xFFFFFFFF = BOOTLOADER_FAIL（odin4 thor_protocol.h:122）
    Ack fail; fail.id = 0xFFFFFFFFu; fail.code = quint32(-4);   // -4 = Write（Thor Extensions.cs:23）
    const AckVerdict v = judgeAck(fail, 0x66, false);
    QVERIFY(!v.ok);
    QVERIFY(v.reason.contains(QStringLiteral("Write")));
    // 负码：默认失败；结束序列允许 -7..-2（odin4:363-366）
    Ack neg; neg.id = 0x66; neg.code = quint32(-3);             // -3 = Erase
    QVERIFY(!judgeAck(neg, 0x66, false).ok);
    QVERIFY(judgeAck(neg, 0x66, true).ok);
    Ack bad6; bad6.id = 0x66; bad6.code = quint32(-6);          // -6 = Size
    QVERIFY(!judgeAck(bad6, 0x66, false).ok);
    QVERIFY(judgeAck(bad6, 0x66, true).ok);
    Ack bad8; bad8.id = 0x66; bad8.code = quint32(-8);          // 表外负码 → 即便放行也失败
    QVERIFY(!judgeAck(bad8, 0x66, true).ok);
    // 正码不拒（Heimdall 要求「必须为 0」，odin4/Thor 只要求非负 —— 采用后者，记日志）
    Ack pos; pos.id = 0x64; pos.code = 3;
    QVERIFY(judgeAck(pos, 0x64, false).ok);
}

void TestOdinProtocol::beginSessionAckKeepsHighVersionBits()
{
    // 0x64/0x00 的 code 字段**是版本号**，不是错误码：带压缩位（bit15 of upper half）时
    // 整个 u32 看起来是负数 —— 对它做 code<0 判定会把合法设备判成失败。
    QByteArray raw(8, '\0');
    raw[0] = char(0x64);
    raw[6] = char(0x02);                       // version = 2（高位在前）
    raw[7] = char(0x80);                       // 压缩支持位
    BeginSessionAck a;
    QString err;
    QVERIFY2(parseBeginSessionAck(raw, a, &err), qPrintable(err));
    QCOMPARE(a.version, quint32(2));
    QVERIFY(a.compressedSupported);
    QCOMPARE(a.id, quint32(0x64));
    QVERIFY(judgeAckIdOnly(Ack{a.id, a.code}, 0x64).ok);         // 只判 id → OK
    // 对照：若用通用判定（含 code<0），同一个合法应答会被判失败 —— 这正是必须分开的原因
    QVERIFY(!judgeAck(Ack{a.id, a.code}, 0x64, false).ok);

    QByteArray plain(8, '\0');
    plain[0] = char(0x64);
    plain[7] = char(0x01);                     // version = 1，无压缩位
    BeginSessionAck b;
    QVERIFY2(parseBeginSessionAck(plain, b, &err), qPrintable(err));
    QCOMPARE(b.version, quint32(1));
    QVERIFY(!b.compressedSupported);
    QVERIFY(judgeAckIdOnly(Ack{b.id, b.code}, 0x64).ok);

    // BOOTLOADER_FAIL 应答
    QByteArray bad(8, '\0');
    bad[0] = char(0xFF); bad[1] = char(0xFF); bad[2] = char(0xFF); bad[3] = char(0xFF);
    BeginSessionAck c;
    QVERIFY(parseBeginSessionAck(bad, c, &err));
    QVERIFY(!judgeAckIdOnly(Ack{c.id, c.code}, 0x64).ok);
}

QTEST_APPLESS_MAIN(TestOdinProtocol)
#include "test_odin_protocol.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**：注册 CMake（同 Task 3 Step 5 的写法：列表末尾追加 `test_odin_protocol.cpp`；分支加 `pit.cpp` + `odin_protocol.cpp`），编译 → 头文件不存在 → 红。

- [ ] **Step 3: 实现 `odin_protocol.{h,cpp}`**

`h`：常量 + 类型 + 函数声明（见 Interfaces）。
`cpp`：按 Step 1 的用例逐条实现。要点：
- 帧一律 `QByteArray(1024, '\0')` 起手，`type` 写 0..3、`request` 写 4..7、payload 写 8.. 起，**不裁剪**（D1）。
- `frameEndSequence` 严格按 D10 合并布局；modem 分支的 `isLast` 在帧偏移 24（并列注释 odin4 的 28 处写法）。
- `parseBeginSessionAck`：`version = (code >> 16) & 0x7FFF`、`compressedSupported = (code >> 31) != 0`（odin4:394-395 的 `ack_upper & 0x8000` 等价写法）。
- `judgeAck` 的负码文案表：`-2 WP / -3 Erase / -4 Write / -5 Auth / -6 Size / -7 Ext4`（Thor `Extensions.cs:19-26` 与 odin4:351-359 同）。
- 小端读写用本文件的匿名命名空间助手（与 `pit.cpp` 的同款 4 行原语各自实现，不建共享头 —— 避免把格式细节提到公共层）。

- [ ] **Step 4: 跑测试，确认通过**：`./build/image_engine_tests_test_odin_protocol && ctest --test-dir build --output-on-failure` → 全绿，42/42。

- [ ] **Step 5: 提交**

```bash
git add src/core/odin/odin_protocol.h src/core/odin/odin_protocol.cpp tests/test_odin_protocol.cpp CMakeLists.txt
git commit -m "feat(odin): 协议帧构造与应答判定（三方对照裁定：1024 控制包 / u64 总字节 / 版本化分片 / 负码文案）"
```

---

### Task 6: 传输接口 + 会话 `odin_transport.h` / `odin_session.{h,cpp}`

**Files:**
- Create: `src/core/odin/odin_transport.h`, `src/core/odin/odin_session.h`, `src/core/odin/odin_session.cpp`
- Create: `tests/mock_odin_transport.h`, `tests/test_odin_session.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: `odin::SamsungPlan`（Task 4）、`odin::PitTable`（Task 3）、`odin::frame*` / `judgeAck*` / `profileForVersion`（Task 5）
- Produces:
  ```cpp
  // odin_transport.h
  class IOdinTransport {
  public:
      virtual ~IOdinTransport() = default;
      virtual bool open(QString *error) = 0;                       // 打开 Odin 下载模式设备；close 后可再开
      virtual void close() = 0;                                    // 幂等
      virtual bool write(const QByteArray &data, QString *error) = 0;  // 裸写 OUT；**空数组 = ZLP**
      virtual QByteArray read(int maxBytes, int timeoutMs, QString *error) = 0; // 裸读 IN；0 = 非阻塞轮询；超时→空
      virtual bool reset(QString *error) = 0;                      // 句柄退场/复位（best-effort）
      virtual int  maxPacketSize() const = 0;                      // OUT 端点包长（0 = 未打开/未知）
  };
  // odin_session.h
  struct OdinOptions { bool dumpDevicePit = true; bool reboot = true; bool queryDeviceType = true;
                       int controlTimeoutMs = 10000; int handshakeTimeoutMs = 1000; };
  struct OdinProgress { QString stage; QString detail; int percent = 0; };
  using OdinProgressFn = std::function<void(const OdinProgress &)>;
  class OdinSession {
  public:
      explicit OdinSession(IOdinTransport &transport, OdinProgressFn progress = {});
      bool run(const SamsungPlan &plan, const OdinOptions &opt, QString *error);
  };
  ```
  `stage` 取值（UI 依赖这组字符串）：`"handshake" / "session" / "info" / "pit" / "validate" / "write" / "end" / "done"`。

**会话流程（spec §5；每一步的失败语义写在函数注释里）**
```
open → 握手(OUT "ODIN" / IN "LOKE") → 0x64/0x00 起会话 + 版本协商[+ 0x64/0x05 片大小]
     → 0x64/0x01 机型查询（best-effort） → 0x64/0x02 总字节
     → 0x65 PIT dump（best-effort：失败 → 告警 + 用包内 PIT）
     → 对账（**以设备 PIT 为准**；设备 PIT 缺分区 → 拒刷、一条数据都不发）
     → 逐条目：0x66/0x00 → [0x66/0x02 → 逐片数据 + 严格 partIndex ACK → 空写(D7) → 0x66/0x03 + ACK]
     → 0x67/0x00 结束会话 →（opt.reboot）0x67/0x01 重启   ← 仅成功路径
close（**所有路径都关句柄**）
```
- **失败即停、不发 0x67、不复位**（与 Phase B 同款；设备留在 Odin 模式便于重试）。Heimdall 无论成败都会发 EndSession（`FlashAction.cpp:570`）—— 本计划**不采纳**，理由写进注释。
- 收尾（0x67/0x00、0x67/0x01）失败**不判 run() 失败**，只落日志（Phase B 的 `reportsUnacknowledgedResetInsteadOfFailing` 同款口径）。

- [ ] **Step 1: 写 mock `tests/mock_odin_transport.h`**

```cpp
// tests/mock_odin_transport.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include "core/odin/odin_transport.h"

namespace odin {

// 脚本化传输：reads 队列按序出队；writes 全量记录；calls 保留调用顺序。
// "无真机可验证"的断言手段 = **写出去的字节逐条比对**（命令帧 + 数据分片），以及调用序列
// （失败路径不发 0x67、close 一定在最后、轮询用的是 timeout 0）。
// 对照先例：tests/mock_edl_transport.h（同一套建模思路）。
class MockOdinTransport : public IOdinTransport
{
public:
    QList<QByteArray> reads;       // 每次 read() 出队一个；空队列 → 轮询返回空 / 正超时返回空 + error
    QList<QByteArray> writes;      // 每次 write() 追加（**含空写 = ZLP**）
    QStringList calls;             // "open"/"close"/"write"/"read"/"poll"/"reset"
    QList<int> readTimeouts;       // 每次 read 的 timeoutMs（钉住「轮询 = 0」）
    int  failWriteAt = -1;         // 第 N 次 write 失败（0 基），-1 = 不失败
    bool openResult = true;
    int  maxPacket = 512;

    bool open(QString *error) override { Q_UNUSED(error) calls << QStringLiteral("open"); return openResult; }
    void close() override { calls << QStringLiteral("close"); }

    bool write(const QByteArray &data, QString *error) override {
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        calls << QStringLiteral("write");
        writes.append(data);
        return true;
    }

    QByteArray read(int maxBytes, int timeoutMs, QString *error) override {
        readTimeouts.append(timeoutMs);
        calls << (timeoutMs == 0 ? QStringLiteral("poll") : QStringLiteral("read"));
        if (reads.isEmpty()) {
            if (timeoutMs == 0) return {};              // 端点静默：空返回**且不置 error**
            if (error) *error = QStringLiteral("读超时（mock 队列空）");
            return {};
        }
        QByteArray head = reads.takeFirst();
        if (head.size() > maxBytes) {                   // 超出请求长度的部分留待下次（真实 USB 语义）
            reads.prepend(head.mid(maxBytes));
            head.truncate(maxBytes);
        }
        return head;
    }

    bool reset(QString *error) override { Q_UNUSED(error) calls << QStringLiteral("reset"); return true; }
    int maxPacketSize() const override { return maxPacket; }
};

} // namespace odin
```

- [ ] **Step 2: 写失败用例 `tests/test_odin_session.cpp`**

（完整用例全文见 Step 2 的代码块；覆盖：完整会话逐条字节、空写只出现在 D7 时机、严格 partIndex、失败不发 0x67、设备 PIT 缺分区拒刷、dump 失败回退、协商分支、进度到 100。）

```cpp
// tests/test_odin_session.cpp
//
// OdinSession 编排：断言**写出去的完整字节序列**（命令帧 + 数据分片 + 唯一一处空写）与调用序列。
// 无真机可验证的核心手段（与 test_edl_session.cpp 同款）。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/odin/odin_session.h"
#include "core/odin/odin_protocol.h"
#include "mock_odin_transport.h"
#include "odin_test_helpers.h"

class TestOdinSession : public QObject
{
    Q_OBJECT
private slots:
    void fullSessionByteSequence();
    void stopsOnPartIndexMismatchWithoutEndSession();
    void refusesWhenDevicePitLacksPartition();
    void fallsBackToPackagePitWhenDumpFails();
    void refusesEmptyPlanWithoutTouchingDevice();
    void refusesZeroSizedImage();
    void negotiatesOnlyForVersion2();
    void reportsProgressToHundred();
    void failsWhenImageFileMissing();
};

using namespace odin;
using namespace odintest;

// 造一个镜像文件 + 一个单条目计划（默认 1 个分区 BOOT/spl.img）
struct Fixture {
    QTemporaryDir dir;
    QByteArray image;
    SamsungPlan plan;
    QString imagePath;
};

static bool makeFixture(Fixture &fx, const QByteArray &image, const QString &partition = QStringLiteral("BOOT"),
                        const QString &imageName = QStringLiteral("spl.img"), quint32 identifier = 80)
{
    fx.image = image;
    const QString tarPath = fx.dir.path() + QStringLiteral("/BL.tar.md5");
    {
        QFile f(tarPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(image) != image.size())
            return false;
    }
    SamsungPlanFile file;
    file.path = tarPath;
    file.sizeBytes = quint64(image.size());
    file.md5HasFooter = false;
    file.verifyOk = false;
    file.entryNames << imageName;
    fx.plan.files.append(file);
    SamsungPlanEntry e;
    e.partition = partition;
    e.imageFile = imageName;
    e.sizeBytes = quint64(image.size());
    e.sourceOffset = 0;                       // 夹具文件里镜像从 0 开始
    e.fileIndex = 0;
    e.matchRule = QStringLiteral("文件名精确匹配");
    e.pit.partitionName = partition;
    e.pit.identifier = identifier;
    e.pit.deviceType = 2;
    e.pit.binaryType = 0;
    e.pit.blockCount = 1024;
    fx.plan.entries.append(e);
    fx.plan.totalBytes = quint64(image.size());
    fx.imagePath = tarPath;
    return true;
}

// 8 字节应答：id + code
static QByteArray ackFrame(quint32 id, quint32 code)
{
    QByteArray a(8, '\0');
    odintest::putU32(a, 0, id);
    odintest::putU32(a, 4, code);
    return a;
}

// 起会话应答：id=0x64，code = (version<<16) | (compressed? 0x80000000:0)
static QByteArray beginAck(quint32 version, bool compressed = false)
{
    const quint32 code = (version << 16) | (compressed ? 0x80000000u : 0u);
    return ackFrame(0x64, code);
}

// 常规起会话读脚本：握手 → 起会话 →（version≥2）片大小协商 → 机型查询（12B）→ 总字节
static void queueSessionSetup(MockOdinTransport &t, quint32 version = 2, bool compressed = true)
{
    t.reads << QByteArray("LOKE") << beginAck(version, compressed);
    if (version >= 2)
        t.reads << ackFrame(0x64, 0);
    QByteArray dt(12, '\0');
    dt[0] = char(0x64);
    odintest::putU32(dt, 8, 0x1234);
    t.reads << dt;
    t.reads << ackFrame(0x64, 0);
}

// PIT dump 读脚本：请求应答（含大小）+ 每 500 字节一片的**原始数据** + 尾空包 + 结束应答
static void queuePitDump(MockOdinTransport &t, const QByteArray &pitBytes)
{
    t.reads << ackFrame(0x65, quint32(pitBytes.size()));
    for (int off = 0; off < pitBytes.size(); off += 500)
        t.reads << pitBytes.mid(off, 500);
    t.reads << QByteArray();                       // 最后一片后设备的空包（Heimdall kEmptyTransferAfter 位）
    t.reads << ackFrame(0x65, 0);                  // 结束应答
}

// 单条目写入段的读脚本：申请文件传输 / 申请序列 / 每片 / 结束序列
static void queueEntryWrites(MockOdinTransport &t, int partCount)
{
    t.reads << ackFrame(0x66, 0) << ackFrame(0x66, 0);
    for (int i = 0; i < partCount; ++i)
        t.reads << ackFrame(0x00, quint32(i));
    t.reads << ackFrame(0x66, 0);
}

static void queueEndSession(MockOdinTransport &t)
{
    t.reads << ackFrame(0x67, 0) << ackFrame(0x67, 0);
}

// 设备侧 PIT（单条 BOOT → spl.img，与夹具计划同源）
static QByteArray devicePitBytes(const QByteArray &partition = QByteArray("BOOT"),
                                 const QByteArray &flashName = QByteArray("spl.img"))
{
    PitSpec s;
    s.name = partition;
    s.deviceType = 2;
    s.identifier = 80;
    s.blockCount = 1024;
    s.flashFilename = flashName;
    return buildPit({s});                          // 默认带 1024 字节尾部 → 3 个 500 字节分片
}

// 完整会话：逐条核对**写出去的字节**（命令帧 + 数据片 + 唯一一处空写）与调用序列
void TestOdinSession::fullSessionByteSequence()
{
    Fixture fx;
    QVERIFY(fx.dir.isValid());
    const QByteArray image(3000, '\x5A');          // 一个片内（1 MiB 片大小）
    QVERIFY(makeFixture(fx, image));

    MockOdinTransport t;
    queueSessionSetup(t);
    queuePitDump(t, devicePitBytes());             // 1184 字节 → 3 片（顺带钉住 PIT 分片拼接）
    queueEntryWrites(t, 1);
    queueEndSession(t);

    OdinSession s(t);
    QString err;
    QVERIFY2(s.run(fx.plan, OdinOptions{}, &err), qPrintable(err));

    const TransferProfile prof = profileForVersion(2);
    const quint32 aligned = alignedSequenceBytes(quint32(image.size()), prof.packetSize);
    QCOMPARE(aligned, prof.packetSize);            // 3000 字节 → 一个整片
    QCOMPARE(t.writes.size(), 17);
    QCOMPARE(t.writes[0], QByteArray("ODIN", 4));
    QCOMPARE(t.writes[1], frameBeginSession());
    QCOMPARE(t.writes[2], frameFilePartSize(prof.packetSize));
    QCOMPARE(t.writes[3], frameDeviceTypeQuery());
    QCOMPARE(t.writes[4], frameTotalBytes(quint64(image.size())));
    QCOMPARE(t.writes[5], framePitDumpRequest());
    QCOMPARE(t.writes[6], framePitPartRequest(0)); // 3 片 PIT：逐片请求，数据是**裸分片**（无 8 字节头）
    QCOMPARE(t.writes[7], framePitPartRequest(1));
    QCOMPARE(t.writes[8], framePitPartRequest(2));
    QCOMPARE(t.writes[9], framePitEndRequest());
    QCOMPARE(t.writes[10], frameRequestFlash());
    QCOMPARE(t.writes[11], frameRequestSequence(aligned));
    QCOMPARE(t.writes[12].size(), int(prof.packetSize));
    QCOMPARE(t.writes[12].left(image.size()), image);
    QCOMPARE(t.writes[12].mid(image.size()), QByteArray(int(prof.packetSize) - image.size(), '\0')); // D9 零填充
    QCOMPARE(t.writes[13], QByteArray());          // D7：结束序列**前**的空写（全序列唯一一处空写）
    QCOMPARE(t.writes[14], frameEndSequence(fx.plan.entries[0].pit, quint32(image.size()), true));
    QCOMPARE(t.writes[15], frameEndSession(false));
    QCOMPARE(t.writes[16], frameEndSession(true));
    QCOMPARE(t.calls.first(), QStringLiteral("open"));
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
    QVERIFY(t.readTimeouts.contains(0));           // 握手后的轮询必须是 timeout 0（否则真机阻塞）
}

void TestOdinSession::stopsOnPartIndexMismatchWithoutEndSession()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;                     // 本例只需写到数据面
    t.reads << ackFrame(0x66, 0) << ackFrame(0x66, 0)
            << ackFrame(0x00, 5);                  // 设备回的分片序号是 5，期望 0
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, opt, &err));
    QVERIFY(err.contains(QStringLiteral("BOOT")));
    QVERIFY(err.contains(QStringLiteral("分片")));
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameEndSession(false));      // 失败路径**不发**结束会话
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

void TestOdinSession::refusesWhenDevicePitLacksPartition()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image, QStringLiteral("BOOT")));
    MockOdinTransport t;
    queueSessionSetup(t);
    queuePitDump(t, devicePitBytes(QByteArray("SBOOT"), QByteArray("sboot.bin")));   // 设备 PIT 里没有 BOOT
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, OdinOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("BOOT")));
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameRequestFlash());         // 拒刷：**一条数据都不发**
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

void TestOdinSession::fallsBackToPackagePitWhenDumpFails()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    t.reads << ackFrame(0xFFFFFFFFu, 4);           // PIT dump 请求被判 BOOTLOADER_FAIL(-4 Write)
    queueEntryWrites(t, 1);
    queueEndSession(t);
    QList<QString> details;
    OdinSession s(t, [&details](const OdinProgress &p) { details << p.detail; });
    QString err;
    QVERIFY2(s.run(fx.plan, OdinOptions{}, &err), qPrintable(err));   // 成功（回退包内 PIT）
    bool warned = false;
    for (const QString &d : std::as_const(details))
        warned = warned || d.contains(QStringLiteral("设备 PIT"));
    QVERIFY(warned);
    bool flashed = false;
    for (const QByteArray &w : std::as_const(t.writes))
        flashed = flashed || w == frameRequestFlash();
    QVERIFY(flashed);
}

void TestOdinSession::refusesEmptyPlanWithoutTouchingDevice()
{
    MockOdinTransport t;
    SamsungPlan plan;                              // 空计划
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(plan, OdinOptions{}, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(t.calls.isEmpty());                    // 连 open 都不发
    QVERIFY(t.writes.isEmpty());
}

void TestOdinSession::refusesZeroSizedImage()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    fx.plan.entries[0].sizeBytes = 0;
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, opt, &err));
    QVERIFY(!err.isEmpty());
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameRequestFlash());
}

void TestOdinSession::negotiatesOnlyForVersion2()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t, 1, false);                // version 1：不发片大小协商（D4）
    OdinOptions opt;
    opt.dumpDevicePit = false;
    queueEntryWrites(t, 1);
    queueEndSession(t);
    OdinSession s(t);
    QString err;
    QVERIFY2(s.run(fx.plan, opt, &err), qPrintable(err));
    bool negotiated = false;
    for (const QByteArray &w : std::as_const(t.writes))
        negotiated = negotiated || w == frameFilePartSize(1048576) || w == frameFilePartSize(131072);
    QVERIFY(!negotiated);
    // 片大小 = 128 KiB（profileForVersion(1)），序列声明值同款
    QCOMPARE(t.writes[2], frameDeviceTypeQuery());  // 索引 1 = 起会话，索引 2 直接是机型查询
    bool seq = false;
    for (const QByteArray &w : std::as_const(t.writes))
        seq = seq || w == frameRequestSequence(131072);
    QVERIFY(seq);
}

void TestOdinSession::reportsProgressToHundred()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;
    queueEntryWrites(t, 1);
    queueEndSession(t);
    QList<OdinProgress> seen;
    OdinSession s(t, [&seen](const OdinProgress &p) { seen << p; });
    QString err;
    QVERIFY2(s.run(fx.plan, opt, &err), qPrintable(err));
    QVERIFY(!seen.isEmpty());
    QCOMPARE(seen.last().percent, 100);
    QStringList stages;
    for (const OdinProgress &p : std::as_const(seen))
        if (!stages.contains(p.stage))
            stages << p.stage;
    QVERIFY(stages.contains(QStringLiteral("handshake")));
    QVERIFY(stages.contains(QStringLiteral("session")));
    QVERIFY(stages.contains(QStringLiteral("write")));
    QVERIFY(stages.contains(QStringLiteral("done")));
    // 进度单调不减
    int prev = -1;
    for (const OdinProgress &p : std::as_const(seen)) {
        QVERIFY(p.percent >= prev);
        prev = p.percent;
    }
}

void TestOdinSession::failsWhenImageFileMissing()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    QVERIFY(QFile::remove(fx.imagePath));          // 计划构建后文件消失
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, opt, &err));
    QVERIFY(err.contains(QStringLiteral("BOOT")));
    // 打开镜像失败必须在**发任何 0x66 命令之前**（不把设备带进半途状态）
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameRequestFlash());
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}
```

- [ ] **Step 3: 实现 `odin_transport.h` / `odin_session.{h,cpp}`**

`odin_transport.h`：头文件顶部写清生命周期契约（`close()` 幂等、`open()` 可重入、`write(空)` = ZLP、`read(timeoutMs=0)` = 非阻塞轮询且**不置 error**）—— 与 `src/core/edl/edl_transport.h` 同款措辞；并写明**本文件是全仓唯一允许被协议模块看到的设备缝**。

`odin_session.h`：

```cpp
#pragma once
#include <QString>
#include <functional>

#include "odin_transport.h"
#include "odin_protocol.h"
#include "samsung_plan.h"

namespace odin {

struct OdinOptions {
    bool dumpDevicePit = true;      // 读设备 PIT 并以其为准（失败 → 告警 + 回退包内 PIT）
    bool reboot = true;             // 收尾 0x67/0x01 重启（best-effort）
    bool queryDeviceType = true;    // 0x64/0x01 机型查询（best-effort，只记日志）
    int controlTimeoutMs = 10000;   // odin4-llucs/src/usb/usb_device.h:16（USB_TIMEOUT_CONTROL）
    int handshakeTimeoutMs = 1000;  // Heimdall BridgeManager.cpp:306（"ODIN" 握手超时）
};

struct OdinProgress { QString stage; QString detail; int percent = 0; };
using OdinProgressFn = std::function<void(const OdinProgress &)>;

class OdinSession
{
public:
    explicit OdinSession(IOdinTransport &transport, OdinProgressFn progress = {});

    // 完整刷写：握手 → 起会话（版本协商）→ 机型查询 → 总字节 → 读设备 PIT（best-effort）
    //           → 对账（以设备 PIT 为准）→ 逐条目写入 → 结束会话/重启。
    // 失败语义：任何一步失败 → 立即返回 false + 中文 *error（带阶段名/分区名/已写字节），
    //           **不发结束会话、不改设备状态**（设备留在 Odin 模式便于重试；Heimdall 无论成败
    //           都发 EndSession（FlashAction.cpp:570），本实现**不采纳**）。
    // 收尾（0x67/0x00、0x67/0x01）失败**不判失败**，只落日志（Phase B 的 reset 同款口径）。
    // 句柄：**所有路径都 close()**（包括失败路径）。
    bool run(const SamsungPlan &plan, const OdinOptions &opt, QString *error);

private:
    struct ResolvedEntry {
        QString partition; QString imageFile; QString path;
        quint64 offset = 0; quint64 size = 0; PitEntry pit;
    };

    bool handshake(QString *error);
    bool beginSession(QString *error);
    bool readAck(quint32 expectedId, bool allowProgressCodes, const QString &context, QString *error);
    void queryDeviceType();
    bool setTotalBytes(quint64 total, QString *error);
    bool dumpDevicePit(PitTable &out, QString *error);
    bool resolveEntries(const SamsungPlan &plan, const PitTable *devicePit,
                        QList<ResolvedEntry> &out, QString *error);
    bool writeEntry(const ResolvedEntry &r, QString *error);
    void endSession(bool reboot);
    void report(const QString &stage, const QString &detail, int percent);
    int  percentFor(quint64 writtenBytes) const;

    IOdinTransport &m_t;
    OdinProgressFn  m_progress;
    OdinOptions     m_opt;
    TransferProfile m_profile;
    quint64         m_totalBytes = 0;
    quint64         m_writtenBytes = 0;
};

} // namespace odin
```

`odin_session.cpp`（完整实现）：

```cpp
// src/core/odin/odin_session.cpp
//
// 会话编排：顺序、时机与字节。协议帧构造在 odin_protocol.cpp、计划在 samsung_plan.cpp、
// 设备访问只经 IOdinTransport —— 本文件不碰 libusb、不解析 XML。
// 流程参照：heimdall/source/FlashAction.cpp:540-571（Initialise → BeginSession →（tflash）
// → TotalBytes → getPitData → flashPartitions → EndSession）与 odin4-llucs/src/odin4.cpp:165-189。
#include "odin_session.h"

#include <QFile>

namespace odin {
namespace {

// 设备回报的 PIT 大小上限（odin4-llucs/src/usb/odin_protocol.cpp:634 同款；真 PIT 2924..18492 B）
constexpr quint32 kMaxPitBytes = 1048576;

void setErr(QString *error, const QString &msg) { if (error) *error = msg; }

void prefixErr(QString *error, const QString &prefix)
{
    if (error && !error->isEmpty())
        *error = prefix + QStringLiteral("：") + *error;
    else if (error)
        *error = prefix;
}

QString hexBytes(const QByteArray &b) { return QString::fromLatin1(b.toHex(' ')); }

} // namespace

OdinSession::OdinSession(IOdinTransport &transport, OdinProgressFn progress)
    : m_t(transport), m_progress(std::move(progress))
{
}

void OdinSession::report(const QString &stage, const QString &detail, int percent)
{
    if (m_progress)
        m_progress(OdinProgress{stage, detail, percent});
}

int OdinSession::percentFor(quint64 writtenBytes) const
{
    if (m_totalBytes == 0)
        return 100;
    return int(qMin<quint64>(writtenBytes * 100 / m_totalBytes, 100));
}

bool OdinSession::run(const SamsungPlan &plan, const OdinOptions &opt, QString *error)
{
    m_opt = opt;
    m_writtenBytes = 0;
    m_totalBytes = plan.totalBytes;

    if (plan.entries.isEmpty()) {              // 空计划：一条命令都不发（不 open）
        setErr(error, QStringLiteral("计划为空：没有可刷写的分区"));
        return false;
    }
    if (!m_t.open(error)) {
        prefixErr(error, QStringLiteral("打开设备失败"));
        return false;
    }

    bool ok = handshake(error) && beginSession(error);
    if (ok && m_opt.queryDeviceType)
        queryDeviceType();                     // best-effort：不影响 ok
    if (ok && !setTotalBytes(plan.totalBytes, error))
        ok = false;

    PitTable devicePit;
    bool haveDevicePit = false;
    if (ok && m_opt.dumpDevicePit) {
        QString derr;
        if (dumpDevicePit(devicePit, &derr))
            haveDevicePit = true;
        else
            report(QStringLiteral("pit"),
                   QStringLiteral("读取设备 PIT 失败，改用包内 PIT：%1").arg(derr),
                   percentFor(m_writtenBytes));
    }

    QList<ResolvedEntry> todo;
    if (ok && !resolveEntries(plan, haveDevicePit ? &devicePit : nullptr, todo, error))
        ok = false;

    if (ok) {
        for (const ResolvedEntry &r : std::as_const(todo)) {
            if (!writeEntry(r, error)) { ok = false; break; }
        }
    }

    if (ok) {
        endSession(m_opt.reboot);              // 收尾失败只落日志，不改结论
        report(QStringLiteral("done"), QStringLiteral("刷写完成"), 100);
    }
    m_t.close();                               // 所有路径都关句柄
    return ok;
}

bool OdinSession::handshake(QString *error)
{
    report(QStringLiteral("handshake"), QStringLiteral("发送 ODIN 握手"), 0);
    QString werr;
    if (!m_t.write(QByteArray("ODIN", 4), &werr)) {
        setErr(error, QStringLiteral("握手失败：%1").arg(werr));
        return false;
    }
    // 读只给 handshakeTimeoutMs（1s）：设备不在 Odin 模式时必须快速失败
    // （Heimdall BridgeManager.cpp:306/316 的 1000ms；odin4 用 10s —— 取短的一侧，失败更快）
    const QByteArray rsp = m_t.read(4, m_opt.handshakeTimeoutMs, error);
    if (rsp.size() != 4 || rsp != QByteArray("LOKE", 4)) {
        setErr(error, rsp.isEmpty()
                          ? QStringLiteral("握手失败：未收到设备响应（期望 LOKE）")
                          : QStringLiteral("握手失败：收到 %1（期望 LOKE）").arg(hexBytes(rsp)));
        return false;
    }
    // 握手后清一次 IN 端点（odin4 odin_protocol.cpp:304-326 的 drain）：残留字节会让后续读错位。
    // timeoutMs = 0 = 非阻塞轮询，拿不到东西**不置 error**。
    m_t.read(64, 0, nullptr);
    return true;
}

bool OdinSession::readAck(quint32 expectedId, bool allowProgressCodes,
                          const QString &context, QString *error)
{
    QString rerr;
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, &rerr);
    if (rsp.size() < kAckSize) {
        setErr(error, QStringLiteral("%1：未收到完整应答（%2 字节）%3")
                          .arg(context).arg(rsp.size())
                          .arg(rerr.isEmpty() ? QString() : QStringLiteral("；传输层报：") + rerr));
        return false;
    }
    Ack ack;
    if (!parseAck(rsp, ack, error)) {
        prefixErr(error, context);
        return false;
    }
    const AckVerdict v = judgeAck(ack, expectedId, allowProgressCodes);
    if (!v.ok) {
        setErr(error, QStringLiteral("%1：%2").arg(context, v.reason));
        return false;
    }
    if (ack.code != 0)
        // 非零但非负：不判失败（odin4/Thor 只要求非负；Heimdall 要求必须为 0 —— 采用前者**但记账**）
        report(QStringLiteral("info"),
               QStringLiteral("%1：设备返回状态码 %2（非零且非失败码，继续）").arg(context).arg(ack.code),
               percentFor(m_writtenBytes));
    return true;
}

bool OdinSession::beginSession(QString *error)
{
    QString werr;
    if (!m_t.write(frameBeginSession(), &werr)) {
        setErr(error, QStringLiteral("起会话失败：%1").arg(werr));
        return false;
    }
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, error);
    BeginSessionAck a;
    if (!parseBeginSessionAck(rsp, a, error)) {
        prefixErr(error, QStringLiteral("起会话失败"));
        return false;
    }
    // ⚠️ 这个应答的 code 字段**是版本号**：只判 id（不能做 code<0 判定，见 odin_protocol.h）
    const AckVerdict v = judgeAckIdOnly(Ack{a.id, a.code}, kControlSession);
    if (!v.ok) {
        setErr(error, QStringLiteral("起会话被设备拒绝：%1").arg(v.reason));
        return false;
    }
    m_profile = profileForVersion(a.version);
    report(QStringLiteral("session"),
           QStringLiteral("协议版本 %1：片大小 %2 KiB / 每序列 %3 片 / 刷写超时 %4 ms%5")
               .arg(a.version).arg(m_profile.packetSize / 1024).arg(m_profile.sequenceCount)
               .arg(m_profile.flashTimeoutMs)
               .arg(a.compressedSupported ? QStringLiteral("（设备支持压缩传输，本期发原始数据）")
                                          : QString()),
           0);

    if (a.version >= 2) {
        // D4：只有 version>=2 才协商片大小（odin4:399-406 / Thor:58-71）
        if (!m_t.write(frameFilePartSize(m_profile.packetSize), &werr)) {
            setErr(error, QStringLiteral("协商片大小失败：%1").arg(werr));
            return false;
        }
        if (!readAck(kControlSession, false, QStringLiteral("协商片大小"), error))
            return false;
    }
    return true;
}

void OdinSession::queryDeviceType()
{
    // best-effort（odin4 odin_protocol.cpp:433-452 的调用点：握手后、起会话前）。
    // ⚠️ D13：同一个 0x64/0x01 在 Thor 是"重置刷写计数"（刷完才发），在 odin4 里既当机型查询
    // 又当重置 —— 本期只做**一次查询**，刷完**不**发重置。语义冲突留给持机人裁定。
    QString err;
    if (!m_t.write(frameDeviceTypeQuery(), &err)) {
        report(QStringLiteral("info"), QStringLiteral("机型查询发送失败（跳过）：%1").arg(err),
               percentFor(m_writtenBytes));
        return;
    }
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, &err);
    if (rsp.size() < 12) {
        report(QStringLiteral("info"),
               QStringLiteral("设备未回报机型（应答 %1 字节）").arg(rsp.size()),
               percentFor(m_writtenBytes));
        return;
    }
    Ack ack;
    if (!parseAck(rsp, ack, nullptr) || !judgeAckIdOnly(ack, kControlSession).ok) {
        report(QStringLiteral("info"),
               QStringLiteral("机型查询被拒（应答 id 0x%1）").arg(ack.id, 8, 16, QLatin1Char('0')),
               percentFor(m_writtenBytes));
        return;
    }
    // odin4 把该值读成 "SM-<十进制>"（:450）—— 那是它的约定、不是权威定义；这里只记原始值。
    const quint32 raw = quint32(quint8(rsp.at(8))) | (quint32(quint8(rsp.at(9))) << 8)
                      | (quint32(quint8(rsp.at(10))) << 16) | (quint32(quint8(rsp.at(11))) << 24);
    report(QStringLiteral("info"),
           QStringLiteral("设备机型原始值 0x%1（odin4 记为 SM-%2；语义未验证，仅记录）")
               .arg(raw, 8, 16, QLatin1Char('0')).arg(raw),
           percentFor(m_writtenBytes));
}

bool OdinSession::setTotalBytes(quint64 total, QString *error)
{
    QString werr;
    if (!m_t.write(frameTotalBytes(total), &werr)) {
        setErr(error, QStringLiteral("上报总字节失败：%1").arg(werr));
        return false;
    }
    return readAck(kControlSession, false, QStringLiteral("上报总字节"), error);
}

bool OdinSession::dumpDevicePit(PitTable &out, QString *error)
{
    QString werr;
    if (!m_t.write(framePitDumpRequest(), &werr)) {
        setErr(error, QStringLiteral("请求读取设备 PIT 失败：%1").arg(werr));
        return false;
    }
    // ① 请求应答：id 回显 0x65 + u32 文件大小（Heimdall PitFileResponse；odin4:626-637）
    QString rerr;
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, &rerr);
    if (rsp.size() < kAckSize) {
        setErr(error, QStringLiteral("读取设备 PIT 失败：未收到大小应答（%1 字节）%2")
                          .arg(rsp.size())
                          .arg(rerr.isEmpty() ? QString() : QStringLiteral("；") + rerr));
        return false;
    }
    Ack ack;
    if (!parseAck(rsp, ack, error)) {
        prefixErr(error, QStringLiteral("读取设备 PIT 失败"));
        return false;
    }
    const AckVerdict v = judgeAck(ack, kControlPitFile, false);
    if (!v.ok) {
        setErr(error, QStringLiteral("读取设备 PIT 失败：%1").arg(v.reason));
        return false;
    }
    const quint32 size = ack.code;
    if (size == 0 || size > kMaxPitBytes) {
        setErr(error, QStringLiteral("设备回报的 PIT 大小不合理（%1 字节）").arg(size));
        return false;
    }
    // ② 逐片取：0x65/0x02 + 片序号 → 应答是**裸的 ≤500 字节数据**（无 8 字节头）
    //    （Heimdall 的 ReceiveFilePartPacket 是 500 字节变长包；odin4:643-652 同样把应答当原始数据）
    QByteArray data;
    data.reserve(int(size));
    const quint32 parts = (size + kPitPartSize - 1) / kPitPartSize;
    for (quint32 i = 0; i < parts; ++i) {
        if (!m_t.write(framePitPartRequest(i), &werr)) {
            setErr(error, QStringLiteral("读取设备 PIT 第 %1 片失败：%2").arg(i).arg(werr));
            return false;
        }
        const QByteArray chunk = m_t.read(kPitPartSize, m_opt.controlTimeoutMs, &rerr);
        if (chunk.isEmpty()) {
            setErr(error, QStringLiteral("读取设备 PIT 第 %1/%2 片失败：%3")
                              .arg(i + 1).arg(parts)
                              .arg(rerr.isEmpty() ? QStringLiteral("设备未回数据") : rerr));
            return false;
        }
        data.append(chunk);
    }
    // ③ 末片之后设备会再发一次空包（Heimdall 末片带 kEmptyTransferAfter；odin4:654-657 紧随一次 IN 空读）
    //    —— best-effort，拿不到不算错
    m_t.read(1, m_opt.controlTimeoutMs, nullptr);
    // ④ 结束：0x65/0x03 → 应答
    if (!m_t.write(framePitEndRequest(), &werr)) {
        setErr(error, QStringLiteral("结束读取设备 PIT 失败：%1").arg(werr));
        return false;
    }
    if (!readAck(kControlPitFile, false, QStringLiteral("结束读取设备 PIT"), error))
        return false;

    data.truncate(int(size));                  // 末片可能多读（设备按 500 发，我们只要 size 字节）
    if (quint32(data.size()) != size) {
        setErr(error, QStringLiteral("设备 PIT 数据不完整（期望 %1 字节，收到 %2）")
                          .arg(size).arg(data.size()));
        return false;
    }
    return parsePit(data, out, error);
}

bool OdinSession::resolveEntries(const SamsungPlan &plan, const PitTable *devicePit,
                                 QList<ResolvedEntry> &out, QString *error)
{
    out.clear();
    QStringList missing;
    for (const SamsungPlanEntry &e : plan.entries) {
        if (e.fileIndex < 0 || e.fileIndex >= plan.files.size()) {
            setErr(error, QStringLiteral("计划条目 %1 的来源包索引非法（%2）")
                              .arg(e.partition).arg(e.fileIndex));
            return false;
        }
        ResolvedEntry r;
        r.partition = e.partition;
        r.imageFile = e.imageFile;
        r.path = plan.files.at(e.fileIndex).path;
        r.offset = e.sourceOffset;
        r.size = e.sizeBytes;
        r.pit = e.pit;
        if (r.size == 0) {
            setErr(error, QStringLiteral("计划条目 %1 的镜像 %2 大小为 0，拒绝刷写")
                              .arg(e.partition, e.imageFile));
            return false;
        }
        if (devicePit) {
            // spec §5：以设备 PIT 为准（设备自身布局才是真相；信包内 PIT 可能写错位置）
            const PitEntry *de = devicePit->findByName(e.partition);
            if (!de) {
                missing << e.partition;         // 收集齐再一次性报（用户能看到全部缺哪些）
                continue;
            }
            if (de->identifier != e.pit.identifier || de->deviceType != e.pit.deviceType
                || de->binaryType != e.pit.binaryType) {
                report(QStringLiteral("validate"),
                       QStringLiteral("设备 PIT 与包内 PIT 不一致，以设备为准：%1"
                                      "（id %2→%3 / deviceType %4→%5 / binaryType %6→%7）")
                           .arg(e.partition).arg(e.pit.identifier).arg(de->identifier)
                           .arg(e.pit.deviceType).arg(de->deviceType)
                           .arg(e.pit.binaryType).arg(de->binaryType),
                       percentFor(m_writtenBytes));
            }
            r.pit = *de;
        }
        out.append(r);
    }
    if (!missing.isEmpty()) {
        setErr(error, QStringLiteral("设备 PIT 中不存在以下分区：%1 —— 拒刷（未下发任何数据）。"
                                     "请核对固件包与机型是否配套")
                          .arg(missing.join(QStringLiteral("、"))));
        return false;
    }
    return true;
}

bool OdinSession::writeEntry(const ResolvedEntry &r, QString *error)
{
    // 先开镜像：**任何设备命令之前**失败（不把设备带进半途状态）
    QFile image(r.path);
    if (!image.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("打开镜像失败：%1（分区 %2）").arg(r.path, r.partition));
        return false;
    }
    if (!image.seek(qint64(r.offset))) {
        setErr(error, QStringLiteral("定位镜像数据失败：%1（分区 %2，偏移 %3）")
                          .arg(r.path, r.partition).arg(r.offset));
        return false;
    }

    QString werr;
    if (!m_t.write(frameRequestFlash(), &werr)) {
        setErr(error, QStringLiteral("申请文件传输失败（分区 %1）：%2").arg(r.partition, werr));
        return false;
    }
    if (!readAck(kControlFileTransfer, false,
                 QStringLiteral("申请文件传输（分区 %1）").arg(r.partition), error))
        return false;

    const quint64 seqBytes = quint64(m_profile.packetSize) * quint64(m_profile.sequenceCount);
    const quint64 sequences = (r.size + seqBytes - 1) / seqBytes;
    quint64 sent = 0;

    for (quint64 si = 0; si < sequences; ++si) {
        const bool isLast = (si + 1 == sequences);
        const quint64 realSize = isLast ? (r.size - si * seqBytes) : seqBytes;
        const quint32 aligned = alignedSequenceBytes(realSize, m_profile.packetSize);
        if (aligned == 0) {
            setErr(error, QStringLiteral("分区 %1 的序列 %2 对齐后长度为 0").arg(r.partition).arg(si));
            return false;
        }
        if (!m_t.write(frameRequestSequence(aligned), &werr)) {
            setErr(error, QStringLiteral("申请序列失败（分区 %1，序列 %2）：%3")
                              .arg(r.partition).arg(si).arg(werr));
            return false;
        }
        if (!readAck(kControlFileTransfer, false,
                     QStringLiteral("申请序列（分区 %1，序列 %2）").arg(r.partition).arg(si), error))
            return false;

        const quint32 parts = aligned / m_profile.packetSize;
        for (quint32 pi = 0; pi < parts; ++pi) {
            const quint64 remaining = r.size - sent;
            const int toRead = int(qMin<quint64>(remaining, quint64(m_profile.packetSize)));
            QByteArray part(int(m_profile.packetSize), '\0');   // D9：末片零填充到整片（三方一致）
            if (toRead > 0) {
                const QByteArray chunk = image.read(toRead);
                if (chunk.size() != toRead) {
                    setErr(error, QStringLiteral("读取镜像失败（分区 %1，偏移 %2，需 %3 字节，读到 %4）")
                                      .arg(r.partition).arg(r.offset + sent).arg(toRead).arg(chunk.size()));
                    return false;
                }
                part.replace(0, toRead, chunk);
            }
            if (!m_t.write(part, &werr)) {
                setErr(error, QStringLiteral("写入分片失败（分区 %1，已写 %2 字节）：%3")
                                  .arg(r.partition).arg(sent).arg(werr));
                return false;
            }
            QString rerr;
            const QByteArray rsp = m_t.read(kAckSize, m_profile.flashTimeoutMs, &rerr);
            if (rsp.size() < kAckSize) {
                setErr(error, QStringLiteral("分片应答缺失（分区 %1，分片 %2，已写 %3 字节）：%4")
                                  .arg(r.partition).arg(pi).arg(sent)
                                  .arg(rerr.isEmpty() ? QStringLiteral("设备未回应答") : rerr));
                return false;
            }
            Ack ack;
            if (!parseAck(rsp, ack, error)) {
                prefixErr(error, QStringLiteral("分片应答解析失败（分区 %1）").arg(r.partition));
                return false;
            }
            const AckVerdict v = judgeAck(ack, kResponseSendFilePart, false);
            if (!v.ok) {
                setErr(error, QStringLiteral("分片被拒（分区 %1，分片 %2，已写 %3 字节）：%4")
                                  .arg(r.partition).arg(pi).arg(sent).arg(v.reason));
                return false;
            }
            if (ack.code != pi) {              // 严格序号：错位继续发会把数据写到别处
                setErr(error, QStringLiteral("分片序号不符（分区 %1，分片 %2）：期望 %3，设备回 %4")
                                  .arg(r.partition).arg(pi).arg(pi).arg(ack.code));
                return false;
            }
            sent += quint64(toRead);
            report(QStringLiteral("write"), QStringLiteral("%1（%2）").arg(r.partition, r.imageFile),
                   percentFor(m_writtenBytes + sent));
        }

        // D7：结束序列命令**前**发一次空写（Heimdall kEmptyTransferBeforeAndAfter + odin4
        // send_empty_transfer；Thor 不发 → 2:1 取"发"）。命令后**不**发（after 仅 Heimdall，D6/D8）。
        if (!m_t.write(QByteArray(), &werr))
            // 空写失败按容忍处理：odin4 只落 verbose 日志（odin_protocol.cpp:296-301）
            report(QStringLiteral("info"),
                   QStringLiteral("结束序列前的空写失败（忽略）：%1").arg(werr),
                   percentFor(m_writtenBytes + sent));

        if (!m_t.write(frameEndSequence(r.pit, quint32(realSize), isLast), &werr)) {
            setErr(error, QStringLiteral("结束序列失败（分区 %1，序列 %2）：%3")
                              .arg(r.partition).arg(si).arg(werr));
            return false;
        }
        // 结束序列允许 -7..-2 的"进度码"（odin4:363-366：Ext4/Size/Auth/Write/Erase/WP 是分类不是失败）
        if (!readAck(kControlFileTransfer, true,
                     QStringLiteral("结束序列（分区 %1，序列 %2）").arg(r.partition).arg(si), error))
            return false;
    }

    m_writtenBytes += sent;
    report(QStringLiteral("write"),
           QStringLiteral("%1 完成（%2 字节）").arg(r.partition).arg(sent),
           percentFor(m_writtenBytes));
    return true;
}

void OdinSession::endSession(bool reboot)
{
    QString werr;
    if (!m_t.write(frameEndSession(false), &werr)) {
        report(QStringLiteral("end"), QStringLiteral("结束会话失败（忽略）：%1").arg(werr),
               percentFor(m_writtenBytes));
        return;
    }
    QString err;
    if (!readAck(kControlEndSession, false, QStringLiteral("结束会话"), &err))
        report(QStringLiteral("end"), QStringLiteral("结束会话未被确认（忽略）：%1").arg(err),
               percentFor(m_writtenBytes));
    if (!reboot)
        return;
    if (!m_t.write(frameEndSession(true), &werr)) {
        report(QStringLiteral("end"), QStringLiteral("重启命令发送失败（忽略）：%1").arg(werr),
               percentFor(m_writtenBytes));
        return;
    }
    err.clear();
    if (!readAck(kControlEndSession, false, QStringLiteral("重启"), &err))
        report(QStringLiteral("end"), QStringLiteral("重启未被确认（忽略）：%1").arg(err),
               percentFor(m_writtenBytes));
    else
        report(QStringLiteral("end"), QStringLiteral("已请求设备重启"), percentFor(m_writtenBytes));
}

} // namespace odin
```

**实现者注意（两处易错）**
1. `judgeAckIdOnly(Ack{a.id, a.code}, kControlSession)`：`BeginSessionAck` 必须**同时**带 `id` 与 `code` —— 起会话应答的 `code` 是版本号，若实现时图省事用 `Ack{a.version, 0}`（丢掉真实 id），`0xFFFFFFFF` 失败应答就会被漏判成成功。`parseBeginSessionAck` 的用例 `beginSessionAckKeepsHighVersionBits` 已钉住这三条（id 回显 / 高位版本 / BOOTLOADER_FAIL）。
2. `m_writtenBytes` 只在**条目写完后**累加，条目内进度用 `m_writtenBytes + sent` —— 两个量混用会导致进度回退（用例 `reportsProgressToHundred` 断言单调）。

- [ ] **Step 4: 注册测试 + 跑**：CMake 两处同前（列表末尾追加 `test_odin_session.cpp`；分支 `pit.cpp` + `odin_protocol.cpp` + `samsung_plan.cpp` + `odin_session.cpp` + `tar_image.cpp`）。
```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_odin_session
./build/image_engine_tests_test_odin_session && ctest --test-dir build --output-on-failure
```
预期：全绿，43/43。

- [ ] **Step 5: 提交**

```bash
git add src/core/odin/odin_transport.h src/core/odin/odin_session.h src/core/odin/odin_session.cpp tests/mock_odin_transport.h tests/test_odin_session.cpp CMakeLists.txt
git commit -m "feat(odin): 会话编排（握手/协商/读设备PIT/对账/逐分区写入）+ mock 字节序列断言"
```

---

### Task 7: 真机传输 `src/core/odin/odin_libusb_transport.{h,cpp}`

**Files:**
- Create: `src/core/odin/odin_libusb_transport.h`, `src/core/odin/odin_libusb_transport.cpp`
- Create: `tests/test_odin_libusb_transport.cpp`
- Modify: `CMakeLists.txt`（注册 + libusb 链接）

**Interfaces:**
- Consumes: `IOdinTransport`（Task 6）
- Produces:
  ```cpp
  class LibusbOdinTransport : public IOdinTransport {
  public:
      LibusbOdinTransport();
      ~LibusbOdinTransport() override;
      // 设备身份判定（**纯函数**，单测钉住；设备检测与真机传输共用同一判据）
      static bool isOdinDevice(quint16 vid, quint16 pid,
                               const QList<quint8> &interfaceClasses, bool hasBulkInOut);
      static QList<quint16> fallbackPids();
      static QString noDeviceError();
      static int effectiveTimeoutMs(int requested);   // 公开仅为单测（Phase B 同款）
      // IOdinTransport 实现 …
  };
  ```
- 本文件与 `src/core/odin/odin_session.cpp` 的分工：**会话只见 `IOdinTransport`**，libusb 依赖只在本文件 + `src/core/device_detector.cpp`（既有）出现。

**判据（三方对照，写进头文件）**
```
VID 0x04E8 + 接口类 0x0A(CDC_DATA) + 有 bulk in/out   → 认（Thor / odin4 现行做法，覆盖现代机型）
否则若 pid ∈ {0x6601, 0x685D, 0x68C3}                 → 认（Heimdall BridgeManager.h:71-78 的老 PID 兜底）
```
**为什么必须带类判据**：正常开机/充电的三星手机也是 VID 0x04E8（MTP/ADB 类）—— 只按 VID 会把它们误报成"下载模式"。Heimdall 的 3 个老 PID 对现代机型完全不适用（事实报告 §1.5）。

- [ ] **Step 1: 写失败用例 `tests/test_odin_libusb_transport.cpp`**

```cpp
// tests/test_odin_libusb_transport.cpp
#include <QtTest>
#include <QList>

#include "core/odin/odin_libusb_transport.h"

class TestOdinLibusbTransport : public QObject
{
    Q_OBJECT
private slots:
    void matchesByInterfaceClass();
    void fallsBackToLegacyPids();
    void rejectsOtherVendorsAndMtpOnly();
    void noDeviceErrorMentionsVidAndClasses();
    void zeroTimeoutBecomesPollTimeout();
};

using namespace odin;

void TestOdinLibusbTransport::matchesByInterfaceClass()
{
    // 现代机型：VID 0x04E8 + CDC_DATA 接口 + 批量端点 → 认（与 PID 无关）
    QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x0A}, true));
    QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, 0x1234, {0x03, 0x0A}, true));
    // 类对但没有批量端点 → 不认（CDC_DATA 控制接口不是下载模式）
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x0A}, false));
    // 类不对且 PID 不在兜底表 → 不认
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x06, 0xFF}, true));
}

void TestOdinLibusbTransport::fallsBackToLegacyPids()
{
    // 描述符读不到（classes 为空）时靠老 PID 兜底（Heimdall BridgeManager.h:71-78）
    for (quint16 pid : LibusbOdinTransport::fallbackPids())
        QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, pid, {}, false));
    QCOMPARE(LibusbOdinTransport::fallbackPids().size(), 3);
    QVERIFY(LibusbOdinTransport::fallbackPids().contains(0x685D));
    // 老 PID + 类不对（例如被系统当成 MTP 枚举）仍认 —— 兜底表的语义就是"认这个 PID"
    QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, 0x6601, {0x06}, true));
}

void TestOdinLibusbTransport::rejectsOtherVendorsAndMtpOnly()
{
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x05C6, 0x9008, {0x0A}, true));   // 高通 EDL 不是三星
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x18D1, 0x4EE7, {0x0A}, true));
    // 三星手机的 MTP 模式（VID 0x04E8 / 类 0x06 / PID 不在兜底表）→ 不认
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x06}, true));
}

void TestOdinLibusbTransport::noDeviceErrorMentionsVidAndClasses()
{
    const QString msg = LibusbOdinTransport::noDeviceError();
    QVERIFY(msg.contains(QStringLiteral("04e8")));
    QVERIFY(msg.contains(QStringLiteral("CDC")) || msg.contains(QStringLiteral("0a")));
}

void TestOdinLibusbTransport::zeroTimeoutBecomesPollTimeout()
{
    // ⚠️ libusb 的 timeout=0 是**无限等待**（libusb sync.c "For an unlimited timeout, use value 0"），
    // 而 IOdinTransport 的 0 = 非阻塞轮询 —— 直接透传会让会话的 drain 在真机上永久阻塞。
    // 与 Phase B 同款红线（src/core/edl/edl_libusb_transport.cpp 的 effectiveTimeoutMs）。
    QCOMPARE(LibusbOdinTransport::effectiveTimeoutMs(0), 1);
    QCOMPARE(LibusbOdinTransport::effectiveTimeoutMs(-5), 1);
    QCOMPARE(LibusbOdinTransport::effectiveTimeoutMs(3000), 3000);
}

QTEST_APPLESS_MAIN(TestOdinLibusbTransport)
#include "test_odin_libusb_transport.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**：注册 CMake（列表末尾追加 `test_odin_libusb_transport.cpp`；分支 `odin_libusb_transport.cpp` + `odin_session.cpp` + `odin_protocol.cpp` + `samsung_plan.cpp` + `pit.cpp` + `tar_image.cpp`；**并把它加进 libusb 链接那行**（`if(_test_stem STREQUAL "test_mtk_brom" OR ... )` 的那个 `if`））→ 编译失败（头文件不存在）。

- [ ] **Step 3: 实现**

结构照抄 `src/core/edl/edl_libusb_transport.cpp`（本仓已验证的同款实现），要点：
- `open()`：`libusb_init` → `get_device_list` → 对 `vid == 0x04E8` 的候选读**活动配置描述符**收接口类 + 批量端点 → `isOdinDevice(...)` 命中则 `libusb_open` + claim（失败先 `detach_kernel_driver` 再 claim）+ 从描述符取端点与 `wMaxPacketSize`（取不到退回常量）。
- **接口号与端点不写死**：Odin 设备的接口号/端点在描述符里（Thor/odin4 都是遍历取），不照搬 Heimdall 的固定值。
- `write()`：空数组 = ZLP（超时 100 ms，`odin4-llucs odin_protocol.cpp:294-302`）；非空走 bulk，**短写必须报错**（数据面继续发下一片会错位）。
- `read()`：`timeoutMs <= 0` → `effectiveTimeoutMs` 换算后短读，`LIBUSB_ERROR_TIMEOUT` 且请求为轮询时**空返回且不置 error**。
- ⚠️ **接口已收窄（Task 6 落地时按审查收窄，以落盘的 `src/core/odin/odin_transport.h` 为准）**：
  `IOdinTransport` **只有 `open` / `close` / `write` / `read` 四个方法** —— 原计划的 `reset()` 与 `maxPacketSize()` 已删除
  （会话从不调用它们：设备重启走协议命令 `0x67/0x01`，不经 USB 复位；ZLP 由会话的显式空写表达，不靠包长判定）。
  **不要**再实现这两个方法（会编译不过）。

- [ ] **Step 4: 跑测试，确认通过**：`./build/image_engine_tests_test_odin_libusb_transport && ctest --test-dir build --output-on-failure` → 全绿，44/44。

- [ ] **Step 5: 提交**

```bash
git add src/core/odin/odin_libusb_transport.h src/core/odin/odin_libusb_transport.cpp tests/test_odin_libusb_transport.cpp CMakeLists.txt
git commit -m "feat(odin): libusb 真机传输（CDC_DATA 类匹配 + 老 PID 兜底 + timeout=0 换算）"
```

---

### Task 8: 设备检测 + `flash_tool` 第 5 通道

**Files:**
- Modify: `src/core/device_detector.h`（`MODE_SAMSUNG_ODIN = 10`）、`src/core/device_detector.cpp`（显示名 + 检测）
- Modify: `src/core/flash_tool.h`（params 注释 + 通道说明）、`src/core/flash_tool.cpp`（`flashChannelForMode` + `flashFullPackage` 分支）
- Modify: `tests/test_pipeline.cpp`（通道映射断言）

**Interfaces:**
- Consumes: `odin::LibusbOdinTransport::isOdinDevice`（Task 7）、`odin::parsePitFile` / `odin::loadPitFromPackage` / `odin::buildSamsungPlan`（Task 3/4）、`odin::OdinSession` / `OdinOptions` / `OdinProgress`（Task 6）
- Produces: `FlashTool::flashChannelForMode(MODE_SAMSUNG_ODIN) == "samsung-odin"`；`flashFullPackage` 的 `samsung-odin` 分支（params：`tarMd5Files`(QStringList) + `pitPath`(可选)）；`DeviceDetector::MODE_SAMSUNG_ODIN` 与显示名「三星 (Odin)」

- [ ] **Step 1: 写失败用例（`tests/test_pipeline.cpp`）**

类声明里加一行 `void samsungOdinChannel();`，并在 `void TestPipeline::channelMapping()` 之后加：

```cpp
// Phase C：三星 Odin 通道 —— 通道名 + 是否整包通道（不含 EDL 的"分区刷写"例外）
void TestPipeline::samsungOdinChannel()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_SAMSUNG_ODIN),
             QStringLiteral("samsung-odin"));
    QVERIFY(FlashTool::isPackageChannelMode(DeviceDetector::MODE_SAMSUNG_ODIN));
    // 缺 tarMd5Files 时必须**明确报错**（不能让通道分支静默 return 掉）
    FlashTool tool;
    QString err;
    QVERIFY(!tool.flashFullPackage(QStringLiteral("usb-1-2"), DeviceDetector::MODE_SAMSUNG_ODIN,
                                  QVariantMap(), &err));
    QVERIFY(err.contains(QStringLiteral("tarMd5Files")));
}
```

- [ ] **Step 2: 跑一遍，确认失败**：`cmake --build build --target image_engine_tests_test_pipeline` → `MODE_SAMSUNG_ODIN` 未声明 → 编译失败。

- [ ] **Step 3: 改 `device_detector.{h,cpp}`**

`device_detector.h`：枚举里加

```cpp
        MODE_SPD = 9,               // 展锐 ResearchDownload（F4）
        MODE_SAMSUNG_ODIN = 10      // 三星 Odin 下载模式（Phase C）
```

`device_detector.cpp` 的 `getModeDisplayName` 加：

```cpp
    case MODE_SAMSUNG_ODIN:
        return QStringLiteral("三星 (Odin)");
```

`detectProtocolDevices()`：在内层遍历之前插入三星分支（`desc` 已读到）：

```cpp
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;

        // 三星 Odin（Phase C）：判据是**纯函数**（odin_libusb_transport.h 的 isOdinDevice）——
        // VID 0x04E8 + 接口类 0x0A(CDC_DATA) + 批量 in/out（Thor/odin4 现行做法），老 PID 兜底。
        // 只对三星 VID 深挖描述符：每 2s 一轮的枚举里，其余厂商不必多读一次配置描述符。
        if (desc.idVendor == 0x04E8) {
            QList<quint8> classes;
            bool bulkIn = false, bulkOut = false;
            libusb_config_descriptor *cfg = nullptr;
            if (libusb_get_active_config_descriptor(list[i], &cfg) == LIBUSB_SUCCESS && cfg) {
                for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
                    for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                        const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                        classes << quint8(alt->bInterfaceClass);
                        for (int e = 0; e < int(alt->bNumEndpoints); ++e) {
                            const libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                                continue;
                            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) bulkIn = true; else bulkOut = true;
                        }
                    }
                }
                libusb_free_config_descriptor(cfg);
            }
            if (odin::LibusbOdinTransport::isOdinDevice(desc.idVendor, desc.idProduct,
                                                        classes, bulkIn && bulkOut)) {
                const QString devId = QStringLiteral("usb-%1-%2")
                                          .arg(libusb_get_bus_number(list[i]))
                                          .arg(libusb_get_device_address(list[i]));
                DeviceInfo info;
                info.serialNumber = devId;
                info.mode = MODE_SAMSUNG_ODIN;
                info.model = getModeDisplayName(MODE_SAMSUNG_ODIN);
                newDevices[devId] = info;
                if (!m_currentDevices.contains(devId)) {
                    emit deviceConnected(info);
                } else if (m_currentDevices[devId].mode != MODE_SAMSUNG_ODIN) {
                    emit deviceModeChanged(devId, MODE_SAMSUNG_ODIN);
                }
                continue;                       // 已认领，不再走下面的 PID 表
            }
        }

        for (const ProtoId &id : ids) {
            // 既有 PID 表逻辑（0x0E8D:0x0003 → MTK BROM / 0x12D1 → 华为 / 0x1782 → 展锐）**原样保留**，
            // 本任务只在它之前插入三星分支
        }
    }
```

并在 `device_detector.cpp` 顶部加 `#include "core/odin/odin_libusb_transport.h"`（`libusb.h` 已包含）。

- [ ] **Step 4: 改 `flash_tool.{h,cpp}`**

`flash_tool.h`：`flashFullPackage` 的 params 注释加一行：

```cpp
    //   samsung-odin:        tarMd5Files(QStringList，BL/AP/CP/CSC 的 .tar.md5) + pitPath(可选；
    //                        缺省用包内 .pit —— 找不到包内 .pit 且未显式指定 → 明确报错)
```

`flash_tool.cpp`：`flashChannelForMode` 加：

```cpp
    case DeviceDetector::MODE_SAMSUNG_ODIN:
        return QStringLiteral("samsung-odin");
```

`flashFullPackage` 在 `oppo-edl` 分支之后、末尾裸 `return false` 之前插入（并把末尾注释里的"四字面量"改成"五字面量"）：

```cpp
    if (channel == QStringLiteral("samsung-odin")) {
        // Phase C 通道：BL/AP/CP/CSC 的 .tar.md5 → PIT（包内优先）→ OdinSession（握手 → 读设备 PIT
        // → 对账 → 逐分区写入 → 结束会话/重启）。**设备须处于 Odin 下载模式**（长按音量下+Home+电源）。
        const QStringList tars = params.value(QStringLiteral("tarMd5Files")).toStringList();
        if (tars.isEmpty()) {
            if (error)
                *error = QStringLiteral("缺少固件包列表（samsung-odin 通道的 tarMd5Files）—— "
                                        "整包刷写请用界面「刷入」（选中三星 Odin 设备后会自动弹选包对话框）");
            return false;
        }
        const QString pitPath = params.value(QStringLiteral("pitPath")).toString();

        odin::PitTable pit;
        QString pitSource;
        if (!pitPath.isEmpty()) {
            QString pitErr;
            if (!odin::parsePitFile(pitPath, pit, &pitErr)) {
                if (error) *error = QStringLiteral("读取 PIT 失败：%1").arg(pitErr);
                return false;
            }
            pitSource = QStringLiteral("用户指定 PIT：%1").arg(QFileInfo(pitPath).fileName());
        } else {
            QString pitDesc;
            QString pitErr;
            if (!odin::loadPitFromPackage(tars, pit, &pitDesc, &pitErr)) {
                if (error) *error = pitErr;
                return false;
            }
            pitSource = pitDesc;
        }

        odin::SamsungPlan plan;
        QString planErr;
        if (!odin::buildSamsungPlan(tars, pit, plan, &planErr, pitSource)) {
            if (error) *error = QStringLiteral("构建刷写计划失败：%1").arg(planErr);
            return false;
        }
        for (const QString &w : plan.warnings)
            emit outputMessage(QStringLiteral("[计划] %1").arg(w), false);
        // 包完整性：构建阶段已校验并记进 warnings；这里把结论再点一次（不阻断 —— 改包刷写在场景里常见）
        for (const odin::SamsungPlanFile &f : std::as_const(plan.files))
            if (!f.verifyOk)
                emit outputMessage(QStringLiteral("[校验] %1 未通过 MD5 校验（%2）")
                                       .arg(QFileInfo(f.path).fileName(),
                                            f.md5HasFooter ? QStringLiteral("校验行不符")
                                                           : QStringLiteral("无校验行")), false);

        // 多设备防护（同既有四通道）：LibusbOdinTransport::open 取首个匹配设备
        const int samsungCount = countDevicesOfVid(0x04E8, nullptr);
        if (samsungCount > 1)
            emit outputMessage(QStringLiteral(
                "检测到 %1 台三星 USB 设备，将刷写首个进入 Odin 模式的设备（完整设备选择器为后续任务）")
                .arg(samsungCount), false);
        emit outputMessage(QStringLiteral("三星 Odin 刷写通道：%1（%2，%3 条条目）")
                               .arg(deviceId, plan.pitSource).arg(plan.entries.size()), false);

        odin::LibusbOdinTransport transport;
        QString lastProgressKey;
        odin::OdinSession session(transport, [this, &lastProgressKey](const odin::OdinProgress &p) {
            const QString key = p.stage + QLatin1Char('\x1f') + p.detail;
            if (key != lastProgressKey) {
                lastProgressKey = key;
                if (!p.detail.isEmpty())
                    emit outputMessage(QStringLiteral("[%1] %2").arg(p.stage, p.detail), false);
            }
            emit flashProgress(p.percent);
        });
        odin::OdinOptions opt;
        QString runErr;
        if (!session.run(plan, opt, &runErr)) {
            if (error) *error = runErr.isEmpty() ? QStringLiteral("三星 Odin 刷写失败") : runErr;
            return false;
        }
        emit flashProgress(100);
        emit outputMessage(QStringLiteral("三星 Odin 刷写完成"), false);
        return true;
    }
```

`flash_tool.cpp` 顶部加 `#include "core/odin/odin_libusb_transport.h"`、`#include "core/odin/odin_session.h"`、`#include "core/odin/samsung_plan.h"`。

- [ ] **Step 5: 跑测试，确认通过**

```bash
cmake -B build -G Ninja && cmake --build build
./build/image_engine_tests_test_pipeline && ctest --test-dir build --output-on-failure
```
预期：`test_pipeline` 全绿（既有通道断言一并回归）；全量 **44/44**（本任务不新增测试目标，只改既有 `test_pipeline`）。

- [ ] **Step 6: 提交**

```bash
git add src/core/device_detector.h src/core/device_detector.cpp src/core/flash_tool.h src/core/flash_tool.cpp tests/test_pipeline.cpp
git commit -m "feat(odin): 三星设备检测（CDC_DATA 类匹配）+ flash_tool 第 5 通道 samsung-odin"
```

---

### Task 9: UI 预览（抽出通用控件 + 三星对话框 + FlashPanel 接线）

**Files:**
- Create: `src/ui/plan_preview_widget.{h,cpp}`（从 `flash_plan_dialog.cpp` 抽出的预览骨架）
- Modify: `src/ui/flash_plan_dialog.{h,cpp}`（改用该控件；**公开 API / 信号 / 控件 objectName 一律不变**）
- Create: `src/ui/samsung_plan_dialog.{h,cpp}`
- Modify: `src/ui/flash_panel.cpp`（模式名 / 协议通道早退列表 / 「刷入」分派）
- Create: `tests/test_samsung_plan_dialog.cpp`
- Modify: `CMakeLists.txt`（两个对话框测试目标都要加 `plan_preview_widget.cpp`）

**Interfaces:**
- Consumes: `odin::SamsungPlan`（Task 4）、`odin::parsePitFile` / `odin::loadPitFromPackage` / `odin::buildSamsungPlan`
- Produces:
  ```cpp
  // src/ui/plan_preview_widget.h
  inline QString planBytesText(quint64 bytes);                  // "8 KiB" 这类文案（两个对话框共用）
  class PlanPreviewWidget : public QWidget {
  public:
      PlanPreviewWidget(const QStringList &headers, const QList<QStringList> &rows,
                        const QString &summaryHtml, const QStringList &warnings,
                        const QString &ackText, QWidget *parent = nullptr);
      void addWarnings(const QStringList &warnings);
      void setColumnTooltips(int column, const QStringList &tips);
      bool acknowledged() const;
  signals:
      void startRequested();      // 点「开始刷写」（勾选框由控件自己门控）
      void cancelRequested();     // 点「取消」→ 调用方 reject()
  };
  // src/ui/samsung_plan_dialog.h
  class SamsungPlanDialog : public QDialog {
  public:
      explicit SamsungPlanDialog(const odin::SamsungPlan &plan, QWidget *parent = nullptr);
      bool confirmed() const;
      void accept() override;
      static bool buildAndShow(const QStringList &tarMd5Files, const QString &pitPath,
                               QWidget *parent, QString *error);   // false + *error 空 = 用户取消
  };
  ```

**回归守卫**：`tests/test_flash_plan_dialog.cpp` **不得修改**，抽取后必须原样全绿（它断言 `planTable`（7 列）/ `warningsList` / `ackCheck` / `startButton` 四个 objectName 与门控语义）。

- [ ] **Step 1: 抽出 `src/ui/plan_preview_widget.{h,cpp}`**

内容 = `flash_plan_dialog.cpp` 现 `buildUi()` 里的通用部分，行为逐条保留（`planTable`/`warningsList`/`ackCheck`/`startButton` 四个 objectName；勾选框门控"未勾选则开始按钮禁用"；告警为空时标题与列表都隐藏）。两处**有意的**差异：
1. **告警区渲染在表格之上** —— 三星侧的两类不匹配（PIT 有而包内无 / 包内有而 PIT 无）是最该先看到的信息（spec §4「warnings 在预览里置顶」）；Phase B 的对话框一并受益（其用例只断言可见性，不断言顺序）。
2. 表格内容与摘要由调用方传入（控件不解析任何计划模型）；tooltip 走 `setColumnTooltips`。

`plan_preview_widget.h`：

```cpp
// src/ui/plan_preview_widget.h
//
// 计划预览的通用控件（从 Phase B 的 FlashPlanDialog 抽出）。职责三条：
//   ① 把"要写什么"摊开（摘要 + 告警 + 表格）；② 用勾选框把"此路径真机未验证"变成**显式确认**
//   （未勾选时「开始刷写」禁用）；③ 把"开始/取消"两个动作以信号抛出（谁接谁负责）。
// 不解析任何计划模型 —— 表格内容由调用方翻译好传进来。
#ifndef PLAN_PREVIEW_WIDGET_H
#define PLAN_PREVIEW_WIDGET_H

#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStandardItemModel;
class QTableView;

// 字节数人性化（两个对话框共用）
inline QString planBytesText(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    if (bytes >= 1024ull * 1024)
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    if (bytes >= 1024ull)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

class PlanPreviewWidget : public QWidget
{
    Q_OBJECT
public:
    // headers/rows = 表格内容（行数 = rows.size()，列数 = headers.size()）；
    // summaryHtml = 顶部摘要（RichText）；warnings = 告警（**渲染在表格之上**）；
    // ackText = 勾选框文案。
    PlanPreviewWidget(const QStringList &headers, const QList<QStringList> &rows,
                      const QString &summaryHtml, const QStringList &warnings,
                      const QString &ackText, QWidget *parent = nullptr);

    void addWarnings(const QStringList &warnings);            // 只追加、不去重
    void setColumnTooltips(int column, const QStringList &tips); // 逐行 tooltip（不足的行不给）
    bool acknowledged() const;

signals:
    void startRequested();     // 用户点了「开始刷写」（勾选框已由控件门控）
    void cancelRequested();    // 用户点了「取消」→ 调用方 reject()

private:
    void refreshWarnings();

    QLabel *m_summaryLabel = nullptr;
    QLabel *m_warningsTitle = nullptr;
    QListWidget *m_warningsList = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QCheckBox *m_ackCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QStringList m_warnings;
};

#endif // PLAN_PREVIEW_WIDGET_H
```

`plan_preview_widget.cpp`：

```cpp
#include "plan_preview_widget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

PlanPreviewWidget::PlanPreviewWidget(const QStringList &headers, const QList<QStringList> &rows,
                                     const QString &summaryHtml, const QStringList &warnings,
                                     const QString &ackText, QWidget *parent)
    : QWidget(parent), m_warnings(warnings)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setTextFormat(Qt::RichText);
    m_summaryLabel->setText(summaryHtml);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    // 告警在表格**之上**：两类不匹配（PIT 有而包内无 / 包内有而 PIT 无）是最该先看到的信息
    // （三星 spec §4「warnings 在预览里置顶」）；条目多时表格会把下方的告警挤出屏幕。
    m_warningsTitle = new QLabel(QStringLiteral("告警"), this);
    layout->addWidget(m_warningsTitle);
    m_warningsList = new QListWidget(this);
    m_warningsList->setObjectName(QStringLiteral("warningsList"));
    m_warningsList->setMaximumHeight(110);
    layout->addWidget(m_warningsList);
    refreshWarnings();

    m_model = new QStandardItemModel(int(rows.size()), int(headers.size()), this);
    m_model->setHorizontalHeaderLabels(headers);
    for (int row = 0; row < rows.size(); ++row) {
        const QStringList &cells = rows.at(row);
        for (int col = 0; col < headers.size(); ++col) {
            QStandardItem *item = new QStandardItem(col < cells.size() ? cells.at(col) : QString());
            item->setEditable(false);
            m_model->setItem(row, col, item);
        }
    }

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("planTable"));
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->resizeColumnsToContents();
    layout->addWidget(m_table, 1);

    // 真机未验证告知：勾选前「开始刷写」保持禁用（默认停手，不是默认开刷）
    m_ackCheck = new QCheckBox(ackText, this);
    m_ackCheck->setObjectName(QStringLiteral("ackCheck"));
    layout->addWidget(m_ackCheck);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_startButton = new QPushButton(QStringLiteral("开始刷写"), this);
    m_startButton->setObjectName(QStringLiteral("startButton"));
    m_startButton->setEnabled(false);
    QPushButton *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);

    connect(m_ackCheck, &QCheckBox::toggled, m_startButton, &QPushButton::setEnabled);
    connect(m_startButton, &QPushButton::clicked, this, &PlanPreviewWidget::startRequested);
    connect(cancelButton, &QPushButton::clicked, this, &PlanPreviewWidget::cancelRequested);
}

void PlanPreviewWidget::refreshWarnings()
{
    m_warningsList->clear();
    m_warningsList->addItems(m_warnings);
    const bool any = !m_warnings.isEmpty();
    m_warningsTitle->setVisible(any);
    m_warningsList->setVisible(any);
}

void PlanPreviewWidget::addWarnings(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return;
    m_warnings.append(warnings);
    refreshWarnings();
}

void PlanPreviewWidget::setColumnTooltips(int column, const QStringList &tips)
{
    if (column < 0 || column >= m_model->columnCount())
        return;
    const int rows = qMin(int(tips.size()), m_model->rowCount());
    for (int row = 0; row < rows; ++row) {
        if (tips.at(row).isEmpty())
            continue;
        if (QStandardItem *item = m_model->item(row, column))
            item->setToolTip(tips.at(row));
    }
}

bool PlanPreviewWidget::acknowledged() const
{
    return m_ackCheck->isChecked();
}
```

- [ ] **Step 2: 改 `flash_plan_dialog.{h,cpp}`**

头文件：删掉 `m_model/m_table/m_summaryLabel/m_warningsTitle/m_warningsList/m_ackCheck/m_startButton` 与 `refreshWarnings()`，换成 `PlanPreviewWidget *m_preview = nullptr;`（前置声明 `class PlanPreviewWidget;`），保留 `m_planDir/m_confirmed` 与全部公开 API、`startRequested` 信号。

`buildUi(plan)` 改成：装配 7 列 headers、把 `plan.entries` 翻译成 rows（`entryName`/`lun`/`entryStart`/扇区数/`entryBytes`/文件名/`sha256` 前 12 位）、tips（真实路径给完整路径，`DISK`/空不给）、摘要 HTML，构造控件并接线：

```cpp
    m_preview = new PlanPreviewWidget(headers, rows, summary, plan.warnings,
                                      QStringLiteral("我知晓此路径真机未验证"), this);
    m_preview->setColumnTooltips(5, tips);
    layout->addWidget(m_preview);
    connect(m_preview, &PlanPreviewWidget::startRequested, this, [this]() {
        emit startRequested(m_planDir);
        accept();
    });
    connect(m_preview, &PlanPreviewWidget::cancelRequested, this, &FlashPlanDialog::reject);
```

`addWarnings(list)` 改成转发给 `m_preview->addWarnings(list)`；`humanBytes` 删掉，改调 `planBytesText`（`entryBytes` 保留）。

- [ ] **Step 3: 跑回归（**必须原样全绿**）**

```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_flash_plan_dialog
./build/image_engine_tests_test_flash_plan_dialog
```
预期：8/8 通过，**测试文件一行未改**。若红了 → 抽取改变了行为，按测试的期望修实现（不得改测试）。

- [ ] **Step 4: 写 `src/ui/samsung_plan_dialog.{h,cpp}`**

按 Interfaces。表格列：`分区 / 镜像 / 大小 / 来源包 / 匹配规则`（5 列）；摘要：`<b>PIT：</b>%1　<b>包：</b>%2　<b>条目：</b>%3　<b>总字节：</b>%4`（`pitSource` / `files.size()` / `entries.size()` / `planBytesText(totalBytes)`）；勾选框文案「我知晓此路径真机未验证（设备侧未联调）」；`buildAndShow` 按 Interfaces 实现（PIT 显式优先 → 否则包内；构建失败 → `*error` 非空；用户取消 → `error->clear()`）。

- [ ] **Step 5: 写用例 `tests/test_samsung_plan_dialog.cpp`**

```cpp
// tests/test_samsung_plan_dialog.cpp
//
// 三星计划预览对话框：确认门控 + 预览内容（条目进表、两类不匹配进告警）。
// offscreen 跑（CMakeLists 的 ENVIRONMENT 与 test_flash_plan_dialog 同款），只断言控件状态。
#include <QtTest>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>

#include "core/odin/samsung_plan.h"
#include "ui/samsung_plan_dialog.h"

class TestSamsungPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
    void rejectLeavesConfirmedFalse();
    void previewsEntriesAndWarnings();
};

static odin::SamsungPlan oneEntryPlan()
{
    odin::SamsungPlan plan;
    plan.pitSource = QStringLiteral("包内 CSC.tar.md5 的 J1POP3G.pit");
    odin::SamsungPlanFile f;
    f.path = QStringLiteral("/tmp/pack/CSC_ODD.tar.md5");
    f.sizeBytes = 12345;
    f.md5HasFooter = true;
    f.verifyOk = true;
    plan.files << f;
    odin::SamsungPlanEntry e;
    e.partition = QStringLiteral("BOOT");
    e.imageFile = QStringLiteral("spl.img");
    e.sizeBytes = 32768;
    e.sourceOffset = 1024;
    e.fileIndex = 0;
    e.matchRule = QStringLiteral("文件名精确匹配");
    e.pit.partitionName = QStringLiteral("BOOT");
    e.pit.identifier = 80;
    plan.entries << e;
    plan.totalBytes = 32768;
    return plan;
}

void TestSamsungPlanDialog::startButtonGatedByCheckbox()
{
    SamsungPlanDialog dlg(oneEntryPlan());
    auto *start = dlg.findChild<QPushButton *>(QStringLiteral("startButton"));
    auto *ack = dlg.findChild<QCheckBox *>(QStringLiteral("ackCheck"));
    QVERIFY(start && ack);
    QVERIFY(!ack->isChecked());
    QVERIFY(!start->isEnabled());            // 未勾选 → 不得开刷
    ack->setChecked(true);
    QVERIFY(start->isEnabled());
    QVERIFY(!dlg.confirmed());
    dlg.accept();
    QVERIFY(dlg.confirmed());
    ack->setChecked(false);                  // 反向门控（防"恒 true"的假实现）
    QVERIFY(!start->isEnabled());
}

void TestSamsungPlanDialog::rejectLeavesConfirmedFalse()
{
    SamsungPlanDialog dlg(oneEntryPlan());
    dlg.reject();
    QVERIFY(!dlg.confirmed());
}

void TestSamsungPlanDialog::previewsEntriesAndWarnings()
{
    odin::SamsungPlan plan = oneEntryPlan();
    plan.warnings << QStringLiteral("PIT 条目 BOOT2 声明的镜像 spl2.img 不在所选包内（跳过）")
                  << QStringLiteral("包内镜像 extra.bin 未出现在 PIT 中（跳过）");
    SamsungPlanDialog dlg(plan);
    auto *table = dlg.findChild<QTableView *>(QStringLiteral("planTable"));
    QVERIFY(table && table->model());
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->columnCount(), 5);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("BOOT"));
    QCOMPARE(table->model()->index(0, 1).data().toString(), QStringLiteral("spl.img"));
    QCOMPARE(table->model()->index(0, 2).data().toString(), QStringLiteral("32 KiB"));
    QCOMPARE(table->model()->index(0, 3).data().toString(), QStringLiteral("CSC_ODD.tar.md5"));
    QCOMPARE(table->model()->index(0, 4).data().toString(), QStringLiteral("文件名精确匹配"));
    auto *warn = dlg.findChild<QListWidget *>(QStringLiteral("warningsList"));
    QVERIFY(warn);
    QCOMPARE(warn->count(), 2);              // 两类不匹配都在预览里（不静默）
    QVERIFY(warn->isVisibleTo(&dlg));
}

QTEST_MAIN(TestSamsungPlanDialog)
#include "test_samsung_plan_dialog.moc"
```

- [ ] **Step 6: 改 `src/ui/flash_panel.cpp`（三处 + 一行 include）**

① 顶部 include 加 `#include "samsung_plan_dialog.h"`。

② 模式名 switch（`case DeviceDetector::MODE_SPD:` 之后）加：

```cpp
    case DeviceDetector::MODE_SAMSUNG_ODIN:
        modeStr = "三星 (Odin)"; break;
```

③ 协议通道早退（约 :352 的 `if (m_deviceInfo.mode == ... MODE_SPD)`）里加一个条件，并把 tooltip 文案改成覆盖新通道：

```cpp
    if (m_deviceInfo.mode == DeviceDetector::MODE_MTK_BROM ||
        m_deviceInfo.mode == DeviceDetector::MODE_HUAWEI_USB_UPDATE ||
        m_deviceInfo.mode == DeviceDetector::MODE_SPD ||
        m_deviceInfo.mode == DeviceDetector::MODE_SAMSUNG_ODIN) {
        m_partitionList->clear();
        m_partitions.clear();
        m_flashBtn->setEnabled(true);
        m_flashBtn->setToolTip(QStringLiteral("协议通道整包刷写（按模式选择 update.app / pac+FDL / DA / 三星 tar.md5）"));
        return;
    }
```

④ `onFlashClicked` 的通道分派里，在 `} else if (channel == QStringLiteral("mtk-brom")) { ... }` 之后追加：

```cpp
        } else if (channel == QStringLiteral("samsung-odin")) {
            // Phase C：选包 → 计划预览（含两类不匹配告警 + 未验证勾选）→ 确认后把**文件列表**
            // 交给通道（通道内自行解析 PIT 并重建计划 —— 与 oppo-edl 通道传 planDir 同款口径）
            const QStringList tars = QFileDialog::getOpenFileNames(
                this, QStringLiteral("选择三星固件包（BL/AP/CP/CSC 的 .tar.md5，可多选）"), QString(),
                QStringLiteral("三星固件包 (*.tar.md5 *.tar);;所有文件 (*)"));
            if (tars.isEmpty()) return;
            QString pitPath;
            const QMessageBox::StandardButton wantPit = QMessageBox::question(
                this, QStringLiteral("PIT"),
                QStringLiteral("是否显式指定 PIT 文件？\n\n"
                               "选「否」用包内 .pit（推荐，通常来自 CSC 包）；\n"
                               "包内没有 .pit 时必须选「是」并指定文件。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (wantPit == QMessageBox::Yes) {
                pitPath = QFileDialog::getOpenFileName(this, QStringLiteral("选择 PIT 文件"), QString(),
                                                       QStringLiteral("PIT (*.pit);;所有文件 (*)"));
                if (pitPath.isEmpty()) return;
            }
            QString planErr;
            if (!SamsungPlanDialog::buildAndShow(tars, pitPath, this, &planErr)) {
                if (!planErr.isEmpty())
                    emit outputMessage(QStringLiteral("三星刷写计划构建失败：%1").arg(planErr), true);
                return;                     // *error 空 = 用户取消（静默返回，与 EDL 计划入口同口径）
            }
            params.insert(QStringLiteral("tarMd5Files"), tars);
            if (!pitPath.isEmpty())
                params.insert(QStringLiteral("pitPath"), pitPath);
        }
```

⑤ 同一函数末尾的完成文案：`mtk-brom` 那条特例之后，`else` 分支已输出"刷写完成" —— 三星通道沿用即可（无需改）。

- [ ] **Step 7: 注册测试 + 跑**

CMake：列表末尾追加 `test_samsung_plan_dialog.cpp`；分支加

```cmake
                elseif(_test_stem STREQUAL "test_samsung_plan_dialog")
                    # Phase C Task 9：三星计划对话框（src/ui，不编入任何静态库）+ 通用预览控件
                    # + 计划层/PIT/tar 索引。Qt6::Widgets 与 offscreen 见下方 if 块。
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/samsung_plan_dialog.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/plan_preview_widget.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/samsung_plan.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/odin/pit.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/image_engine/tar_image.cpp)
```

并把 `test_flash_plan_dialog` 的 extra sources 加上 `plan_preview_widget.cpp`，把两个对话框测试一起纳入 Widgets + offscreen 的那个 `if`：

```cmake
                if(_test_stem STREQUAL "test_flash_plan_dialog" OR _test_stem STREQUAL "test_samsung_plan_dialog")
                    target_link_libraries(${_test_target} PRIVATE Qt6::Widgets)
                    set_tests_properties(${_test_target} PROPERTIES
                        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=")
                endif()
```

```bash
cmake -B build -G Ninja && cmake --build build
./build/image_engine_tests_test_samsung_plan_dialog
QT_QPA_PLATFORM=offscreen ./build/image_engine_tests_test_samsung_plan_dialog
ctest --test-dir build --output-on-failure
```
预期：两个对话框测试全绿；全量 **45/45**。

- [ ] **Step 8: GUI 启动冒烟（offscreen，不做交互）**

```bash
QT_QPA_PLATFORM=offscreen timeout 20 ./build/PhoneToolbox; echo "exit=$?"
```
预期：`exit=124`（超时被杀 = 起来了没崩）或用户手动关闭；**不是**崩溃/断言退出。结果如实贴进报告（这就是"GUI 只到启动冒烟"的边界）。

- [ ] **Step 9: 提交**

```bash
git add src/ui/plan_preview_widget.h src/ui/plan_preview_widget.cpp src/ui/samsung_plan_dialog.h src/ui/samsung_plan_dialog.cpp src/ui/flash_plan_dialog.h src/ui/flash_plan_dialog.cpp src/ui/flash_panel.cpp tests/test_samsung_plan_dialog.cpp CMakeLists.txt
git commit -m "feat(odin): 三星刷写计划预览（抽出通用预览控件 + 未验证勾选门控 + FlashPanel 接线）"
```

---

### Task 10: 文档与诚实边界

**Files:**
- Modify: `功能清单.txt`（第 296 行那条「三星 Heimdall 集成」+ 新增 Phase C 小节）
- Modify: `README.md`（支持的设备模式 + 目录结构）
- Modify: `docs/superpowers/specs/samsung-odin-facts.md`（追加"本期裁定与实现落点"一节）

- [ ] **Step 1: `功能清单.txt`**

① 第 296 行 `[ ] 三星 Heimdall 集成 (Odin 协议开源实现)` 改为：

```
[√] 三星 Heimdall 集成 (Odin 协议开源实现) —— Phase C 代码就绪: PIT 解析/计划层/协议/会话/通道/UI, mock 验证; **真机验证待持机人**
```

② 在「--- OPPO/一加/realme EDL 刷写 (Phase B 自研) ---」小节之后插入：

```
--- 三星 Odin 刷写 (Phase C 自研) ---
[√] PIT 解析 (28B 头 + 132B 条目 + 尾部签名容忍 + 字符串清洗; 10 个真 PIT 硬断言)
[√] 刷写计划层 (tar.md5 流式索引 + 以 PIT 条目文件名为准的匹配 + .pit 唯一性回退 + 双向不匹配告警)
[√] Odin 协议层 (1024B 控制包 / 会话协商 / 总字节 u64 / 逐分区数据面 / ACK 与负码文案; 三方对照逐条带出处)
[√] 刷写会话 (握手 → 起会话 → 读设备 PIT → **以设备 PIT 为准**对账 → 逐分区写入 → 结束/重启)
[√] 真机传输 (VID 0x04E8 + CDC_DATA 类匹配 + 老 PID 兜底; timeout=0 轮询换算)
[√] 设备检测条目「三星 (Odin)」+ FlashTool 第 5 通道 samsung-odin + 计划预览对话框 (未验证勾选门控)
[√] 前置修复: 三星 .tar.md5 校验行接受 "␣*" 分隔符 (真 MODEM 包此前静默跳过 MD5 校验)
[√] 诚实边界: 真机全链未验证 (USB 时序/类匹配实际枚举/真实 ACK/能否接受未签名镜像);
    设备侧读 PIT (dump) 无真机可验; repartition 不做; attributes/updateAttributes 不做语义解释;
    真包只覆盖一台老机型 (SM-J110H 展锐方案) 的 BL/CSC/MODEM, AP 包 (966MB) 未纳入
```

- [ ] **Step 2: `README.md`**

「支持的设备模式」小节加一行（与既有各模式同格式）：

```
- 三星 Odin 下载模式（VID 0x04E8 + CDC_DATA 接口类）→ 按 PIT 刷写 BL/AP/CP/CSC 的 .tar.md5（Phase C，真机未验证）
```

「目录结构」小节在既有 `src/core/edl/` 之类的条目旁补 `src/core/odin/`（6 个模块一句话说明）与 `src/ui/samsung_plan_dialog.*` + `src/ui/plan_preview_widget.*`。

- [ ] **Step 3: `docs/superpowers/specs/samsung-odin-facts.md` 追加一节**

在文件末尾追加：

```markdown
---

## 7. 本期裁定与实现落点（Phase C 实施后回填）

**三方冲突裁定表**：见实施计划 `docs/superpowers/plans/2026-09-14-samsung-heimdall-phase-c.md`
的「Global Constraints」（D1-D15，逐条给出三方取值、本期采用值与依据）。其中**未**采纳 Heimdall 的四处：
D3（起会话 payload）、D4（版本化协商）、D6/D7/D8（空传输时机）、D11（ACK 负码语义）；
**未**采纳 odin4 的两处：D10（modem 的 isLast 位置）、D14（512 对齐）。
**未裁定、留给持机人**：D13（`0x64/0x01` 在 Thor 是 ResetFlashCount、在 odin4 既是机型查询
又是 ResetFlashCount —— 本期只做一次 best-effort 机型查询、刷完不发重置）。

**实现落点**（便于持机人继续）：
| 事实 | 落点 |
|---|---|
| PIT 解析 | `src/core/odin/pit.cpp` |
| 计划层（含 .pit 唯一性回退） | `src/core/odin/samsung_plan.cpp` |
| 协议帧/ACK 判定 | `src/core/odin/odin_protocol.cpp` |
| 会话编排（读设备 PIT + 对账） | `src/core/odin/odin_session.cpp` |
| 真机传输（类匹配/端点/超时换算） | `src/core/odin/odin_libusb_transport.cpp` |
| 设备检测 / 通道 / UI | `src/core/device_detector.cpp`、`src/core/flash_tool.cpp`、`src/ui/samsung_plan_dialog.cpp` |
| 校验行 "␣*" 变体修复 | `src/image_engine/tar_image.cpp`（`scanMd5Footer` / `verifyMd5Footer`） |

**真包实证的告警口径**（与 spec §4 的差异，已在实现里落地）：
「分区大小与镜像大小**不符** → warning」按字面执行会在真包上产生 9 条无意义告警
（SM-J110H 的 10 条匹配里 9 条镜像小于分区、属正常）→ 收窄为：
**镜像 > 分区 → 逐条 warning（放不下）**；镜像 < 分区 → 一条汇总 warning；
分区未声明大小（blockCount=0）→ 逐条 warning。
```

- [ ] **Step 4: 提交**

```bash
git add 功能清单.txt README.md docs/superpowers/specs/samsung-odin-facts.md
git commit -m "docs: 三星 Odin 集成交付记录与诚实边界（功能清单/README/事实报告裁定回填）"
```

---

## 收尾（全部任务完成后）

- [ ] **全量验证**：`cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure`
      预期 **45/45 全绿**（基线 39/39 + 新增 6 个测试目标：test_pit / test_samsung_plan /
      test_odin_protocol / test_odin_session / test_odin_libusb_transport / test_samsung_plan_dialog）。
- [ ] **真样本证据复核**：`./build/image_engine_tests_test_pit && ./build/image_engine_tests_test_samsung_plan`
      —— 两个用例里的真样本断言**必须真跑**（输出不得出现 `SKIP`）；把输出贴进交付报告。
- [ ] **终审（whole-branch review）**后按发现分波修复，Minor 记账后再决定去留。
- [ ] **诚实边界复核**：逐一核对 spec §6 的边界是否在交付物里出现（真机未验 / dump 未验 / repartition 不做 / attributes 不解释 / 真包只覆盖一台老机型）。
- [ ] 用户手动验证一次（GUI 拖放/渲染、真机联调归持机人）。







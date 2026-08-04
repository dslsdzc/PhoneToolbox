# 镜像处理工具（Image Tool）设计文档

日期: 2026-08-04
状态: 待审查
范围: PhoneToolbox 新增独立镜像处理面板，全自研格式引擎 + 全格式覆盖

## 背景与动机

用户反馈当前刷机功能支持不足。经头脑风暴确定四个扩展方向（镜像处理、一键批量刷机、备份/恢复向导、更多协议），决定**镜像处理先行** —— 它是批量刷机的前置（解包才能匹配分区），且纯本地、不依赖设备连接。

2026 年 Root 生态调研结论：Magisk（修补 boot ramdisk）、KernelSU（LKM 修补 init_boot）、APatch（修补 boot 内核段）三家的共同地基都是 **boot 镜像解包/重打包引擎**，与 payload/super/sparse/tar 格式处理一起构成本工具的核心。

## 目标

1. **全格式原则**：所有能解包/打包的 Android 镜像格式全部支持（不仅限本矩阵，架构采用格式注册表模式，新增格式注册即用，不改面板代码）
2. 全自研实现（引擎与装配逻辑），注入物（magiskinit / kernelsu.ko / kpatch）运行时从官方渠道下载
3. 支持解包与打包双向操作
4. 支持 Magisk / KernelSU / APatch 三家 Root 修补
5. 支持增量 OTA diff 分区解包（需旧分区镜像）
6. 处理签名/校验层（Odin MD5、AVB footer、解锁提示）

## 格式覆盖矩阵

| 格式 | 来源 | 魔数/特征 | 支持操作 |
|------|------|-----------|---------|
| `payload.bin` | Pixel/一加/摩托等 OTA | `CrAU` | 解包（含 5 类分区特殊处理）、全量打包 |
| zip 刷机包 | 小米/Lineage/fastboot update | `PK\x03\x04` | 内层继续识别（Qt QZipReader） |
| 小米 `.tgz` | 小米/红米固件 | gzip tar | 解包 → 内层 .img 路由 |
| 三星 `.tar.md5` | 三星固件 | tar + 尾部 MD5 | 解包/重打包（重算 MD5 footer） |
| `.img` 集合 | 任意已解包 ROM | 按内部格式 | 识别并路由 |
| sparse 镜像 | 所有现代系统分区 | `0xED26FF3A` | 双向: simg2img / img2simg（4.0 格式） |
| super 分区 | 动态分区设备 | `0x414C5030`("0PLA") | 拆分逻辑分区 / 合并回 super |
| boot / init_boot | 内核+ramdisk（13+ 分离） | `ANDROID!` | header v0-v4 解析、解包/重打包 |
| vendor_boot | Pixel 系（vendor ramdisk） | `VNDRBOOT` | 同上 + vendor_ramdisk_table 关联 |
| `.br` 镜像 | 小米部分分区 | brotli magic | brotli 解压 → 内部再路由 |
| `.lz4/.xz/.gz/.zst` 裸压缩 | 各厂商散装镜像 | 各压缩魔数 | 解压/压缩 |
| dtbo/dtb/vbmeta | 设备树/AVB | `d\r\n`(dtbo) | 查看 + 打包联动（vbmeta 重算哈希） |
| `system.new.dat` 系 | Android 5-9 OTA | `transfer.list` 同目录 | 解包（.dat/.dat.gz/.dat.br + transfer.list 增量应用）/ 打包 |
| TWRP 备份 | TWRP 备份目录 | `.win` 头 | 解包（.win/.win001 分段合并）/ 打包 |
| MTK `.pac` | SP Flash Tool 整包 | 自描述头 | 解包（含分区描述）/ 打包 |
| EROFS 镜像 | Android 13+ 系统分区 | `\xe2\xe1\xf5\x00`(magic) | 目录浏览、文件提取、替换式重打包 |
| ext4 镜像 | Android 12- 系统分区 | `\x53\xef`(superblock) | 目录浏览、文件提取、替换式重打包 |
| LG KDZ / DZ | LG 官方固件 | v2/v3 容器头 | **仅解包**（KDZ→DZ→分区 chunk 按 eMMC 偏移合并；重打包经社区验证不可靠） |
| 华为 `update.app` | 华为安卓固件 | 512B 头 `0x55 0xAA` + 64B 文件表 | 解包/重打包（保持文件表顺序 + 数据段对齐，有工具参考） |
| 华为 `update.bin` | HarmonyOS 5+ | L2 型分区表 87B/条 | 解包（打包需官方签名） |
| 索尼 `.sin` | Xperia 固件 | v3 `03 53 49 4E`，块头 `LZ4A`/`ADDR` | **仅解包**（RSA 签名无法重签） |
| GPT/MBR 整盘镜像 | 全盘备份 | `EFI PART` / 55AA | 分区表解析、单分区提取 / 合成 |

注意: super 镜像魔数为 `0x414C5030`("0PLA")，geometry magic 为 `0x616c4467`("gDla")—— 早期草案中的 "LPML" 有误，已更正。

### 增量 OTA diff 分区解包

| 操作类型 | 含义 | 策略 |
|---------|------|------|
| `SOURCE_COPY` | 从旧分区复制块 | 直接实现 |
| `SOURCE_BSDIFF` | bspatch 打补丁 | 自研 bspatch |
| `BROTLI_BSDIFF` | brotli 压缩 bsdiff | bspatch + brotli 解压 |
| `PUFFDIFF` | Google gzip 差分 | 自研 puffpatch（puffin 格式） |
| `ZUCCHINI` | Chromium 差分 | **暂缓**（安卓 OTA 极少用） |

所有 diff 解包需要用户提供旧分区镜像，UI 检测到 diff 分区时提示选择源镜像。

### 签名/校验层（响应"三星签名"讨论）

- **Odin 刷写层**: 只校验尾部 MD5（完整性），重打包后重算即可刷入
- **Secure boot 层**: boot.img 内嵌厂商 RSA 签名 + AVB 链，无法伪造；**仅已解锁 Bootloader 设备可刷自定义镜像** —— UI 明确提示
- **AVB 处理**（自研 avbtool 核心逻辑）: boot/vendor_boot 重打包后剥离/重算 hash footer（AVB_FOOTER 32 字节 + VBMeta）、重生成 vbmeta（支持 `VERIFICATION_DISABLED` flag）

## 架构

```
src/image_engine/            # 纯格式库层 —— 无 UI 依赖、无网络依赖，可独立测试
  image_detector             # 魔数嗅探 + 格式路由
  sparse_image               # simg2img / img2simg
  super_image                # lp metadata 解析/生成, 逻辑分区拆分/合并
  boot_image                 # boot/init_boot/vendor_boot: header v0-v4, ramdisk
  payload_image              # payload.bin: protobuf wire 手写解析/序列化, 全量解包/打包
  bspatch_image              # bsdiff patch 应用 (SOURCE_BSDIFF/BROTLI_BSDIFF/PUFFDIFF)
  tar_image                  # tar 解包/打包, 三星 .tar.md5 MD5 footer
  avb_image                  # VBMeta 解析/生成, hash footer add/erase, 禁用验证
  dat_image                  # system.new.dat 系: transfer.list 解析 + 增量应用/生成
  twrp_image                 # TWRP .win 备份: 分段合并/拆分
  pac_image                  # MTK .pac 整包: 分区描述解析, 解包/打包
  kdz_image                  # LG KDZ/DZ: 容器头解析, DZ 分区 chunk 按 eMMC 偏移合并
  huawei_image               # update.app (512B 头 + 64B/条文件表, 含重打包) / update.bin (L2 分区表)
  sin_image                  # 索尼 sin v3: ADDR/LZ4A 块描述符 → raw
  disk_image                 # GPT/MBR 整盘: 分区表解析/单分区提取
  fs/                        # 文件系统镜像 (无 FUSE, 纯解析):
    fs_image                 #   FsImage 抽象接口: listFiles/extractFile/replaceFile/repack
    erofs_reader             #   EROFS 解析器 (目录/提取/替换重打包)
    ext4_reader              #   ext4 解析器 (extent tree/inline data/xattr)
  registry.h                 # 格式注册表: detect/unpack/pack/inspect 统一接口, 按魔数路由
  compression/               # zstd / lz4 / bzip2 / xz / brotli 封装层

src/root_patcher/            # 修补层 —— 自研装配逻辑
  RootPatcher (抽象接口)      #   patch(bootImage, config) → patchedImage, 自动备份原镜像
  magisk_patcher             # 下载 Magisk APK → 提取 magiskinit → 注入 ramdisk + init 链替换
  kernelsu_patcher           # 读设备 KMI → 下载匹配 kernelsu.ko → 注入 init_boot ramdisk
  apatch_patcher             # 下载 APatch APK → 提取 kpatch → 内核段注入
  assets_downloader          # 官方注入物下载器（版本缓存, 失败可手动指定本地文件）

src/ui/image_tool_panel      # 左侧第 5 个独立面板
```

分层原则: `image_engine` 纯库（不碰网络/UI），`root_patcher` 依赖它做修补，`image_tool_panel` 只做编排。

## 数据流与交互

整个面板是拖放目标（`setAcceptDrops(true)`，无单独"拖拽区"），文件拖到任意位置即识别；"打开文件"按钮兜底。

```
拖入文件 → 魔数嗅探 → 格式信息展示(分区列表/版本/元数据)
  → 用户选择动作:
      解包 → 选输出目录 → 进度条 → 完成(列出产物)
      打包 → 选输入目录/文件 → 自动识别组 → 生成
      转换 → sparse↔raw / 压缩↔解压
      修补 → 选 Root 方案(Magisk/KernelSU/APatch) → 下载注入物 → 输出 patched 镜像
      (diff 分区 → 提示选旧镜像 → 应用补丁)
```

UI 布局沿用现有面板风格: 格式识别信息卡、动作按钮组（按识别结果动态启用）、进度条 + 日志（OutputPanel 风格）。

## 错误处理

- 格式损坏 → 定位到具体结构（如 "lp metadata tables_checksum 不匹配"），不抛裸异常
- diff 分区缺旧镜像 → 明确提示需要的分区名，不静默跳过
- 注入物下载失败 → 可手动指定本地文件（APK / .ko / APatch APK）
- 修补/重打包前自动备份原镜像（`xxx.orig.bak`）
- 输出校验: 解包产物校验 SHA-256（payload manifest 自带 data_sha256_hash）

## 测试策略

- `image_engine` 用 Qt Test（项目已依赖 Qt6，零新依赖）:
  - sparse 往返转换、lp metadata 解析（构造二进制样本）、boot header v0-v4 解析、bsdiff patch 应用（小样本）、AVB footer 往返
- 真实样本冒烟测试: 仓库提供 2-3 个小 payload/super 样本下载脚本（不入库）

## 新增依赖

- `zstd`、`lz4`、`bzip2`、`xz`（payload + 裸压缩镜像）
- `brotli`（.br 镜像 + BROTLI_BSDIFF）
- 无 protobuf —— payload 手写 wire format 解析（格式固定，主流 payload_dumper 同做法）

Linux 系统包 / Windows vcpkg 获取。

## CMake 变更

- 新增 `image_engine`、`root_patcher` 静态库目标
- `image_tool_panel` 编入主程序
- 可选 `ENABLE_IMAGE_TESTS` 开关生成测试二进制

## 产物结构

```
解包 payload → <输出>/system.img, vendor.img, boot.img, init_boot.img ...（sparse 保留/转 raw 可选）
解包 zip/tgz/tar.md5 → 原目录结构 + 每镜像再路由
修补 boot → boot_patched.img + .orig.bak
拆分 super → system.img, vendor.img, product.img ...
```

## 签名材料获取原则（2026-08-04 补充）

华为等私有厂商固件（update.app / update.bin / KDZ 等）的签名验证材料与密钥参数**不内置、不捆绑分发**，但支持**运行时从外部获取 + 手动导入**两种途径：

1. **运行时下载（优先）**：从外部公开仓库（如提供签名工具/材料的逆向仓库）下载签名处理工具与参数配置 —— 复用 `AssetsDownloader` 模式（与 Magisk/KernelSU/APatch 注入物、AdbEmbedded 的 Google platform-tools 下载同一模式），下载源列表可配置（JSON 配置：仓库 URL + 文件路径 + 版本），产物缓存于用户数据目录（`QStandardPaths::AppDataLocation + "/patcher/"`）
2. **手动导入（兜底）**：签名处理参数配置文件（JSON：签名头类型 "08"/"06"、签名长度偏移规则）+ 厂商签名密钥/证书文件路径，UI 提供"导入签名材料"入口

使用规则：
- 未获取/未导入签名材料时，相应操作（重打包/打包）禁用并提示"需要签名材料（可从仓库下载或手动导入）"
- 下载的签名工具与材料**不写入代码库**，仅缓存于用户数据目录
- 签名材料按 key 版本化缓存（如 `huawei-sign/`、`lg-kdz/`），下载失败可回退手动导入

受影响模块：华为 update.app/update.bin（B5/B6）、LG KDZ（重打包需签名验证材料时）。

## 维修诊断模块（计划 E，排期在 D 之后）

面向手机维修场景的诊断测试功能，落地在 `system_tool_panel` 新增"维修诊断"分类（复用现有 ADB QProcess 执行模式）。

### 功能清单（12 项，全部纳入）

| 测试项 | 实现方式 | 权限 |
|--------|---------|------|
| 纯色屏幕测试（坏点检测） | 红/绿/蓝/白/黑全屏 + 亮度调节 | 纯 ADB（部分机型需辅助权限） |
| 触摸画线测试 | `input swipe` 轨迹 | 纯 ADB |
| 电池诊断 | `dumpsys battery` + `/sys/class/power_supply` 实时电流/电压/温度 | 纯 ADB |
| 电池循环次数 | `battery_stats` 节点 | 需 Root |
| 音频测试 | 测试音播放（扬声器/听筒）+ `dumpsys audio` | 纯 ADB |
| 麦克风回路 | 录音 + 回放 | 纯 ADB |
| 传感器读数 | `dumpsys sensorservice` 实时六轴/光感/距离 | 纯 ADB |
| 相机测试 | 前后摄启动 + 闪光灯 | 纯 ADB |
| 按键/触摸坐标 | `getevent` 实时流 | 纯 ADB |
| 通信测试 | WiFi/BT/NFC 状态 + SIM 信号 | 纯 ADB |
| eMMC/UFS 寿命 | `mmc extcsd` / UFS 健康节点 | 需 Root |
| IMEI/基带查询 | `dumpsys telephony` / AT 通道 | 纯 ADB（写入需 EDL/MTK，列为后续扩展） |

### 约束

- 纯 ADB 项优先落地；需 Root 项 UI 标注"需 Root"
- EEPROM/字库读写（EDL/MTK 通道）列为后续扩展，不在本计划
- 测试：QProcess 命令构造的 mock 验证（无真机依赖）

## 明确不做（YAGNI）

- ZUCCHINI diff 解包（暂缓，标注提示）
- 三星 secure boot 重签、索尼 sin 重签（技术上不可能，仅提示解锁）
- LG KDZ 重打包（社区验证不可靠，仅解包）
- 增量 OTA 打包生成（只做解包方向，打包为全量）
- Magisk 模块 / KernelSU 元模块的编辑管理
- **从零 mkfs 打包文件系统**（EROFS/ext4 只做"基于原镜像的替换式重打包"，参考 e2fsprogs debugfs 思想；从零生成文件系统为后续扩展）
- f2fs 文件系统浏览（列为 fs 模块后续扩展点）

## 参考实现（学思想，对照格式，不直接搬代码）

| 模块 | 参考项目 | 许可证 | 学习点 |
|------|---------|--------|--------|
| boot/init_boot/vendor_boot | magiskboot (topjohnwu/Magisk) | GPLv3 | boot 解包/重打包权威实现 |
| payload 解包 | payload_dumper-go (ssut)、ota-dump (Rust) | GPLv3 / Apache-2.0 | 手写 protobuf + 压缩分派 |
| super 拆分 | lpunpack (unix3dgforce)、AOSP liblp | GPLv3 / Apache-2.0 | lp metadata 结构 |
| sparse 转换 | AOSP libsparse | Apache-2.0 | sparse 4.0 格式 |
| AVB | avbtool (AOSP external/avb) | BSD-2 | VBMeta/hash footer/禁用验证 |
| bsdiff/puffdiff | bsdiff (Colin Percival)、puffin (Google) | BSD-2 / Apache-2.0 | 增量补丁算法 |
| 三星重打包 | SamsungImageRepacker (Mnky313) | GPLv3 | tar.md5 重打包 |
| LG KDZ/DZ | unkdz/undz (IOMonster)、dumpyara (sebaubuntu-python) | MIT/AGPL | KDZ→DZ→chunk 合并 |
| 华为 update.app | huextract (echo-devim)、HuaweiUpdateExtractor | MIT/GPLv2 | 文件表 + 重打包对齐 |
| 华为 update.bin | unpack_huawei_package (SimomYung) | MIT | L2 分区表解析 |
| 索尼 sin | sin2raw (munjeni)、Flashtool | GPLv2 | SIN v3 ADDR/LZ4A 块 |
| EROFS | erofs-utils (Linux 内核社区) | GPLv2 | fs 解析/重打包 |
| ext4 | e2fsprogs (debugfs write 思想) | GPLv2 | extent tree/替换重打包 |
| Magisk 修补 | Magisk (topjohnwu) | GPLv3 | magiskinit 注入装配 |
| KernelSU 修补 | ksud (tiann/KernelSU) | GPLv3 | init_boot 注入 + KMI 匹配 |
| APatch 修补 | APatch/KernelPatch (bmax121) | GPLv3 | kpatch 内核段注入 |

所有许可证与 GPLv3 兼容，与项目现有 edl/、mtkclient/ 子模块风格一致。

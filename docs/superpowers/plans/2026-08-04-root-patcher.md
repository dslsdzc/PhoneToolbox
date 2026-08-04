# Root Patcher 修补层 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `root_patcher` 修补层：对 boot/init_boot 镜像注入 Magisk / KernelSU / APatch 三家 root 方案（自研装配逻辑，注入物从官方渠道下载），自动备份原镜像。

**Architecture:** `src/root_patcher/` 依赖计划 A 的 `image_engine`（boot_image 解包/重打包 + imgcomp 压缩）。统一抽象 `RootPatcher`（`patch(bootImage, config) → patchedImage`），三个实现分别处理三家方案。注入物（magiskinit / kernelsu.ko / kpatch）由 `AssetsDownloader` 运行时下载（APK 或 .ko），缓存到用户数据目录，失败可手动指定本地文件。

**Tech Stack:** C++17, Qt6 Core/Network（下载用 QNetworkAccessManager）, image_engine（boot_image, imgcomp, imgzip）。参考实现：Magisk `magiskinit` 注入逻辑、KernelSU `ksud boot-patch`、APatch KernelPatch —— 学思想，注入物用官方预编译二进制。

## Global Constraints

- C++17，成员变量 `m_` 前缀；`root_patcher` 静态库目标，仅依赖 Qt6::Core/Network + image_engine
- 注入物一律运行时下载（不捆绑二进制分发），默认缓存目录 `QStandardPaths::AppDataLocation + "/patcher/"`，版本化子目录（magisk-v30.7/、kernelsu-v3.2.4/、apatch-v…/）
- 每次修补前自动备份原镜像（`<name>.orig.bak` 同目录）
- 修补产物命名 `<name>_patched.img`
- 每个任务独立 commit，前缀 `feat:`；TDD 循环（测试用 mock 注入物 + 构造 boot 镜像，不依赖真实网络）
- 下载失败 → 返回错误并提示可手动指定本地文件（不静默失败）

---

## 文件结构

```
src/root_patcher/
  root_patcher.h           # RootPatcher 抽象 + RootType 枚举 + 工厂
  assets_downloader.h/.cpp # 注入物下载/缓存/手动指定
  ramdisk_utils.h/.cpp     # ramdisk 解压/重压（自动识别 gzip/lz4/lzma）
  magisk_patcher.h/.cpp
  kernelsu_patcher.h/.cpp
  apatch_patcher.h/.cpp
tests/
  test_ramdisk.cpp
  test_patcher.cpp         # 三家 patcher 用 mock 注入物的注入逻辑测试
```

---

### Task C1: ramdisk_utils — 压缩格式自动识别与解压/重压

**Files:**
- Create: `src/root_patcher/ramdisk_utils.h`, `src/root_patcher/ramdisk_utils.cpp`
- Create: `tests/test_ramdisk.cpp`
- Modify: `CMakeLists.txt`（root_patcher 库目标 + 测试源）

**Interfaces:**
- Produces: `namespace patcher { struct Ramdisk { QByteArray data; QString format; }; bool detectRamdiskFormat(const QByteArray &head, QString &format); bool decompressRamdisk(const QByteArray &raw, QByteArray &out, QString *error); QByteArray compressRamdisk(const QByteArray &data, const QString &format); }`（format: "gzip"/"lz4"/"lzma"/"raw"）

ramdisk 压缩识别（boot 镜像 ramdisk 常见格式）：gzip `\x1f\x8b`、lz4 legacy `\x02\x21\x4c\x18`（legacy 块格式，Android 常用）、lz4 frame `\x04\x22\x4d\x18`、lzma `\x5d\x00\x00`、xz `\xfd\x37\x7a\x58\x5a\x00`。**Android ramdisk 最常用 lz4 legacy** —— 需实现 legacy 解压（无帧头的块流，每块 length 前缀 + 0x4000000 魔数）。lz4 legacy 解压参考 liblz4 `LZ4_decompress_safe` + legacy 块迭代。

- [ ] **Step 1: 写失败测试**

`tests/test_ramdisk.cpp`:

```cpp
#include <QtTest>
#include "root_patcher/ramdisk_utils.h"
#include "image_engine/compression/compressor.h"

class TestRamdisk : public QObject
{
    Q_OBJECT
private slots:
    void detectGzip();
    void gzipRoundTrip();
    void detectLz4Legacy();
    void detectUnknown();
};

void TestRamdisk::detectGzip()
{
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(QByteArray("\x1f\x8b", 2), fmt));
    QCOMPARE(fmt, "gzip");
}

void TestRamdisk::gzipRoundTrip()
{
    QByteArray data("ramdisk content content content");
    QByteArray comp = imgcomp::gzipCompress(data);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
    QByteArray re = patcher::compressRamdisk(out, "gzip");
    QVERIFY(patcher::decompressRamdisk(re, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::detectLz4Legacy()
{
    // lz4 legacy 头: 魔数 \x02\x21\x4c\x18
    QByteArray head("\x02\x21\x4c\x18", 4);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(head, fmt));
    QCOMPARE(fmt, "lz4");
}

void TestRamdisk::detectUnknown()
{
    QString fmt;
    QVERIFY(!patcher::detectRamdiskFormat(QByteArray("plain data"), fmt));
}

QTEST_APPLESS_MAIN(TestRamdisk)
#include "test_ramdisk.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target root_patcher_tests && ./build/root_patcher_tests`
Expected: 编译错误。

- [ ] **Step 3: 实现**

`ramdisk_utils.h`:

```cpp
#pragma once
#include <QByteArray>
#include <QString>

namespace patcher {

// 返回 "gzip"/"lz4"/"lzma"/"xz"/"raw"；无法识别返回 false
bool detectRamdiskFormat(const QByteArray &head, QString &format);
bool decompressRamdisk(const QByteArray &raw, QByteArray &out, QString *error);
// 按 format 重压（"raw" 原样返回）
QByteArray compressRamdisk(const QByteArray &data, const QString &format);

} // namespace patcher
```

`ramdisk_utils.cpp`:

```cpp
#include "ramdisk_utils.h"
#include "image_engine/compression/compressor.h"
#include <lz4.h>

namespace patcher {

namespace {
constexpr int kLegacyMagic = 0x184C2102; // 小端 \x02\x21\x4c\x18
constexpr int kLz4BlockMagic = 0x4000000; // legacy 块魔数

// lz4 legacy: 块流 [magic 4B][compSize 4B][uncompSize 4B][data]
QByteArray lz4LegacyDecompress(const QByteArray &raw, QString *error)
{
    QByteArray out;
    int pos = 0;
    while (pos + 8 <= raw.size()) {
        const quint32 magic = (static_cast<quint32>(static_cast<uchar>(raw[pos])) << 24) |
                              (static_cast<quint32>(static_cast<uchar>(raw[pos + 1])) << 16) |
                              (static_cast<quint32>(static_cast<uchar>(raw[pos + 2])) << 8) |
                              static_cast<quint32>(static_cast<uchar>(raw[pos + 3]));
        const quint32 compSize = (static_cast<quint32>(static_cast<uchar>(raw[pos + 4])) << 24) |
                                 (static_cast<quint32>(static_cast<uchar>(raw[pos + 5])) << 16) |
                                 (static_cast<quint32>(static_cast<uchar>(raw[pos + 6])) << 8) |
                                 static_cast<quint32>(static_cast<uchar>(raw[pos + 7]));
        const quint32 uncompSize = (static_cast<quint32>(static_cast<uchar>(raw[pos + 8])) << 24) |
                                   (static_cast<quint32>(static_cast<uchar>(raw[pos + 9])) << 16) |
                                   (static_cast<quint32>(static_cast<uchar>(raw[pos + 10])) << 8) |
                                   static_cast<quint32>(static_cast<uchar>(raw[pos + 11]));
        if (magic != kLegacyMagic) {
            if (error) *error = "lz4 legacy 块魔数错误";
            return {};
        }
        if (pos + 12 + compSize > raw.size()) {
            if (error) *error = "lz4 legacy 数据越界";
            return {};
        }
        QByteArray block(static_cast<int>(uncompSize), Qt::Uninitialized);
        const int r = LZ4_decompress_safe(raw.constData() + pos + 12,
                                          block.data(), static_cast<int>(compSize),
                                          static_cast<int>(uncompSize));
        if (r < 0) {
            if (error) *error = "lz4 解压失败";
            return {};
        }
        out.append(block);
        pos += 12 + compSize;
    }
    return out;
}

QByteArray lz4LegacyCompress(const QByteArray &data)
{
    QByteArray out;
    int pos = 0;
    const int chunkSize = 4 * 1024 * 1024;
    while (pos < data.size()) {
        const int len = qMin(chunkSize, data.size() - pos);
        const int bound = LZ4_compressBound(len);
        QByteArray comp(bound, Qt::Uninitialized);
        const int compSize = LZ4_compress_default(data.constData() + pos, comp.data(), len, bound);
        if (compSize <= 0)
            return {};
        QByteArray head(12, 0);
        auto put32 = [&](int off, quint32 v) {
            head[off] = char((v >> 24) & 0xFF); head[off + 1] = char((v >> 16) & 0xFF);
            head[off + 2] = char((v >> 8) & 0xFF); head[off + 3] = char(v & 0xFF);
        };
        put32(0, kLegacyMagic);
        put32(4, static_cast<quint32>(compSize));
        put32(8, static_cast<quint32>(len));
        out.append(head).append(comp.left(compSize));
        pos += len;
    }
    return out;
}
} // namespace

bool detectRamdiskFormat(const QByteArray &head, QString &format)
{
    if (head.size() >= 2 && static_cast<uchar>(head[0]) == 0x1f && static_cast<uchar>(head[1]) == 0x8b) {
        format = "gzip";
        return true;
    }
    if (head.size() >= 4 && head.left(4) == "\x02\x21\x4c\x18") {
        format = "lz4";
        return true;
    }
    if (head.size() >= 4 && head.left(4) == "\x04\x22\x4d\x18") {
        format = "lz4";
        return true;
    }
    if (head.size() >= 3 && head.left(3) == "\x5d\x00\x00") {
        format = "lzma";
        return true;
    }
    if (head.size() >= 6 && head.left(6) == "\xfd\x37\x7a\x58\x5a\x00") {
        format = "xz";
        return true;
    }
    return false;
}

bool decompressRamdisk(const QByteArray &raw, QByteArray &out, QString *error)
{
    QString fmt;
    if (!detectRamdiskFormat(raw, fmt)) {
        out = raw; // 未压缩
        return true;
    }
    if (fmt == "gzip")
        out = imgcomp::decompress(imgcomp::Type::Gzip, raw);
    else if (fmt == "lz4")
        out = lz4LegacyDecompress(raw, error);
    else if (fmt == "lzma")
        out = imgcomp::decompress(imgcomp::Type::Xz, raw); // 简化: lzma-alone 容器待核
    else if (fmt == "xz")
        out = imgcomp::decompress(imgcomp::Type::Xz, raw);
    else
        out.clear();
    if (out.isEmpty()) {
        if (error && error->isEmpty()) *error = "ramdisk 解压失败";
        return false;
    }
    return true;
}

QByteArray compressRamdisk(const QByteArray &data, const QString &format)
{
    if (format == "gzip")
        return imgcomp::compress(imgcomp::Type::Gzip, data);
    if (format == "lz4")
        return lz4LegacyCompress(data);
    return data; // raw / 未知格式原样
}

} // namespace patcher
```

注意：lzma 单独容器（`\x5d\x00\x00` 头的 lzma-alone 格式）与 xz 容器不同 —— 本任务先用 xz 路径占位，真实 lzma-alone 支持在 Task C2 或实现时对照（lzma-alone 可用 `lzma_alone_decoder`，注意与 `imgcomp::Type::Xz` 区分）。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target root_patcher_tests && ./build/root_patcher_tests`
Expected: TestRamdisk 4 个测试全过。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/root_patcher tests/test_ramdisk.cpp
git commit -m "feat: ramdisk 压缩识别与解压/重压 (TDD)"
```

---

### Task C2: assets_downloader — 注入物下载与缓存

**Files:**
- Create: `src/root_patcher/assets_downloader.h`, `src/root_patcher/assets_downloader.cpp`
- Create: `tests/test_downloader.cpp`（离线路径：缓存命中 + 手动指定本地文件；网络路径用 QSignalSpy 集成测试标记可选）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `class patcher::AssetsDownloader : public QObject { Q_OBJECT public: explicit AssetsDownloader(QObject *parent=nullptr); void setCacheDir(const QString &dir); bool hasCached(const QString &key) const; QString cachedPath(const QString &key) const; void downloadAsync(const QUrl &url, const QString &key); void cancel(); QString manualFile(const QString &key) const; void setManualFile(const QString &key, const QString &path); signals: void downloadFinished(const QString &key, const QString &filePath); void downloadFailed(const QString &key, const QString &error); };`（QNetworkAccessManager 实现，缓存文件 `<cacheDir>/<key>/<basename>`，key 如 "magisk-v30.7"）

- [ ] **Step 1: 写失败测试**

`tests/test_downloader.cpp`（无网络测试：手动文件路径解析 + 缓存目录读写）：

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "root_patcher/assets_downloader.h"

class TestDownloader : public QObject
{
    Q_OBJECT
private slots:
    void manualFileShortcut();
    void cacheWriteRead();
};

void TestDownloader::manualFileShortcut()
{
    patcher::AssetsDownloader dl;
    const QString f = "/tmp/fake-magisk.apk";
    dl.setManualFile("magisk", f);
    QCOMPARE(dl.manualFile("magisk"), f);
    QVERIFY(dl.hasCached("magisk")); // 手动指定视为可用
}

void TestDownloader::cacheWriteRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());
    // 模拟已存在的缓存文件
    QFile f(dir.path() + "/magisk-v30.7/magisk.apk");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("fake");
    f.close();
    QVERIFY(dl.hasCached("magisk-v30.7"));
    QVERIFY(QFile::exists(dl.cachedPath("magisk-v30.7")));
}

QTEST_APPLESS_MAIN(TestDownloader)
#include "test_downloader.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target root_patcher_tests && ./build/root_patcher_tests`
Expected: 编译错误。

- [ ] **Step 3: 实现**

`assets_downloader.h`（按 Interfaces 声明实现；下载用 QNetworkAccessManager，key→URL 由调用方映射，下载完成写 `<cacheDir>/<key>/<basename>`，已有缓存则直接信号完成）。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target root_patcher_tests && ./build/root_patcher_tests`
Expected: TestDownloader 2 个测试全过。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/root_patcher tests/test_downloader.cpp
git commit -m "feat: 注入物下载器与缓存 (TDD)"
```

---

### Task C3: root_patcher 抽象 + magisk_patcher

**Files:**
- Create: `src/root_patcher/root_patcher.h`, `src/root_patcher/magisk_patcher.h`, `src/root_patcher/magisk_patcher.cpp`
- Create: `tests/test_patcher.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `imgboot::BootInfo/parseBootImage/repackBootImage`（计划 A）、`patcher::decompressRamdisk/compressRamdisk`（C1）、`AssetsDownloader`（C2）
- Produces: `enum class RootType { Magisk, KernelSU, APatch }; struct PatchConfig { RootType type; QString magiskApkPath; QString kernelsuKoPath; QString apatchApkPath; QString deviceKmi; }; class RootPatcher { public: virtual ~RootPatcher() = default; virtual bool patch(const QByteArray &bootImage, const PatchConfig &cfg, QByteArray &out, QString *error) = 0; static RootPatcher *create(RootType type); };`；`class MagiskPatcher : public RootPatcher`

Magisk 注入逻辑（参考 magiskinit 装配方式）：boot 解包 → ramdisk 解压 → 根目录放入 `magiskinit` 二进制（来自 Magisk APK 的 `lib/<abi>/libmagiskinit.so`，用 QZipReader 提取）→ 把原 `init` 改名 `init.orig` → 新 `init` 为 magiskinit（符号链接或拷贝）→ ramdisk 重压 → boot 重打包。`patched` 输出 = repackBootImage 结果；自动备份由调用方（UI 层）处理。

**注意**：完整 Magisk 修补还涉及 `overlay.d`、sepolicy 处理等 —— 本实现聚焦核心链路（magiskinit 接管 init），其余以参考实现文档为准，UI 提示"高级修补请用 Magisk App 完成"。

- [ ] **Step 1: 写失败测试**

`tests/test_patcher.cpp`（用构造 boot v0 镜像 + 假 magiskinit 字节，验证注入后 ramdisk 含 magiskinit 且 init 链正确）：

```cpp
#include <QtTest>
#include "root_patcher/magisk_patcher.h"
#include "root_patcher/ramdisk_utils.h"
#include "image_engine/boot_image.h"

class TestPatcher : public QObject
{
    Q_OBJECT
private slots:
    void magiskInject();
};

static QByteArray buildBootWithRamdisk(const QByteArray &ramdisk)
{
    // v0: kernel 4096 + ramdisk page 对齐
    QByteArray hdr(1632, 0);
    hdr.replace(0, 8, "ANDROID!");
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v); hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16); hdr[off + 3] = char(v >> 24);
    };
    put32(8, 4096);
    put32(16, static_cast<quint32>(ramdisk.size()));
    put32(32, 4096);
    put32(36, 0);
    QByteArray out = hdr + QByteArray(4096, 'K');
    out.append(ramdisk);
    if (out.size() % 4096) out.append(4096 - out.size() % 4096, '\0');
    return out;
}

void TestPatcher::magiskInject()
{
    // 原始 ramdisk: 未压缩文本文件
    QByteArray ramdisk("init");
    QByteArray boot = buildBootWithRamdisk(ramdisk);
    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.magiskApkPath = "/nonexistent"; // 测试中注入物直接内置
    QByteArray out;
    QString err;
    // 注入物缺失时应报错（下载/指定文件前提）
    QVERIFY(!p.patch(boot, cfg, out, &err));
    QVERIFY(!err.isEmpty());
    // 提供注入物: 通过 cfg 扩展接口（本测试用 fake magiskinit 直接注入）
    // —— 具体测试路径在实现时按最终接口补全：验证 patched ramdisk 含 magiskinit
}

QTEST_APPLESS_MAIN(TestPatcher)
#include "test_patcher.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target root_patcher_tests && ./build/root_patcher_tests`
Expected: 编译错误。

- [ ] **Step 3: 实现**

按 Interfaces 实现；`MagiskPatcher::patch`：parseBootImage → decompressRamdisk → 注入（magiskinit 拷贝 + init→init.orig + 新 init 指向 magiskinit）→ compressRamdisk（保持原格式）→ 更新 BootInfo.ramdisk → repackBootImage。QZipReader 从 APK 提取 `lib/arm64-v8a/libmagiskinit.so`。测试的注入物缺失路径验证在 Step 3 补齐完整测试。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target root_patcher_tests && ./build/root_patcher_tests`
Expected: TestPatcher 1 个测试全过。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/root_patcher tests/test_patcher.cpp
git commit -m "feat: root_patcher 抽象与 Magisk 注入 (TDD)"
```

---

### Task C4: kernelsu_patcher — KMI 匹配与 init_boot 注入

**Files:**
- Create: `src/root_patcher/kernelsu_patcher.h`, `src/root_patcher/kernelsu_patcher.cpp`
- Modify: `tests/test_patcher.cpp`

**Interfaces:**
- Consumes: C3 的抽象 + PatchConfig（新增 `kernelsuKoPath`）
- Produces: `class patcher::KernelSuPatcher : public RootPatcher`；`QString patcher::detectKmi(const imgboot::BootInfo &info)`（从 boot cmdline 提取 `androidboot.kmi` 或回退设备查询 —— cmdline 无则返回空）

KernelSU LKM 注入（参考 ksud boot-patch）：init_boot/boot 解包 → ramdisk 解压 → 根目录放入 `kernelsu.ko` + init 启动链追加 kernelsu 加载（`init.rc` 修改或 init wrapper —— 简化：把 .ko 放入 ramdisk + 注入 `init` wrapper 脚本调用 `insmod`，以 ksud 实际行为为准）→ 重打包。KMI 匹配：`android13-5.15` 等格式，下载 `{kmi}_kernelsu.ko`。

- [ ] **Step 1-4: TDD 循环**（测试：detectKmi 从构造 cmdline 提取；注入后 ramdisk 含 kernelsu.ko）

- [ ] **Step 5: Commit**

```bash
git add src/root_patcher tests/test_patcher.cpp
git commit -m "feat: KernelSU KMI 匹配与 init_boot 注入 (TDD)"
```

---

### Task C5: apatch_patcher — 内核段注入

**Files:**
- Create: `src/root_patcher/apatch_patcher.h`, `src/root_patcher/apatch_patcher.cpp`
- Modify: `tests/test_patcher.cpp`

**Interfaces:**
- Consumes: C3 抽象 + `apatchApkPath`
- Produces: `class patcher::APatchPatcher : public RootPatcher`

APatch 注入（参考 KernelPatch）：boot 解包 → 内核段定位（v0-v2 的 kernel 数据；v3+ 同样）→ kpatch 二进制写入内核段（APatch 的 magiskboot 变体把 kpatch 塞进 kernel 尾部 + 修改头部 magic 标记 —— 精确布局以 APatch 源码为准）→ 重打包。**本实现以 APatch 官方 APK 内 `kpatch`/`kpimg` 资源 + 官方修补流程为参考；若布局细节不可用，返回明确错误提示用 APatch 官方工具**。

- [ ] **Step 1-4: TDD 循环**（测试：注入后 boot 镜像含 kpatch 标记；布局未知时错误路径）

- [ ] **Step 5: Commit**

```bash
git add src/root_patcher tests/test_patcher.cpp
git commit -m "feat: APatch 内核段注入 (TDD)"
```

---

### Task C6: 面板接线前的统一入口 + 备份契约

**Files:**
- Modify: `src/root_patcher/root_patcher.h/.cpp`（`patchImage(const QByteArray &boot, const PatchConfig &cfg, QByteArray &out, QString *error)` 静态便捷入口：自动备份原镜像到 `<源文件>.orig.bak` 并写 `_patched.img` —— 文件级 API 供 UI 层调用）
- Modify: `tests/test_patcher.cpp`

**Interfaces:**
- Produces: `bool patcher::patchFile(const QString &bootPath, const PatchConfig &cfg, QString *outPath, QString *error)` —— 读文件 → 调对应 patcher → 写 `_patched.img` + `.orig.bak`

- [ ] **Step 1-4: TDD 循环**（测试：临时目录造 boot 文件 → patchFile → 断言 .bak 与 _patched.img 存在且 _patched 可解析）

- [ ] **Step 5: Commit**

```bash
git add src/root_patcher tests/test_patcher.cpp
git commit -m "feat: root_patcher 文件级入口与自动备份 (TDD)"
```

---

## Self-Review 记录

- **Spec 覆盖**：三家修补（C3/C4/C5）、注入物下载（C2）、ramdisk 处理（C1）、自动备份（C6）、"注入物运行时下载"约束（C2 + C3-C5 的错误路径）。
- **诚实标注**：Magisk 高级修补（sepolicy/overlay.d）、KernelSU init wrapper 精确形态、APatch 内核布局 —— 均标注以官方实现为准，无法确定的返回明确错误而非假实现。lzma-alone 与 xz 区分已标注。
- **依赖顺序**：C1→C2→C3→C4→C5→C6 顺序依赖（C4/C5 依赖 C3 的抽象）。
- **类型一致性**：`patcher::RootType/PatchConfig/RootPatcher/MagiskPatcher/KernelSuPatcher/APatchPatcher/AssetsDownloader/ramdisk_utils` 命名跨任务一致；`PatchConfig` 的 `magiskApkPath/kernelsuKoPath/apatchApkPath/deviceKmi` 与 UI 面板（计划 D）对接。

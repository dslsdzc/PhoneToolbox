# OPPO 固件解包引擎 Phase A 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 镜像工具面板新增 OPPO 系固件包（`.ofp` QC/MTK + `.ops`）的解密解包引擎，纯 C++ 自研，离线可验证。

**Architecture:** 在 `src/image_engine/` 下新增 `imgopp` 命名空间的 4 个纯函数模块（crypto/keys/ofp/ops），经 `imgreg::detect` 识别、`ImageWorker` 分派解包，产物文件写到用户选定目录。密码学：AES-128 自实现（FIPS-197）+ Qt `QCryptographicHash`；OPS 自定义流密码按双源参照移植。

**Tech Stack:** C++17 / Qt6 Core（QCryptographicHash, QFile, QXmlStreamReader）/ Qt6 Test / CMake + Ninja。

## Global Constraints

- 参照物已就位（gitignored，仅供实现时对照，不进构建）：
  - `reference/oppo_decrypt/{ofp_qc_decrypt.py, ofp_mtk_decrypt.py, opscrypto.py}`（bkerler，文件头 MIT）
  - `reference/FirmwareKit.Oppo/`（Uotan-Dev，MIT，C#；KeyDatabase/Crypto/FormatParser 可交叉核对）
  - 格式常量与偏移速查：`docs/superpowers/specs/oppo-format-notes.md`（先读它）
  - 设计 spec：`docs/superpowers/specs/2026-09-03-oppo-unpack-phase-a-design.md`
- 命名空间 `imgopp`；文件放 `src/image_engine/`（CMake 用 `GLOB_RECURSE` 自动收录，新增 .cpp 无需改 CMakeLists）
- 测试放 `tests/test_oppo_*.cpp`，用 `QTEST_APPLESS_MAIN`，**必须**把文件名加进根 `CMakeLists.txt` 的 `IMAGE_TEST_SOURCES` 列表（约 L262-288 区）
- 无 QObject/Q_OBJECT（纯函数模块，worker 线程直接调用）
- 错误一律 `bool 返回 + QString *error` 传递；失败必须写明确中文错误文案，禁止静默
- 所有反推的格式常量必须带来源注释（参照文件 + 语义），同 pac/sin 引擎惯例
- 构建命令：`cmake -B build -G Ninja && cmake --build build`；测试：`ctest --test-dir build -R <target>` 或直接 `./build/<target>`
- 公开密钥覆盖 ~2020-2021 机型；未知密钥报明确错误（不内置猜测）

---

### Task 0: 测试基建与任务清单确认（5 分钟）

**Files:**
- Read: `CMakeLists.txt:257-300`（IMAGE_TEST_SOURCES 注册模式）
- Read: `tests/test_registry.cpp`（Qt Test 写法样板）

- [ ] **Step 1: 读注册模式**

Run: `sed -n 257,300p CMakeLists.txt`
Expected: 看到 `set(IMAGE_TEST_SOURCES ...)` 列表与 foreach 逐源可执行文件创建。后续每个测试文件都要加进这个列表。

- [ ] **Step 2: 确认参照文件可读**

Run: `ls reference/oppo_decrypt/ reference/FirmwareKit.Oppo/ | head`
Expected: 两个目录都列得出内容。若为空，先 `git clone --depth 1 https://github.com/bkerler/oppo_decrypt.git reference/oppo_decrypt` 与 `https://github.com/Uotan-Dev/FirmwareKit.Oppo.git reference/FirmwareKit.Oppo`。

---

### Task 1: AES-128 核心 + CFB 解密（自实现）

**Files:**
- Create: `src/image_engine/oppo_crypto.h`
- Create: `src/image_engine/oppo_crypto.cpp`
- Create: `tests/test_oppo_crypto.cpp`
- Modify: `CMakeLists.txt`（IMAGE_TEST_SOURCES 加 `tests/test_oppo_crypto.cpp`）

**Interfaces:**
- Consumes: 无
- Produces:
  - `namespace imgopp`
  - `class Aes128 { public: bool setKey(const quint8 key[16]); void encryptBlock(const quint8 in[16], quint8 out[16]) const; bool isValid() const; }`
  - `QByteArray aes128CfbDecrypt(const QByteArray &data, const QByteArray &key16, const QByteArray &iv16)`（segment_size=128，即全块 CFB：C0..C15 与 E(IV) 异或，后续块与 E(前密文块) 异或）
  - `QByteArray aes128CfbEncrypt(const QByteArray &data, const QByteArray &key16, const QByteArray &iv16)`（CFB 加密：P0..P15 与 E(IV) 异或得 C；后续块 P 与 E(前密文块) 异或。**合成包测试夹具需要用它生成密文**，不可用解密函数反向调用代替）

- [ ] **Step 1: 写失败测试**

`tests/test_oppo_crypto.cpp`:

```cpp
#include <QtTest>
#include "image_engine/oppo_crypto.h"

class TestOppoCrypto : public QObject
{
    Q_OBJECT
private slots:
    void aesFips197Vector();
    void cfbRoundTrip();
    void cfbKnownAnswer();
};

// FIPS-197 Appendix C.1: AES-128
// key 000102030405060708090a0b0c0d0e0f
// plaintext 00112233445566778899aabbccddeeff -> ciphertext 69c4e0d86a7b0430d8cdb78070b4c55a
void TestOppoCrypto::aesFips197Vector()
{
    imgopp::Aes128 aes;
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    QVERIFY(aes.setKey(reinterpret_cast<const quint8 *>(key.constData())));
    QVERIFY(aes.isValid());
    const QByteArray pt = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    quint8 out[16];
    aes.encryptBlock(reinterpret_cast<const quint8 *>(pt.constData()), out);
    QCOMPARE(QByteArray(reinterpret_cast<char *>(out), 16),
             QByteArray::fromHex("69c4e0d86a7b0430d8cdb78070b4c55a"));
}

// CFB 自洽：解密(加密(x)) == x（用已知 key 手工构造 CFB 密文）
void TestOppoCrypto::cfbRoundTrip()
{
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray iv  = QByteArray::fromHex("0f0e0d0c0b0a09080706050403020100");
    const QByteArray pt  = QByteArray("hello cfb world!!");  // 17 字节，跨块
    // 用 Python: Cryptodome AES.MODE_CFB segment_size=128 生成参考密文
    // 本测试由实现者用 reference/ 环境的 python3 -c 生成一次（见 Step 3 注释）
    const QByteArray ct = QByteArray::fromHex("REPLACE_ME");  // Step 3 生成
    QCOMPARE(imgopp::aes128CfbDecrypt(ct, key, iv), pt);
    // 自实现加解密互逆（供合成包夹具信任）
    QCOMPARE(imgopp::aes128CfbEncrypt(pt, key, iv), ct);
}

void TestOppoCrypto::cfbKnownAnswer()
{
    // 空输入返回空
    QCOMPARE(imgopp::aes128CfbDecrypt(QByteArray(), QByteArray(16, 0), QByteArray(16, 0)),
             QByteArray());
    // 长度非法（key/iv 非 16B）返回空
    QCOMPARE(imgopp::aes128CfbDecrypt(QByteArray(16, 1), QByteArray(15, 0), QByteArray(16, 0)),
             QByteArray());
}

QTEST_APPLESS_MAIN(TestOppoCrypto)
#include "test_oppo_crypto.moc"
```

- [ ] **Step 2: 注册测试并确认编译失败**

把 `tests/test_oppo_crypto.cpp` 加进 `CMakeLists.txt` 的 `IMAGE_TEST_SOURCES` 列表末尾。

Run: `cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_oppo_crypto 2>&1 | tail -5`
Expected: 编译失败 "oppo_crypto.h: No such file or directory"。

- [ ] **Step 3: 生成 CFB 参考密文（填回 Step 1 的 REPLACE_ME）**

Run:
```bash
python3 -c "
from Cryptodome.Cipher import AES
key = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
iv  = bytes.fromhex('0f0e0d0c0b0a09080706050403020100')
pt  = b'hello cfb world!!'
print(AES.new(key, AES.MODE_CFB, iv=iv, segment_size=128).encrypt(pt).hex())
"
```
若缺 `Cryptodome` 且 `reference/oppo_decrypt/requirements.txt` 未装，改用 C# 参照不合适时，可先用 `openssl enc -aes-128-cfb`（注意 OpenSSL 的 `-cfb` 是 CFB128，等价）：
```bash
python3 - <<'EOF'
pt = b'hello cfb world!!'
open('/tmp/pt.bin','wb').write(pt)
EOF
openssl enc -aes-128-cfb -K 000102030405060708090a0b0c0d0e0f -iv 0f0e0d0c0b0a09080706050403020100 -in /tmp/pt.bin | xxd -p | tr -d '\n'
```
把输出的 hex 填回测试的 `REPLACE_ME`。

- [ ] **Step 4: 实现 AES + CFB**

`src/image_engine/oppo_crypto.h`:

```cpp
#pragma once
// OPPO 固件解密密码核心（Phase A，spec 2026-09-03）
// AES-128 自实现（FIPS-197）；CFB segment_size=128 与 bkerler/oppo_decrypt
// 的 Cryptodome AES.MODE_CFB 逐字兼容（ofp_qc_decrypt.py aes_cfb()）。
#include <QByteArray>
#include <QtGlobal>

namespace imgopp {

class Aes128
{
public:
    // key: 16 字节；成功 true。失败（null）后 isValid() 为 false
    bool setKey(const quint8 key[16]);
    bool isValid() const { return m_valid; }
    void encryptBlock(const quint8 in[16], quint8 out[16]) const;

private:
    void keyExpansion(const quint8 key[16]);
    quint8 m_roundKey[176] = {};   // 11 轮 × 16B
    bool m_valid = false;
};

// AES-128-CFB（segment_size=128）解密。key16/iv16 必须各 16 字节，
// data 为空返回空，参数非法返回空 QByteArray。
QByteArray aes128CfbDecrypt(const QByteArray &data, const QByteArray &key16,
                            const QByteArray &iv16);

} // namespace imgopp
```

`src/image_engine/oppo_crypto.cpp`：实现 FIPS-197 标准 AES-128（S-box 表、密钥扩展、10 轮 SubBytes/ShiftRows/MixColumns/AddRoundKey），CFB 解密算法：

```cpp
// CFB segment_size=128: 
//   block0 密文 ^ E(IV) = 明文0; blockN 密文 ^ E(密文N-1) = 明文N
//   末块不足 16B 时：按剩余长度截断 E(prev) 前缀（与 Cryptodome 行为一致）
```
要求：不用查表加速花活，直接教科书实现（表 + 轮函数），注释标注 FIPS-197 章节对应。

- [ ] **Step 5: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_oppo_crypto`
Expected: `Totals: 3 passed, 0 failed, 0 skipped`

- [ ] **Step 6: 提交**

```bash
git add src/image_engine/oppo_crypto.h src/image_engine/oppo_crypto.cpp tests/test_oppo_crypto.cpp CMakeLists.txt
git commit -m "feat(oppo): AES-128 核心 + CFB 解密（FIPS-197 自实现 + 向量测试）"
```

---

### Task 2: 密钥库 + QC/MTK 派生 + OPS 自定义密码

**Files:**
- Create: `src/image_engine/oppo_keys.h`
- Create: `src/image_engine/oppo_keys.cpp`
- Create: `tests/test_oppo_keys.cpp`
- Modify: `CMakeLists.txt`（注册测试）

**Interfaces:**
- Consumes: `imgopp::Aes128`（Task 1）
- Produces:
  - `struct OppoKeyPair { QString keyId; QByteArray key; QByteArray iv; }`（key/iv 各 16B）
  - `QList<OppoKeyPair> qcKeyCandidates()`（顺序 V1.4.17→V1.6.17→V1.5.13→V1.6.6族→V1.7.2→V2.0.3）
  - `QList<OppoKeyPair> mtkKeyCandidates()`（MTK0..MTK8）
  - `QList<OppoKeyPair> opsKeyCandidates()`（mbox5→mbox6→mbox4；key = mbox 前 16B）
  - `bool loadOppoKeysJson(const QString &path, QList<OppoKeyPair> &out, QString *error)`（外部追加）
  - `QByteArray opsDecrypt(const QByteArray &data, const QByteArray &mboxKey16, const QByteArray &roundBlob)` —— OPS 自定义流密码（Task 5 用）

- [ ] **Step 1: 写失败测试**

`tests/test_oppo_keys.cpp`——断言派生链（数据取自 `docs/superpowers/specs/oppo-format-notes.md` §QC 派生自测断言）：

```cpp
#include <QtTest>
#include "image_engine/oppo_keys.h"

class TestOppoKeys : public QObject
{
    Q_OBJECT
private slots:
    void qcDerivation();
    void mtkDerivation();
    void opsKeyCandidates();
};

void TestOppoKeys::qcDerivation()
{
    const auto keys = imgopp::qcKeyCandidates();
    QCOMPARE(keys.size(), 6);
    // 顺序与派生值（spec 速查表）
    QCOMPARE(keys[0].keyId, QStringLiteral("V1.4.17"));
    QCOMPARE(keys[0].key, QByteArray("d154afeeaafa958f"));
    QCOMPARE(keys[0].iv, QByteArray("2c040f5786829207"));
    QCOMPARE(keys[2].keyId, QStringLiteral("V1.5.13"));
    QCOMPARE(keys[2].key, QByteArray("94d62e831cf1a1a0"));
    QCOMPARE(keys[2].iv, QByteArray("7ab5e33bd50d81ca"));
    QCOMPARE(keys[3].key, QByteArray("4a837229e6fc77d4"));  // V1.6.6 族
    QCOMPARE(keys[3].iv, QByteArray("00bed47b80eec9d7"));
    QCOMPARE(keys[5].keyId, QStringLiteral("V2.0.3"));
    QCOMPARE(keys[5].key, QByteArray("b4b7358eea220991"));
}

void TestOppoKeys::mtkDerivation()
{
    const auto keys = imgopp::mtkKeyCandidates();
    QCOMPARE(keys.size(), 9);
    QCOMPARE(keys[0].key, QByteArray("94d62e831cf1a1a0"));  // MTK0 == QC V1.5.13 同 triplet
    QCOMPARE(keys[1].key, QByteArray("52dddab2c46aab56"));
    QCOMPARE(keys[1].iv, QByteArray("35f19b6877f9c360"));
    QCOMPARE(keys[8].key, QByteArray("ab3f76d7989207f2"));  // MTK8 直接 ASCII
    QCOMPARE(keys[8].iv, QByteArray("2bf515b3a9737835"));
}

void TestOppoKeys::opsKeyCandidates()
{
    const auto keys = imgopp::opsKeyCandidates();
    QCOMPARE(keys.size(), 3);
    QCOMPARE(keys[0].keyId, QStringLiteral("mbox5"));
    QCOMPARE(keys[0].key, QByteArray::fromHex("608A3F2D686BD423510CD095BB40E976"));
    QCOMPARE(keys[1].key, QByteArray::fromHex("AA69829E5DDEB13D30BB81A34665A3E1"));
    QCOMPARE(keys[2].key, QByteArray::fromHex("C45D057199DDBBEE29A16DC7ADBFA43F"));
}

QTEST_APPLESS_MAIN(TestOppoKeys)
#include "test_oppo_keys.moc"
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败（oppo_keys.h 不存在）。

- [ ] **Step 3: 实现密钥库**

派生算法（对照 `reference/oppo_decrypt/ofp_qc_decrypt.py` `deobfuscate()`/`generatekey2()`，L33-49）：

```cpp
// deobfuscate: out[i] = ROL8(data[i] ^ mask[i], 4)
//   ROL8(x,n) = ((x << n) | (x >> (8 - n))) & 0xFF   // n=4 时即半字节交换后的循环左移
// key = md5(deobfuscate(userkey, mc)).toHex().left(16) 的 ASCII 编码
// iv  = md5(deobfuscate(ivec, mc)).toHex().left(16) 的 ASCII 编码
```

QC triplet 表、MTK 表（MTK0-7 同派生法、MTK8 直接 ASCII）、OPS mbox 常量：全部按 `docs/superpowers/specs/oppo-format-notes.md` §QC/§MTK/§OPS 逐字填入，每行注释来源（如 `// bkerler ofp_qc_decrypt.py generatekey2() R9s/A57t`）。

`loadOppoKeysJson`：JSON 格式 `{"qc":[{"id":"...","mc":"hex","userkey":"hex","iv":"hex"}],"ops":[{"id":"...","key":"hex"}]}`；解析失败写 `*error` 返回 false。用 `QJsonDocument`。

`opsDecrypt`：**暂留实现桩**——签名写全，函数体 `Q_UNUSED(...); return QByteArray();`，注释 `// Task 5 实现（OPS 自定义密码移植）`。本任务测试不覆盖它。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_oppo_keys`
Expected: `Totals: 3 passed, 0 failed`

- [ ] **Step 5: 提交**

```bash
git add src/image_engine/oppo_keys.h src/image_engine/oppo_keys.cpp tests/test_oppo_keys.cpp CMakeLists.txt
git commit -m "feat(oppo): 密钥库（QC/MTK 派生 + OPS mbox + 外部 JSON 追加）"
```

---

### Task 3: OFP 识别与解析（QC + MTK 变体）

**Files:**
- Create: `src/image_engine/oppo_ofp.h`
- Create: `src/image_engine/oppo_ofp.cpp`
- Create: `tests/test_oppo_ofp.cpp`
- Modify: `CMakeLists.txt`（注册测试）

**Interfaces:**
- Consumes: `imgopp::qcKeyCandidates/mtkKeyCandidates`（Task 2）、`aes128CfbDecrypt`（Task 1）
- Produces:

```cpp
namespace imgopp {

enum class OfpVariant { Qc, Mtk, Unknown };

struct OfpFile {
    QString name;          // 分区/文件名（清单 Path 或文件表 filename）
    QString group;         // QC 清单所属组（Sahara/Firmware/Config/...）；MTK 为空
    quint64 offset = 0;    // 包内绝对偏移（字节）
    quint64 size = 0;      // 落盘大小（字节）
    quint64 encryptedSize = 0;  // 需解密的字节数（0=全明文）
    bool fullDecrypt = false;   // true=Sahara 类全解密；false=按 encryptedSize
    QString sha256Hex;     // 可选（QC 清单属性）
    QString md5Hex;        // 可选
    bool sparse = false;   // QC 清单 sparse="true"
};

struct OfpInfo {
    OfpVariant variant = OfpVariant::Unknown;
    QString keyId;         // 命中的 key 候选 id
    QByteArray key;        // 命中的 16B AES key（Task 4 提取用）
    QByteArray iv;         // 命中的 16B IV（Task 4 提取用）
    quint32 pageSize = 0;  // QC: 0x200/0x1000
    QString projectName;   // MTK prjname / QC 清单顶层名（如有）
    QString version;       // 可用时填（MTK flashtype 等）
    QList<OfpFile> files;
};

// 尾页探测：tail 为文件末尾至少 max(0x1000, 0x6C) 字节；fileSize 为整个文件大小。
// 命中返回 true 并填 variant。
bool detectOFP(const QByteArray &tail, quint64 fileSize, OfpVariant &variant);

// 解析（含 key 试解）。path 为包路径。成功填 info。
bool parseOFP(const QString &path, OfpInfo &info, QString *error);

} // namespace imgopp
```

- [ ] **Step 1: 写失败测试（合成包）**

`tests/test_oppo_ofp.cpp`——按格式速查手工构造两个最小合成包（**这是本任务的测试夹具，也是 Task 4 的输入**）：

```cpp
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "image_engine/oppo_ofp.h"
#include "image_engine/oppo_keys.h"

// 合成 QC 包：tail page(0x200) + XML(AES-CFB 加密, 用 Task1 的 aes128CfbEncrypt 生成密文)
static QByteArray buildSyntheticQcPackage()
{
    QByteArray xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<ProFile>\n"
        "  <Sahara>\n"
        "    <File Path=\"prog_ufs_firehose_test.elf\" FileOffsetInSrc=\"2\""
        " SizeInByteInSrc=\"4096\" SizeInSectorInSrc=\"8\" md5=\"\" sha256=\"\"/>\n"
        "  </Sahara>\n"
        "  <Firmware>\n"
        "    <File Path=\"boot.img\" FileOffsetInSrc=\"10\" SizeInByteInSrc=\"4096\""
        " SizeInSectorInSrc=\"8\"/>\n"
        "  </Firmware>\n"
        "</ProFile>\n";
    const quint32 page = 0x200;
    const quint32 xmlOffsetPages = 3;            // XML 从第 3 页起
    // 取 V1.5.13 候选 key 加密 XML
    const auto keys = imgopp::qcKeyCandidates();
    const QByteArray encXml =
        imgopp::aes128CfbEncrypt(xml, keys[2].key, keys[2].iv);
    QByteArray pkg;
    pkg.resize(xmlOffsetPages * page + encXml.size());
    pkg.fill('\0');
    pkg.replace(xmlOffsetPages * page, encXml.size(), encXml);
    QByteArray tail(page, '\0');
    // +0x10 magic 0x7CEF (LE)
    tail[0x10] = char(0xEF); tail[0x11] = char(0x7C);
    // +0x14 xml offset(页), +0x18 xml length
    const quint32 off = xmlOffsetPages;
    tail[0x14] = char(off & 0xFF);         tail[0x15] = char((off >> 8) & 0xFF);
    tail[0x16] = char((off >> 16) & 0xFF); tail[0x17] = char((off >> 24) & 0xFF);
    const quint32 len = encXml.size();
    tail[0x18] = char(len & 0xFF);         tail[0x19] = char((len >> 8) & 0xFF);
    tail[0x1A] = char((len >> 16) & 0xFF); tail[0x1B] = char((len >> 24) & 0xFF);
    pkg.append(tail);
    // 数据区（供 Task 4 提取测试）：填充可识别字节
    pkg.resize(10 * page);
    pkg.fill('\xAA', 10 * page);
    return pkg;
}

class TestOppoOfp : public QObject
{
    Q_OBJECT
private slots:
    void detectQcFromTail();
    void detectMtkFromTail();
    void detectRejectsRandomTail();
    void parseQcSynthetic();
};

void TestOppoOfp::detectQcFromTail()
{
    const QByteArray pkg = buildSyntheticQcPackage();
    imgopp::OfpVariant v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg.right(0x1000), pkg.size(), v));
    QCOMPARE(v, imgopp::OfpVariant::Qc);
    v = imgopp::OfpVariant::Unknown;   // 二次调用稳定（结果不依赖内部状态）
    QVERIFY(imgopp::detectOFP(pkg.right(0x1000), pkg.size(), v));
    QCOMPARE(v, imgopp::OfpVariant::Qc);
}

void TestOppoOfp::detectMtkFromTail()
{
    // MTK: 尾 0x6C 混淆头（XOR + ROL4, key "geyixue"）—— 构造 prjname="TESTPRJ"
    QByteArray pkg(0x1000, '\0');
    const QByteArray key("geyixue");
    QByteArray hdr(0x6C, '\0');
    const char *prj = "TESTPRJ";
    for (int i = 0; i < qstrlen(prj); ++i) hdr[i] = prj[i];
    // 混淆: out = ROL8(in ^ key[i%7], 4)
    for (int i = 0; i < 0x6C; ++i) {
        quint8 x = quint8(hdr[i]) ^ quint8(key[i % 7]);
        hdr[i] = char(((x << 4) | (x >> 4)) & 0xFF);
    }
    pkg.replace(0x1000 - 0x6C, 0x6C, hdr);
    imgopp::OfpVariant v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg.right(0x1000), pkg.size(), v));
    QCOMPARE(v, imgopp::OfpVariant::Mtk);
}
// ... detectRejectsRandomTail / parseQcSynthetic 略写法同上，
// parseQcSynthetic 断言: info.variant==Qc, keyId=="V1.5.13", files.size()==2,
//   files[0].group=="Sahara" && files[0].fullDecrypt==true,
//   files[1].group=="Firmware" && files[1].offset==10*0x200
```

（实现者补全 `detectRejectsRandomTail`：随机 0x1000 字节 → detectOFP 返回 false。）

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败（oppo_ofp.h 不存在）。

- [ ] **Step 3: 实现 detectOFP + parseOFP**

对照 `reference/oppo_decrypt/ofp_qc_decrypt.py`（L110-152 extract_xml）+ `ofp_mtk_decrypt.py`：

- `detectOFP`：
  - QC：对 `[0x200, 0x1000]` 两候选页尺寸，取 `tail.right(pageSize)`，读 `+0x10` LE32 == 0x7CEF → Qc（QC 尾页无 version 字段约束）
  - MTK：解混淆尾 0x6C（`ROL8(x ^ key[i%7], 4)`，key=`"geyixue"`），prjname 前若干字节全为可打印 ASCII 且非全零 → Mtk
  - 注意 QC 与 MTK 判定顺序：先 MTK 后 QC（MTK 无 0x7CEF，不冲突，但保持确定性）
- `parseOFP`：
  - 读尾页确定变体；QC 分支：按 `qcKeyCandidates()` 顺序逐个 `aes128CfbDecrypt(xmlBlob, key, iv)`，命中条件 = 解密结果含 `"<?xml"`（**子串**判定，bkerler 同款）；xmlLength<200 时按 `(filesize-page) - xmlOffset - 0x57` 重算
  - XML 解析用 `QXmlStreamReader`：顶层子元素即组名；`File` 元素属性 `Path`（兼容 `filename`）、`FileOffsetInSrc`（页单位 ×pageSize）、`SizeInByteInSrc`、`SizeInSectorInSrc`、`md5`、`sha256`、`sparse`
  - 组语义映射：`Sahara` → `fullDecrypt=true`；`Firmware`/`DigestsToSign`/`ChainedTableOfDigests` → `encryptedSize=0`（明文）；其余 → `encryptedSize = min(0x40000, size)`（参考 `decryptitem()` L247-272 的 `decryptsize` 逻辑）
  - MTK 分支：解混淆头取 prjname/version；文件表条目 0x60B：`name[32]`、`start u64@32`、`length u64@40`、`encrypted_length u64@48`、`filename[32]@56`；`encryptedSize = encrypted_length`；`fullDecrypt=false`
  - 越界防护：offset+size > fileSize → error 拒绝
  - 密码 ZIP 老包：头 2 字节为 `PK`（或 QC 试解全部失败且文件头是 `PK`）→ `*error = "旧式密码 ZIP 打包的 OFP 暂不支持"` 返回 false（spec §7）

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_oppo_ofp`
Expected: `Totals: 4 passed, 0 failed`

- [ ] **Step 5: 提交**

```bash
git add src/image_engine/oppo_ofp.h src/image_engine/oppo_ofp.cpp tests/test_oppo_ofp.cpp CMakeLists.txt
git commit -m "feat(oppo): OFP 识别与解析（QC/MTK 变体，key 试解 + 合成包测试）"
```

---

### Task 4: OFP 解包（流式提取 + 校验）

**Files:**
- Create: `src/image_engine/oppo_extract.h`
- Create: `src/image_engine/oppo_extract.cpp`（OFP + OPS 提取统一放这里）
- Create: `tests/test_oppo_extract.cpp`
- Modify: `CMakeLists.txt`（注册测试）

**Interfaces:**
- Consumes: `parseOFP`（Task 3）、`aes128CfbDecrypt`（Task 1）
- Produces: `src/image_engine/oppo_extract.h`

```cpp
namespace imgopp {

// 进度回调：当前文件名 + 0-100
using ExtractProgress = std::function<void(const QString &name, int percent)>;

// 解包整个包到 outDir。产物: outDir/<file.name>。
// 校验清单中给出的 md5/sha256（如提供）：不匹配写 error 并返回 false（不回滚已写文件）。
bool extractOFP(const QString &path, const QString &outDir,
                const ExtractProgress &progress, QString *error);

} // namespace imgopp
```

- [ ] **Step 1: 写失败测试**

`tests/test_oppo_extract.cpp`：复用 Task 3 的合成包构造（**把 `buildSyntheticQcPackage` 提为共享 helper**——放 `tests/oppo_test_helpers.h`，Task 3/4/5 共用；Task 3 实现时若尚未抽出，本任务先抽出并把 Task 3 测试改为引用）：

```cpp
#include <QtTest>
#include <QTemporaryDir>
#include "image_engine/oppo_extract.h"
#include "image_engine/oppo_ofp.h"
#include "oppo_test_helpers.h"

class TestOppoExtract : public QObject
{
    Q_OBJECT
private slots:
    void extractQcSynthetic();
    void extractRejectsBadChecksum();
};

void TestOppoExtract::extractQcSynthetic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pkgPath = dir.filePath("test.ofp");
    QFile f(pkgPath); QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildSyntheticQcPackage()); f.close();

    const QString outDir = dir.filePath("out");
    QDir().mkpath(outDir);
    QString err;
    QString lastProgressName;
    int lastPercent = -1;
    const bool ok = imgopp::extractOFP(pkgPath, outDir,
        [&](const QString &n, int p) { lastProgressName = n; lastPercent = p; }, &err);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY(QFile::exists(outDir + "/prog_ufs_firehose_test.elf"));
    QVERIFY(QFile::exists(outDir + "/boot.img"));
    // 数据区全 0xAA → boot.img 应全 0xAA（Firmware 组明文）
    QFile boot(outDir + "/boot.img");
    QVERIFY(boot.open(QIODevice::ReadOnly));
    const QByteArray bootData = boot.readAll();
    QCOMPARE(bootData, QByteArray(4096, '\xAA'));
    QCOMPARE(lastPercent, 100);
}

void TestOppoExtract::extractRejectsBadChecksum()
{
    // 合成包把 boot.img 的 sha256 填成全 0 → 提取应失败并写 error
    // （实现者按 helper 参数化 sha256 字段）
}

QTEST_APPLESS_MAIN(TestOppoExtract)
#include "test_oppo_extract.moc"
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败。

- [ ] **Step 3: 实现 extractOFP**

对照 `ofp_qc_decrypt.py` `decryptfile()`/`copy()`（L154-243）：
- 打开包 `QFile`，按 `OfpFile.offset/size` seek 读取
- `fullDecrypt==true`：整段 `aes128CfbDecrypt(data, key, iv)`（key/iv 从 `parseOFP` 之后的命中候选传入——**`parseOFP` 需扩展 `OfpInfo` 增 `QByteArray key, iv` 字段供提取用**，本任务补上）
- `encryptedSize > 0 && !fullDecrypt`：前 `encryptedSize` 字节解密，余下原样拷贝；分块 1MB 流式（`copysub` 同款）
- `encryptedSize == 0`：纯拷贝
- 写 `outDir/<name>`；文件名净化：拒绝含 `/` 或 `..` 的名字（写 error 跳过该文件）
- 输出路径守卫：产物路径与包路径相同时拒绝（spec §5；沿用 worker 既有同路径守卫语义）
- `OfpFile.sparse == true` 时在进度回调名后标注（日志文案 `"<name>（sparse 镜像，原样输出）"`），不自动转 raw（spec §4）
- 校验：清单 `md5`/`sha256` 非空则用 `QCryptographicHash` 校验（`.toHex()` 比较，大小写不敏感）；不符 → `*error = "校验失败: <name> sha256 不匹配"`，返回 false
- 进度：每文件完成回调 `(name, 已完成字节/总字节 ×100)`

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_oppo_extract`
Expected: `Totals: 2 passed, 0 failed`

- [ ] **Step 5: 提交**

```bash
git add src/image_engine/oppo_extract.h src/image_engine/oppo_extract.cpp tests/test_oppo_extract.cpp tests/oppo_test_helpers.h CMakeLists.txt
git commit -m "feat(oppo): OFP 流式解包 + 校验（Firmware 明文/Sahara 全解/首 0x40000 解密三策略）"
```

---

### Task 5: OPS 识别 + 自定义密码 + 解包

**Files:**
- Create: `src/image_engine/oppo_ops.h`
- Create: `src/image_engine/oppo_ops.cpp`
- Modify: `src/image_engine/oppo_extract.cpp`（补 `extractOPS`）
- Modify: `src/image_engine/oppo_keys.cpp`（实现 `opsDecrypt` 实体）
- Create: `tests/test_oppo_ops.cpp`
- Modify: `CMakeLists.txt`（注册测试）

**Interfaces:**
- Consumes: `opsKeyCandidates`/`opsDecrypt`（Task 2 桩）、`aes128CfbDecrypt`（Task 1）
- Produces:

```cpp
// oppo_ops.h
namespace imgopp {
struct OpsInfo {
    QString projectId;    // 尾页 +0x1C 16B ASCII
    QString firmwareName; // 尾页 +0x2C
    quint64 settingsOffset = 0;  // 字节
    quint32 settingsLength = 0;  // 加密长度
    QString keyId;        // 命中的 mbox
};
bool detectOPS(const QByteArray &tail, quint64 fileSize);
bool parseOPS(const QString &path, OpsInfo &info, QString *error);
}
// oppo_extract.h 补：
bool extractOPS(const QString &path, const QString &outDir,
                const ExtractProgress &progress, QString *error);
```

- [ ] **Step 1: 写失败测试**

`tests/test_oppo_ops.cpp`：
- `opsCipherAgainstPython()`：**核心对拍**——用 `reference/oppo_decrypt/opscrypto.py` 的算法在测试里构造已知答案：实现者先跑
  ```bash
  cd reference/oppo_decrypt && printf 'TestBlock1234567' > /tmp/opspt.bin && python3 - <<'EOF'
  # 用 opscrypto.py 的 OpsCrypto 对 /tmp/opspt.bin 加密(ops_encrypt 路径)并打印 hex
  # （实现者按 opscrypto.py 当前 API 自行调用；若 API 不便于独立调用，
  #   则改用 FirmwareKit.Oppo 的 OpsFormatCryptoProvider 自测用例作为向量来源）
  EOF
  ```
  把得到的密文 hex 固化为测试向量，断言 `imgopp::opsDecrypt(ct, mbox5Key, mboxBlob).left(16) == "TestBlock1234567"`。**若两步都拿不到可用向量**：退而写自洽测试（`opsDecrypt` 加密路径与解密路径互为逆，若实现同时提供 `opsEncrypt` 便于测试），并在测试注释记录"真实样本验证待补"。
- `detectOpsFromTail()`：合成尾页（version=2 @0, flags=1 @4, magic 0x7CEF @0x10）→ `detectOPS` true；QC 尾页（version=0）→ false
- `parseOpsSettings()`：构造 settings.xml 明文 → 用测试向量密文替换 → 断言 parseOPS 命中 keyId 且 projectId 解析正确

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败。

- [ ] **Step 3: 实现 opsDecrypt（移植自定义密码）**

对照 `reference/oppo_decrypt/opscrypto.py` 与 `reference/FirmwareKit.Oppo/FirmwareKit.OpsReader/Crypto/OpsFormatCryptoProvider.cs`（双源核对）：
- 状态初始化 = 常量 `d1b5e39e5eea049d671dd5abd2afcbaf` 的 4 个 LE word
- 轮函数 = AES S-box/T-table 风格 4-word 10 轮构造；轮密钥来自 mbox blob（前 16B 密钥材料；blob[0x3C]=0x0A 轮数）
- 整 16B 块：块加密后与密文异或（CFB），状态反馈用密文块
- 尾块 <16B：走 S-box 路径（`ProcessSubBlock` 等价）
- **实现时必须逐行对照两个参照源，注释标注每段对应的参照文件与函数名**

- [ ] **Step 4: 实现 detectOPS / parseOPS / extractOPS**

- `detectOPS`：tail 末 0x200：`version==2 && flags==1 && magic==0x7CEF`
- `parseOPS`：读尾页字段；`settingsOffset = configSectorPos × 0x200`；按 `opsKeyCandidates()` 逐个试解 settings.xml（判定 = 解密结果含 `"<?xml"` 或 `"xml "`）；记录命中 keyId；越界防护同上
- `extractOPS`：SAHARA 组文件（从 settings.xml 解析出的清单：bkerler `opscrypto.py` 的 settings 结构与分组名——实现时对照该文件的确切标签名）全量解密；UFS_PROVISION / Program 组原样拷贝；产物命名与校验同 `extractOFP`

- [ ] **Step 5: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_oppo_ops`
Expected: 全部 passed（对拍向量或自洽测试按 Step 1 决策）

- [ ] **Step 6: 提交**

```bash
git add src/image_engine/oppo_ops.h src/image_engine/oppo_ops.cpp src/image_engine/oppo_keys.cpp src/image_engine/oppo_extract.cpp tests/test_oppo_ops.cpp CMakeLists.txt
git commit -m "feat(oppo): OPS 识别/自定义密码移植/解包（双源对照 + 对拍测试）"
```

---

### Task 6: registry + worker + 面板接线

**Files:**
- Modify: `src/image_engine/registry.h`（Format 枚举加 `OFP`、`OPS`）
- Modify: `src/image_engine/registry.cpp`（扩展名兜底 `.ofp`/`.ops`）
- Modify: `src/ui/image_worker.h/.cpp`（尾页探测 + 解包分派）
- Modify: `src/ui/image_tool_panel.cpp`（信息卡文案 + 动作启用）
- Modify: `tests/test_registry.cpp`（新增探测用例）
- Modify: `CMakeLists.txt`（无——image_worker/panel 在主程序 GLOB）

**Interfaces:**
- Consumes: `detectOFP`/`parseOFP`/`extractOFP`/`extractOPS`/`parseOPS`（Task 3-5）
- Produces: 面板可拖放 `.ofp`/`.ops` → 识别 → 解包按钮 → 产物目录

- [ ] **Step 1: 写失败测试（registry）**

`tests/test_registry.cpp` 加：

```cpp
void TestRegistry::detectOppoByExtension()
{
    QCOMPARE(imgreg::detect(QByteArray(64, 0), "firmware.ofp").format, imgreg::Format::OFP);
    QCOMPARE(imgreg::detect(QByteArray(64, 0), "firmware.ops").format, imgreg::Format::OPS);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build && ./build/image_engine_tests`（registry 测试目标名）
Expected: 编译失败（Format::OFP 不存在）。

- [ ] **Step 3: registry 接线**

- `registry.h`：`enum class Format` 加 `OFP, OPS`（放 `Pac` 附近）
- `registry.cpp` `byExtension()`：`.ofp` → OFP，`.ops` → OPS
- 魔数分支**不加**（无头魔数）；在 detect() 末尾注释说明"OFP/OPS 需尾页探测，由 ImageWorker 二次探测"
- **命名冲突注意**：`registry.cpp` 的匿名命名空间里已有成员函数名 `detect`；二次探测一律写全限定名 `imgopp::detectOFP(...)`，不要引入 `using namespace imgopp`（否则 `detect` 与匿名空间的 `detect()` 在调用点歧义）

- [ ] **Step 4: worker 尾页探测**

`src/ui/image_worker.cpp` `detectImpl()`（约 L316）：在 `imgreg::detect(header, name)` 之后追加——若结果为 Unknown 或按扩展名命中 OFP/OPS：

```cpp
// OFP/OPS 无头魔数，需读文件尾做二次探测（spec: oppo-format-notes.md）
if (result.format == imgreg::Format::Unknown ||
    result.format == imgreg::Format::OFP || result.format == imgreg::Format::OPS) {
    QFile tf(path);
    if (tf.open(QIODevice::ReadOnly) && tf.size() >= 0x1000) {
        tf.seek(tf.size() - 0x1000);
        const QByteArray tail = tf.read(0x1000);
        imgopp::OfpVariant v = imgopp::OfpVariant::Unknown;
        if (imgopp::detectOFP(tail, tf.size(), v)) {
            result.format = imgreg::Format::OFP;
            result.detail = (v == imgopp::OfpVariant::Qc) ? "OPPO/realme OFP 固件包 (QC)"
                                                          : "OPPO/realme OFP 固件包 (MTK)";
        } else if (imgopp::detectOPS(tail, tf.size())) {
            result.format = imgreg::Format::OPS;
            result.detail = "OnePlus OPS 固件包";
        }
    }
}
```
（include `image_engine/oppo_ofp.h`、`image_engine/oppo_ops.h`）

- [ ] **Step 5: 解包分派**

`doUnpack()` switch（约 L645 附近）加两个 case：调 `imgopp::extractOFP`/`extractOPS`，进度回调转发到既有 `unpackProgress` 信号，产物清单填 `outputs` 供 `unpackFinished` 返回。
面板侧：`image_tool_panel.cpp` 的动作启用表/信息卡加 OFP/OPS 条目（文案：`"OPPO/realme 固件包"` / `"OnePlus 固件包"`，动作为"解包"）。

- [ ] **Step 6: 构建 + 全测试 + 手动冒烟**

Run:
```bash
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure -R "oppo|registry" 2>&1 | tail -8
./build/PhoneToolbox   # 手动: 拖入一个 .ofp 文件, 确认识别文案与解包按钮(无真包时用 Task 3 helper 生成的合成包, 存成 /tmp/test.ofp)
```
Expected: 全部测试通过；合成包被识别为 "OPPO/realme OFP 固件包 (QC)" 且可解包。

- [ ] **Step 7: 提交**

```bash
git add src/image_engine/registry.h src/image_engine/registry.cpp src/ui/image_worker.h src/ui/image_worker.cpp src/ui/image_tool_panel.cpp tests/test_registry.cpp
git commit -m "feat(oppo): 镜像面板接线 — OFP/OPS 尾页探测 + 解包分派 (Phase A 收尾)"
```

---

### Task 7: 收尾文档

**Files:**
- Modify: `功能清单.txt`（"规划中"段更新：第 1 项拆出 Phase A 已完成；新增"十三、镜像处理工具"的 OFP/OPS 解包行）
- Modify: `README.md`（镜像处理格式表如有格式清单则加 OFP/OPS）
- Modify: `docs/superpowers/specs/oppo-format-notes.md`（如有实现期修正，回填）

- [ ] **Step 1: 更新功能清单**

在 "--- 解包 (15 种格式真实实现) ---" 段末加：
```
[√] OPPO/realme OFP 解包 (QC: 尾页清单 + AES-128-CFB 分组策略; MTK: 混淆头 + 文件表)
[√] OnePlus OPS 解包 (尾页 + 自定义流密码, SAHARA/settings.xml 解密)
```
"规划中"段第 1 项改写为：
```
[ ] OPPO/一加/realme EDL 刷机链 (Phase B: 解包已交付, 刷写链待接线 — 需真机验证)
[√] OPPO 固件解包引擎 (Phase A 已交付: OFP QC/MTK + OPS, 离线验证)
```

- [ ] **Step 2: 提交**

```bash
git add 功能清单.txt README.md docs/superpowers/specs/oppo-format-notes.md
git commit -m "docs: OPPO 解包引擎 Phase A 交付 — 功能清单/README/格式速查更新"
```

---

## 验证清单（全部完成后）

- [ ] `ctest --test-dir build --output-on-failure` 全绿
- [ ] 手工拖入合成 `.ofp` → 识别 + 解包成功
- [ ] 真实 `.ofp`/`.ops` 样本验证：**待用户提供样本**；未提供时在交付说明中明确"真包待验"
- [ ] 未跟踪文件检查：`git status` 无意外产物（reference/ 已 gitignore）

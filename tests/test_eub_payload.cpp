// tests/test_eub_payload.cpp
//
// 载荷来源三条路径（spec §D8）：裸镜像 / LZ4 frame / BL tar 内查找。
// 本线**无真机、也无真样本**（sboot.bin 是三星签名二进制，不进仓库；facts §F1/§F8）：
// 用例自造合成 sboot（确定性非周期图案），用 imgtar::buildTar / appendMd5Footer 造包 ——
// 不读 reference/ 下的任何二进制、不碰设备。
//
// 参照事实：BL_*.tar.md5 内是 sboot.bin（现代包为 .lz4 压缩，需 lz4.frame.decompress）
// —— facts §C1（reference/hubble/hubble.py:152-183）；本仓 lz4 能力为 LZ4 **frame** 格式
// （src/image_engine/compression/lz4_wrapper.cpp，用 LZ4F_* API）。
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "core/eub/eub_payload.h"
#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/tar_image.h"

class TestEubPayload : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString writeFile(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) qFatal("无法写临时文件");
        f.write(bytes);
        f.close();
        return path;
    }
    static QByteArray syntheticSboot()
    {
        // 非周期填充（xorshift32）：`(i * k) & 0xFF` 仍是 256 周期图案，会让"偏移错 256 的整数倍"漏检
        //（T4 实现者的变异证据：brief 原夹具下 offset 错 0x100 时 split 槽仍全绿）
        QByteArray b(0x8000, '\0');
        quint32 x = 0x12345678u;
        for (int i = 0; i < b.size(); ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = char(x & 0xFF); }
        return b;
    }
    // 同长度、异图案的第二份载荷（用于"取错条目/取错来源"能被 QCOMPARE 立刻判红）
    static QByteArray otherSyntheticSboot()
    {
        QByteArray b = syntheticSboot();
        for (int i = 0; i < b.size(); ++i) b[i] = char(b[i] ^ 0x5A);
        return b;
    }
    // 单条目 tar 的快捷封装
    static QByteArray tarOf(const QString &entryName, const QByteArray &data, bool md5Footer = false)
    {
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = entryName; e.data = data;
        entries << e;
        const QByteArray tar = imgtar::buildTar(entries);
        return md5Footer ? imgtar::appendMd5Footer(tar) : tar;
    }

private slots:
    void rawImageIsLoadedVerbatim()
    {
        const QByteArray img = syntheticSboot();
        // 夹具自检：裸路径槽必须真的走裸路径（带 LZ4 魔数的"裸镜像"是另一条分支）
        QVERIFY(!eub::looksLikeLz4Frame(img));
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("sboot.bin", img), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin")));
        // "裸镜像"三字不在路径里 —— 本断言真正约束描述文案（仅 contains(文件名) 会被"原样回显路径"骗过）
        QVERIFY(src.description.contains(QStringLiteral("裸镜像")));
    }

    void lz4FrameImageIsDecompressed()
    {
        const QByteArray img = syntheticSboot();
        const QByteArray packed = imgcomp::lz4Compress(img);
        QVERIFY(!packed.isEmpty());
        QVERIFY(eub::looksLikeLz4Frame(packed));

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("sboot.bin.lz4", packed), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(src.wasCompressed);
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin.lz4")));
        QVERIFY(src.description.contains(QStringLiteral("LZ4")));   // 同理：路径含 ".lz4" 但不含大写 "LZ4"
    }

    void tarMd5WithPlainSbootIsExtracted()
    {
        const QByteArray img = syntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("sboot.bin"); e.data = img;
        imgtar::TarEntry other; other.name = QStringLiteral("param.bin"); other.data = QByteArray(16, '\x11');
        entries << e << other;
        const QByteArray tar = imgtar::appendMd5Footer(imgtar::buildTar(entries));

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_TEST.tar.md5", tar), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
        // 两条互补判据（原 `contains("tar")` 恒真已替换，T5 审查 Minor 3；改"删除"为"加固"是复审意见）：
        //   ① 条目名出现在描述里 —— 实现若退化成"裸镜像"描述（只回显文件名）即红
        //   ② 模板结构（"<包名> 内的 <条目名>"）在 —— 只回显条目名、不回显包名的实现即红
        // 变异证据（控制方实跑）：把 eub_payload.cpp:187 的 describe(fileName, entryInTar, ...)
        // 改成 describe(fileName, QString(), ...) → 夹在本 slot 的 ① 上变红（13 passed / 1 failed），
        // 其余 13 条（含 raw/lz4 两个同型槽：它们的文件名本就含条目名）全绿，与推演吻合。
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin")));
        QVERIFY(src.description.contains(QStringLiteral("内的")));
    }

    void tarWithLz4SbootIsExtractedThenDecompressed()
    {
        const QByteArray img = syntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("sboot.bin.lz4"); e.data = imgcomp::lz4Compress(img);
        entries << e;
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_LZ4.tar", imgtar::buildTar(entries)), out, &src, &err),
                 qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(src.wasCompressed);
    }

    void tarWithoutSbootFailsAndListsWhatItHas()
    {
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry a; a.name = QStringLiteral("boot.img");   a.data = QByteArray(8, '\x22');
        imgtar::TarEntry b; b.name = QStringLiteral("tzsw.img");   b.data = QByteArray(8, '\x33');
        entries << a << b;

        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");   // 失败路径必须清空 out（仓内约定），预置陈旧字节才有断言力
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("BL_NO.tar", imgtar::buildTar(entries)), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
        QVERIFY(err.contains(QStringLiteral("boot.img")));   // 文案列出包内有什么（可行动）
    }

    void tarEntryIsMatchedByBasenameCaseInsensitively()
    {
        // 要点②：按 basename（最后一段 '/' 之后）大小写不敏感匹配。带目录前缀 + 全大写的条目
        // 仍须命中 —— 大小写敏感或整串比较的实现会在此判红（brief 原用例无覆盖）。
        const QByteArray img = otherSyntheticSboot();
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_SUB.tar.md5",
                                               tarOf(QStringLiteral("firmware/SBOOT.BIN"), img, true)),
                                     out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
    }

    void tarPrefersPlainSbootOverLz4()
    {
        // 要点②：**先找 sboot.bin，没有才找 sboot.bin.lz4**。两条目并存时取错就会解出
        // otherSyntheticSboot() 的图案 → QCOMPARE 判红。.lz4 条目**放在前面**：条目顺序
        // 不能成为实现的依据。
        const QByteArray plain = syntheticSboot();
        const QByteArray viaLz4 = otherSyntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry lz4e;   lz4e.name = QStringLiteral("sboot.bin.lz4"); lz4e.data = imgcomp::lz4Compress(viaLz4);
        imgtar::TarEntry plainE; plainE.name = QStringLiteral("sboot.bin");   plainE.data = plain;
        entries << lz4e << plainE;

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_BOTH.tar", imgtar::buildTar(entries)), out, &src, &err),
                 qPrintable(err));
        QCOMPARE(out, plain);
        QVERIFY(!src.wasCompressed);
    }

    void tarEntryNamedLz4WithPlainContentStaysVerbatim()
    {
        // 要点③：解压判据是**内容**（LZ4 frame 魔数），不是条目扩展名 —— 名字骗人时按裸字节收下。
        // 若实现按扩展名解压，lz4Decompress 对非 frame 数据返回空 → 本槽会以"解压失败"判红。
        const QByteArray img = syntheticSboot();
        QVERIFY(!eub::looksLikeLz4Frame(img));
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_MISNAME.tar", tarOf(QStringLiteral("sboot.bin.lz4"), img)),
                                     out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
    }

    void tarIndexFailureIsNotFallenBackToRaw()
    {
        // 要点①：`indexTarStream` 失败 → **直接报错，不回退裸镜像路径**。把首条目的 size 字段
        // （偏移 124，base-8）破坏成非八进制 → 索引报"size 字段非法"。若实现回退成"整文件读作
        // 裸镜像"，本槽会拿到整个 tar 字节（非空）且返回 true → 两条断言同时判红。
        const QByteArray img = syntheticSboot();
        QByteArray tar = tarOf(QStringLiteral("sboot.bin"), img);
        tar.replace(124, 12, QByteArrayLiteral("zzzzzzzzzzzz"));

        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("BL_BROKEN.tar", tar), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
    }

    void corruptLz4Fails()
    {
        QByteArray junk = QByteArray::fromHex("04224d18") + QByteArray(64, '\x77');  // 魔数对、内容坏
        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("bad.lz4", junk), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
    }

    void emptyInputFails()
    {
        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("empty.bin", QByteArray()), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());

        out = QByteArrayLiteral("stale");
        err.clear();
        QVERIFY(!eub::loadSbootBytes(m_dir.filePath("does-not-exist.bin"), out, &src, &err));
        QVERIFY(!err.isEmpty());   // 打不开的文件也要给中文原因（含路径），不能只返回 false
        QVERIFY(out.isEmpty());
    }

    void lz4MagicDetection()
    {
        QVERIFY(!eub::looksLikeLz4Frame(QByteArray::fromHex("04224d")));    // 太短
        QVERIFY(!eub::looksLikeLz4Frame(QByteArray::fromHex("00000000")));
        QVERIFY(eub::looksLikeLz4Frame(QByteArray::fromHex("04224d18" "00")));
    }
};

QTEST_APPLESS_MAIN(TestEubPayload)
#include "test_eub_payload.moc"

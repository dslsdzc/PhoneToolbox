#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "image_engine/fs/erofs_reader.h"

// EROFS superblock 布局按 Linux 内核 fs/erofs/erofs_fs.h 核对（v5.10~v6.x 及
// erofs-utils master 一致，mkfs.erofs 输出即此布局）:
//   EROFS_SUPER_OFFSET = 1024（偏移 0 为保留区）
//   magic 0xE0F5E1E2 小端落盘 E2 E1 F5 E0 @1024 (4B)
//   feature_compat  LE32 @1032 | blkszbits u8 @1036 (blockSize = 1<<blkszbits)
//   root_nid        LE16 @1038 (48BIT 特性时改用 rootnid_8b LE64 @1136)
//   meta_blkaddr    LE32 @1064 (inode 表起始块)
//   feature_incompat LE32 @1104 | available_compr_algs LE16 @1108 (bit0 = LZ4;
//   algs==0 为 legacy 布局，内核按仅有 LZ4 处理)
//
// B10 目录/inode 布局按 erofs-utils master 与 Linux 6.6+ erofs_fs.h 核对：
//   inode 槽恒 32B（iloc = meta_blkaddr*blksz + nid*32）
//   erofs_inode_compact(32B): i_format(+0, bit0 版本/bit1-3 datalayout/bit4 NLINK_1)
//     i_xattr_icount(+2) i_mode(+4) i_nb(+6) i_size(+8 le32) i_u.startblk_lo(+16)
//   erofs_inode_extended(64B): i_size(+8 le64) i_u(+16) i_nlink(+44 le32)
//   erofs_dirent(12B): nid(+0 le64) nameoff(+8 le16) file_type(+10)
//     —— 目录块 = 定长 dirent 数组 + nameoff 指名的名字区（新格式）
class TestErofs : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseSuper();
    void parseSuperLz4Flag();
    void parseSuper48Bit();
    void invalidInput();
    // B10
    void parseSuperMeta();
    void listTreeFlat();
    void extractFlat();
    void extractInlineTail();
    void compressedFailsWithLz4Marker();
    void traverse48BitExtended();
    void corruptedInputs();
    void depthLimit();
    void startblk48BitCombination();
    // G4 流式/懒加载
    void listTreeLazyFlat();
    void lazyMatchesTree();
    void extractFileStreamMatches();
    void extractFileStreamErrors();
    void streamProgress();
    void imgfsDispatch();
};

static void put16(QByteArray &d, int off, quint32 v)
{
    d[off] = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}
static void put32(QByteArray &d, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static void put64(QByteArray &d, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static void putMagic(QByteArray &d)
{
    d[1024] = char(0xE2); d[1025] = char(0xE1); d[1026] = char(0xF5); d[1027] = char(0xE0);
}

void TestErofs::detect()
{
    QByteArray s(1028, 0);
    putMagic(s);
    QVERIFY(imgerofs::isErofs(s));
    QVERIFY(!imgerofs::isErofs(QByteArray("CrAU")));
    QVERIFY(!imgerofs::isErofs(QByteArray(1024, 0))); // 截断，读不到完整魔数
}

void TestErofs::parseSuper()
{
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(12);   // blkszbits = 4096
    put16(s, 1038, 2);    // root_nid
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(s, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.rootNid, 2ull);
}

void TestErofs::parseSuperLz4Flag()
{
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(12);
    put16(s, 1038, 2);
    imgerofs::SuperBlock sb;
    put16(s, 1108, 0x0001);  // available_compr_algs: bit0 = LZ4
    QVERIFY(imgerofs::parseSuper(s, sb));
    QVERIFY(sb.isLz4);
    put16(s, 1108, 0x0000);  // legacy 布局（algs==0）: 唯一可用算法即 LZ4
    QVERIFY(imgerofs::parseSuper(s, sb));
    QVERIFY(sb.isLz4);
    put16(s, 1108, 0x0002);  // bit0 清零: 无 LZ4
    QVERIFY(imgerofs::parseSuper(s, sb));
    QVERIFY(!sb.isLz4);
}

void TestErofs::parseSuper48Bit()
{
    // EROFS_FEATURE_INCOMPAT_48BIT(0x80) 置位时 root_nid 取 rootnid_8b @1136
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(12);
    put32(s, 1104, 0x00000080);
    put64(s, 1136, 0x123456789ull);
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(s, sb));
    QCOMPARE(sb.rootNid, 0x123456789ull);
}

void TestErofs::invalidInput()
{
    imgerofs::SuperBlock sb;
    QVERIFY(!imgerofs::parseSuper(QByteArray(), sb));          // 空输入
    QVERIFY(!imgerofs::parseSuper(QByteArray(1028, 0), sb));   // 魔数错误
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(3);   // blkszbits 过小
    QVERIFY(!imgerofs::parseSuper(s, sb));
    s[1036] = char(17);  // blkszbits 过大
    QVERIFY(!imgerofs::parseSuper(s, sb));
}

// ===================== B10 镜像构造辅助 =====================

static const int kBlksz = 4096;
static const quint64 kMeta = 4096;   // meta_blkaddr = 1

struct TestDirEntry { quint64 nid; QByteArray name; quint8 ft; };
static TestDirEntry de(quint64 nid, const char *name, quint8 ft = 2)
{
    return { nid, QByteArray(name), ft };
}

// erofs_inode_compact（32B）: fmt/mode/nb/size/startblk/ino 对应 i_format/i_mode/
// i_nb(或 nlink)/i_size/i_u.startblk_lo/i_ino（其余字段 0）
static void putInodeCompact(QByteArray &d, quint64 off, quint16 fmt, quint16 mode,
                            quint16 nb, quint32 size, quint32 startblk,
                            quint32 ino, quint16 xattrIcount = 0)
{
    put16(d, int(off + 0), fmt);
    put16(d, int(off + 2), xattrIcount);
    put16(d, int(off + 4), mode);
    put16(d, int(off + 6), nb);
    put32(d, int(off + 8), size);
    put32(d, int(off + 16), startblk);
    put32(d, int(off + 20), ino);
}

// erofs_inode_extended（64B）
static void putInodeExtended(QByteArray &d, quint64 off, quint16 fmt, quint16 mode,
                             quint16 startblkHi, quint64 size, quint32 startblkLo,
                             quint32 ino, quint32 nlink)
{
    put16(d, int(off + 0), fmt);
    put16(d, int(off + 2), 0);
    put16(d, int(off + 4), mode);
    put16(d, int(off + 6), startblkHi);
    put64(d, int(off + 8), size);
    put32(d, int(off + 16), startblkLo);
    put32(d, int(off + 20), ino);
    put32(d, int(off + 24), 0);   // uid
    put32(d, int(off + 28), 0);   // gid
    put32(d, int(off + 44), nlink);
}

static void putSuper(QByteArray &d, int blkszbits, quint16 rootNid,
                     quint32 metaBlkAddr, quint32 featureIncompat)
{
    putMagic(d);
    d[1024 + 12] = char(blkszbits);
    put16(d, 1024 + 14, rootNid);
    put32(d, 1024 + 40, metaBlkAddr);
    put32(d, 1024 + 80, featureIncompat);
    put16(d, 1024 + 84, 0x0001);   // available_compr_algs: bit0 = LZ4
}

// 目录块（fill_dirblock 语义）：dirent 定长 12B 数组从块首，名字区从 N*12 起连续存放
static void putDirBlock(QByteArray &d, quint64 blockOff,
                        const QList<TestDirEntry> &entries)
{
    quint64 p = blockOff;
    quint64 q = blockOff + 12 * quint64(entries.size());
    for (const TestDirEntry &e : entries) {
        put64(d, int(p), e.nid);
        put16(d, int(p + 8), quint32(q - blockOff));   // nameoff（块内相对）
        d[int(p + 10)] = char(e.ft);
        d[int(p + 11)] = char(0);
        memcpy(d.data() + q, e.name.constData(), size_t(e.name.size()));
        p += 12;
        q += quint64(e.name.size());
    }
}

// 最小镜像：root(hello.txt + subdir/inner.txt)，全部 FLAT_PLAIN
//   block0: boot + superblock | block1: inode 表(nid 0..3)
//   block2: root 目录数据 | block3: subdir 目录数据
//   block4: hello.txt 数据 | block5: inner.txt 数据
static QByteArray buildFlatImage()
{
    QByteArray img(6 * kBlksz, 0);
    putSuper(img, 12, 0, 1, 0);
    putInodeCompact(img, kMeta + 0,  0x0000, 0x41ED, 3, 66, 2, 0);   // root 目录
    putInodeCompact(img, kMeta + 32, 0x0010, 0x81A4, 0, 13, 4, 1);   // hello.txt
    putInodeCompact(img, kMeta + 64, 0x0000, 0x41ED, 2, 48, 3, 2);   // subdir
    putInodeCompact(img, kMeta + 96, 0x0010, 0x81A4, 0, 15, 5, 3);   // inner.txt
    putDirBlock(img, 2 * kBlksz, { de(0, "."), de(0, ".."),
                                   de(1, "hello.txt", 1), de(2, "subdir") });
    putDirBlock(img, 3 * kBlksz, { de(2, "."), de(2, ".."), de(3, "inner.txt", 1) });
    memcpy(img.data() + 4 * kBlksz, "Hello, EROFS!", 13);
    memcpy(img.data() + 5 * kBlksz, "inner file data", 15);
    return img;
}

// root(inline.txt)：FLAT_INLINE = 1 个整块(0xAB 填充) + 5B 尾部内联在 inode 之后
static QByteArray buildInlineImage()
{
    QByteArray img(7 * kBlksz, 0);
    putSuper(img, 12, 0, 1, 0);
    putInodeCompact(img, kMeta + 0,  0x0000, 0x41ED, 2, 49, 2, 0);   // root 目录
    putInodeCompact(img, kMeta + 32, 0x0014, 0x81A4, 0, kBlksz + 5, 3, 1); // inline.txt
    putDirBlock(img, 2 * kBlksz, { de(0, "."), de(0, ".."), de(1, "inline.txt", 1) });
    for (int i = 0; i < kBlksz; ++i)
        img[3 * kBlksz + i] = char(0xAB);
    memcpy(img.data() + kMeta + 64, "tail!", 5);   // iloc(nid1)+inode32B+xattr0
    return img;
}

// 同 flat 镜像，但 hello.txt 改为 COMPRESSED_COMPACT（i_format = (3<<1)|NLINK_1）
static QByteArray buildCompressedImage()
{
    QByteArray img = buildFlatImage();
    put16(img, int(kMeta + 32), 0x0016);
    return img;
}

// 48BIT：root 与 big.txt 均用 extended(64B) inode（占 2 槽 → 下一 nid = 2）
static QByteArray build48BitImage()
{
    QByteArray img(4 * kBlksz, 0);
    putSuper(img, 12, 0, 1, 0x00000080);
    putInodeExtended(img, kMeta + 0,  0x0001, 0x41ED, 0, 46, 2, 0, 2);   // root
    putInodeExtended(img, kMeta + 64, 0x0001, 0x81A4, 0, 18, 3, 2, 1);   // big.txt
    putDirBlock(img, 2 * kBlksz, { de(0, "."), de(0, ".."), de(2, "big.txt", 1) });
    memcpy(img.data() + 3 * kBlksz, "48bit file content", 18);
    return img;
}

// 48BIT：compact NLINK_1 inode 携带 startblk_hi=1（mkfs 真实组合：
// need_48bit 且 nlink==1 时保持 compact，i_nb 存 startblk_hi）。
// startblk = (1<<32)|5 = 4294967301，数据在 2^32+5 块远超镜像 →
// 必须报"数据区越界（startblk 4294967301）"而非被掩成 32 位读取块 5。
static QByteArray build48BitHiImage()
{
    QByteArray img(4 * kBlksz, 0);
    putSuper(img, 12, 0, 1, 0x00000080);
    putInodeCompact(img, kMeta + 0,  0x0000, 0x41ED, 2, 49, 2, 0);   // root 目录
    putInodeCompact(img, kMeta + 32, 0x0010, 0x81A4, 1, 19, 5, 1);   // hi.bin
    putDirBlock(img, 2 * kBlksz, { de(0, "."), de(0, ".."), de(1, "hi.bin", 1) });
    return img;
}

// G4：大文件镜像 —— root(big.bin)，big.bin = 640 块（2.5MB，>1 个流式 chunk）
static QByteArray buildBigFileImage()
{
    const int kFileBlocks = 640;
    QByteArray img((11 + kFileBlocks) * kBlksz, 0);
    putSuper(img, 12, 0, 1, 0);
    putInodeCompact(img, kMeta + 0, 0x0000, 0x41ED, 2, kBlksz, 2, 0);   // root 目录
    putInodeCompact(img, kMeta + 32, 0x0010, 0x81A4, 0,
                    quint32(kFileBlocks * kBlksz), 10, 1);   // big.bin
    putDirBlock(img, 2 * kBlksz, { de(0, "."), de(0, ".."), de(1, "big.bin", 1) });
    for (int i = 0; i < kFileBlocks * kBlksz; ++i)
        img[10 * kBlksz + i] = char(0x5A);
    return img;
}

// 深层目录链：root(nid0) -> d1(nid1) -> ... -> d{depth}(nid{depth})，每层仅一个 "d" 目录项
static QByteArray buildDeepChainImage(int depth)
{
    const int ninodes = depth + 1;                       // root + depth 个子目录
    const int metaBlocks = (ninodes * 32 + kBlksz - 1) / kBlksz;
    const int dirStartBlock = 1 + metaBlocks;            // 目录数据块起始
    QByteArray img((dirStartBlock + depth + 1) * kBlksz, 0);
    putSuper(img, 12, 0, 1, 0);
    for (int i = 0; i < ninodes; ++i) {
        const quint64 iloc = kMeta + quint64(i) * 32;
        // 中间层目录："." ".." "d"（40B）；最深层：仅 "." ".."（27B）
        putInodeCompact(img, iloc, 0x0000, 0x41ED, 2,
                        quint32(i == depth ? 27 : 40),
                        quint32(dirStartBlock + i), quint32(i));
    }
    for (int i = 0; i < depth; ++i)
        putDirBlock(img, quint64(dirStartBlock + i) * kBlksz,
                    { de(quint64(i), "."), de(quint64(i), ".."),
                      de(quint64(i + 1), "d", 2) });
    putDirBlock(img, quint64(dirStartBlock + depth) * kBlksz,
                { de(quint64(depth), "."), de(quint64(depth), "..") });
    return img;
}

// ===================== B10 测试槽 =====================

void TestErofs::parseSuperMeta()
{
    QByteArray img = buildFlatImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.metaBlkAddr, 1u);
    QCOMPARE(sb.featureIncompat, 0u);
    QCOMPARE(sb.rootNid, 0ull);
}

void TestErofs::listTreeFlat()
{
    QByteArray img = buildFlatImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgerofs::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 3);

    const imgfs::FsEntry *hello = nullptr, *sub = nullptr, *inner = nullptr;
    for (const imgfs::FsEntry &e : out) {
        if (e.path == QLatin1String("hello.txt")) hello = &e;
        else if (e.path == QLatin1String("subdir")) sub = &e;
        else if (e.path == QLatin1String("subdir/inner.txt")) inner = &e;
    }
    QVERIFY(hello != nullptr);
    QVERIFY(sub != nullptr);
    QVERIFY(inner != nullptr);
    QVERIFY(!hello->isDir);
    QCOMPARE(hello->size, 13ull);
    QVERIFY(sub->isDir);
    QCOMPARE(sub->size, 48ull);
    QVERIFY(!inner->isDir);
    QCOMPARE(inner->size, 15ull);
}

void TestErofs::extractFlat()
{
    QByteArray img = buildFlatImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QByteArray data;
    QString err;
    QVERIFY2(imgerofs::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("Hello, EROFS!"));
    QVERIFY2(imgerofs::extractFile(img, sb, "/subdir/inner.txt", data, &err),
             qPrintable(err));
    QCOMPARE(data, QByteArray("inner file data"));
    // 目录不能提取
    QVERIFY(!imgerofs::extractFile(img, sb, "subdir", data, &err));
    QVERIFY(!err.isEmpty());
}

void TestErofs::extractInlineTail()
{
    QByteArray img = buildInlineImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QByteArray data;
    QString err;
    QVERIFY2(imgerofs::extractFile(img, sb, "inline.txt", data, &err), qPrintable(err));
    QCOMPARE(data.size(), kBlksz + 5);
    QCOMPARE(data.mid(kBlksz), QByteArray("tail!"));
    for (int i = 0; i < kBlksz; ++i)
        QCOMPARE(int(uchar(data[i])), 0xAB);
}

void TestErofs::compressedFailsWithLz4Marker()
{
    QByteArray img = buildCompressedImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QByteArray data;
    QString err;
    QVERIFY(!imgerofs::extractFile(img, sb, "hello.txt", data, &err));
    QVERIFY(err.contains("LZ4"));
    // 未压缩文件不受影响
    QVERIFY2(imgerofs::extractFile(img, sb, "subdir/inner.txt", data, &err),
             qPrintable(err));
    QCOMPARE(data, QByteArray("inner file data"));
}

void TestErofs::traverse48BitExtended()
{
    QByteArray img = build48BitImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgerofs::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].path, QString("big.txt"));
    QCOMPARE(out[0].size, 18ull);
    QByteArray data;
    QVERIFY2(imgerofs::extractFile(img, sb, "big.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("48bit file content"));
}

void TestErofs::corruptedInputs()
{
    imgerofs::SuperBlock sb;
    QString err;
    QByteArray data;
    QList<imgfs::FsEntry> out;

    // 空输入（sb 默认合法但镜像为空）
    QVERIFY(!imgerofs::listTree(QByteArray(), sb, out, &err));
    QVERIFY(!err.isEmpty());

    QByteArray img = buildFlatImage();
    QVERIFY(imgerofs::parseSuper(img, sb));

    // 截断：hello.txt 数据区不完整
    QByteArray trunc = img.left(4 * kBlksz + 5);
    err.clear();
    QVERIFY(!imgerofs::extractFile(trunc, sb, "hello.txt", data, &err));
    QVERIFY(!err.isEmpty());

    // 截断：inode 表不完整
    QByteArray trunc2 = img.left(4096 + 40);
    err.clear();
    QVERIFY(!imgerofs::listTree(trunc2, sb, out, &err));
    QVERIFY(!err.isEmpty());

    // 无效 nameoff（de[0].nameoff = 0）
    QByteArray bad = buildFlatImage();
    put16(bad, 2 * kBlksz + 8, 0);
    err.clear();
    QVERIFY(!imgerofs::listTree(bad, sb, out, &err));
    QVERIFY(!err.isEmpty());

    // 名字长度 0（de[1].nameoff == de[0].nameoff）
    QByteArray bad2 = buildFlatImage();
    put16(bad2, 2 * kBlksz + 12 + 8, 48);
    err.clear();
    QVERIFY(!imgerofs::listTree(bad2, sb, out, &err));
    QVERIFY(!err.isEmpty());

    // 不存在的路径 / 空路径 / 含 '.' 的路径
    QVERIFY(!imgerofs::extractFile(img, sb, "nope.txt", data, &err));
    QVERIFY(!imgerofs::extractFile(img, sb, QString(), data, &err));
    QVERIFY(!imgerofs::extractFile(img, sb, "/", data, &err));
    QVERIFY(!imgerofs::extractFile(img, sb, "./hello.txt", data, &err));

    // 无效 SuperBlock（blockSize 非法）
    imgerofs::SuperBlock badSb = sb;
    badSb.blockSize = 0;
    err.clear();
    QVERIFY(!imgerofs::listTree(img, badSb, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestErofs::depthLimit()
{
    imgerofs::SuperBlock sb;
    QString err;
    QList<imgfs::FsEntry> out;

    // 140 层无环目录链（约 580KB）→ 深度超限报错，不栈溢出
    QByteArray deep = buildDeepChainImage(140);
    QVERIFY(imgerofs::parseSuper(deep, sb));
    QVERIFY(!imgerofs::listTree(deep, sb, out, &err));
    QVERIFY(err.contains("目录深度超限"));

    // 100 层仍在限制内（正向控制）
    QByteArray ok = buildDeepChainImage(100);
    QVERIFY(imgerofs::parseSuper(ok, sb));
    err.clear();
    QVERIFY2(imgerofs::listTree(ok, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 100);
}

void TestErofs::startblk48BitCombination()
{
    QByteArray img = build48BitHiImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    QString err;
    QByteArray data;
    QList<imgfs::FsEntry> out;

    // 遍历正常（hi.bin 是文件，不递归）
    QVERIFY2(imgerofs::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].path, QString("hi.bin"));

    // startblk = (1<<32)|5 = 4294967301：若被掩成 32 位会静默读块 5（无报错）
    QVERIFY(!imgerofs::extractFile(img, sb, "hi.bin", data, &err));
    QVERIFY(err.contains("4294967301"));
}

// ===================== G4 流式/懒加载用例 =====================

// 镜像写入临时文件（失败返回空路径）
static QString writeTempImage(const QByteArray &img, QTemporaryDir &dir, const QString &name)
{
    const QString path = dir.path() + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    if (f.write(img) != img.size())
        return QString();
    return path;
}

void TestErofs::listTreeLazyFlat()
{
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    const QByteArray img = buildFlatImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    const QString path = writeTempImage(img, dir, QStringLiteral("fs.img"));
    QVERIFY(!path.isEmpty());

    imgfs::FsFile f;
    QString err;
    QVERIFY2(f.open(path, &err), qPrintable(err));
    imgerofs::SuperBlock fsb;
    QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));

    QList<imgfs::FsEntry> out;
    // 根：直接子项 = hello.txt + subdir（不含 subdir 内部）
    QVERIFY2(imgerofs::listTreeLazy(f, fsb, QString(), out, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out[0].path, QString("hello.txt"));
    QVERIFY(!out[0].isDir);
    QCOMPARE(out[0].size, 13ull);
    QCOMPARE(out[1].path, QString("subdir"));
    QVERIFY(out[1].isDir);

    // 子目录：只有直接子项 subdir/inner.txt（无递归）
    QVERIFY2(imgerofs::listTreeLazy(f, fsb, QStringLiteral("subdir"), out, &err),
             qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].path, QString("subdir/inner.txt"));
    QCOMPARE(out[0].size, 15ull);

    // "/" 与 "/subdir/" 规范化等价
    QVERIFY2(imgerofs::listTreeLazy(f, fsb, QStringLiteral("/"), out, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);
    QVERIFY2(imgerofs::listTreeLazy(f, fsb, QStringLiteral("/subdir/"), out, &err),
             qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].path, QString("subdir/inner.txt"));

    // 不存在的目录 / 文件路径 → false + error
    err.clear();
    QVERIFY(!imgerofs::listTreeLazy(f, fsb, QStringLiteral("nope"), out, &err));
    QVERIFY(!err.isEmpty());
    err.clear();
    QVERIFY(!imgerofs::listTreeLazy(f, fsb, QStringLiteral("hello.txt"), out, &err));
    QVERIFY(err.contains("不是目录"));
}

void TestErofs::lazyMatchesTree()
{
    // 增量加载与全树 listTree 条目集合一致：从根逐目录 lazy 列出并收集
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    const QByteArray img = buildFlatImage();
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(img, sb));
    const QString path = writeTempImage(img, dir, QStringLiteral("fs.img"));
    QVERIFY(!path.isEmpty());

    QList<imgfs::FsEntry> tree;
    QString err;
    QVERIFY2(imgerofs::listTree(img, sb, tree, &err), qPrintable(err));

    imgfs::FsFile f;
    QVERIFY2(f.open(path, &err), qPrintable(err));
    imgerofs::SuperBlock fsb;
    QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));

    QSet<QString> lazyAll;
    QList<QString> pending;
    pending << QString();
    while (!pending.isEmpty()) {
        const QString d = pending.takeFirst();
        QList<imgfs::FsEntry> entries;
        QVERIFY2(imgerofs::listTreeLazy(f, fsb, d, entries, &err), qPrintable(err));
        for (const imgfs::FsEntry &e : entries) {
            lazyAll.insert(e.path);
            if (e.isDir)
                pending << e.path;
        }
    }
    QCOMPARE(lazyAll.size(), tree.size());
    for (const imgfs::FsEntry &e : tree)
        QVERIFY2(lazyAll.contains(e.path), qPrintable(e.path));
}

// 流式提取结果与内存提取逐字节一致
static void verifyStreamMatches(const QByteArray &img, const imgerofs::SuperBlock &sb,
                                const QString &imagePath, const QString &inPath,
                                const QString &outPath, QString &err)
{
    imgfs::FsFile f;
    QVERIFY2(f.open(imagePath, &err), qPrintable(err));
    imgerofs::SuperBlock fsb;
    QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));
    QVERIFY2(imgerofs::extractFileStream(f, fsb, inPath, outPath, {}, &err),
             qPrintable(err));
    QByteArray memData;
    QVERIFY2(imgerofs::extractFile(img, sb, inPath, memData, &err), qPrintable(err));
    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), memData);
}

void TestErofs::extractFileStreamMatches()
{
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    QString err;
    const QString outPath = dir.path() + QStringLiteral("/out.bin");

    // flat：hello.txt（PLAIN 数据区）
    {
        const QByteArray img = buildFlatImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("flat.img"));
        QVERIFY(!path.isEmpty());
        verifyStreamMatches(img, sb, path, QStringLiteral("hello.txt"), outPath, err);
    }
    // inline：inline.txt（1 整块数据区 + 内联尾部）
    {
        const QByteArray img = buildInlineImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("inline.img"));
        QVERIFY(!path.isEmpty());
        verifyStreamMatches(img, sb, path, QStringLiteral("inline.txt"), outPath, err);
    }
    // 48BIT extended inode
    {
        const QByteArray img = build48BitImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("b48.img"));
        QVERIFY(!path.isEmpty());
        verifyStreamMatches(img, sb, path, QStringLiteral("big.txt"), outPath, err);
    }
    // 大文件（>1 chunk，跨多次流式读写）
    {
        const QByteArray img = buildBigFileImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("big.img"));
        QVERIFY(!path.isEmpty());
        verifyStreamMatches(img, sb, path, QStringLiteral("big.bin"), outPath, err);
    }
}

void TestErofs::extractFileStreamErrors()
{
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    QString err;

    // 目录不能提取，且输出文件不被创建
    {
        const QByteArray img = buildFlatImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("flat.img"));
        QVERIFY(!path.isEmpty());
        imgfs::FsFile f;
        QVERIFY2(f.open(path, &err), qPrintable(err));
        imgerofs::SuperBlock fsb;
        QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));
        const QString outPath = dir.path() + QStringLiteral("/dir.out");
        QVERIFY(!imgerofs::extractFileStream(f, fsb, QStringLiteral("subdir"), outPath,
                                             {}, &err));
        QVERIFY(err.contains("目录"));
        QVERIFY(!QFile::exists(outPath));
        // 不存在的路径
        err.clear();
        QVERIFY(!imgerofs::extractFileStream(f, fsb, QStringLiteral("nope.txt"), outPath,
                                             {}, &err));
        QVERIFY(err.contains("路径不存在"));
        QVERIFY(!QFile::exists(outPath));
    }
    // 压缩 inode：LZ4 标记错误；失败时已创建的输出文件被删除
    {
        const QByteArray img = buildCompressedImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("c.img"));
        QVERIFY(!path.isEmpty());
        imgfs::FsFile f;
        QVERIFY2(f.open(path, &err), qPrintable(err));
        imgerofs::SuperBlock fsb;
        QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));
        const QString outPath = dir.path() + QStringLiteral("/c.out");
        QVERIFY(!imgerofs::extractFileStream(f, fsb, QStringLiteral("hello.txt"), outPath,
                                             {}, &err));
        QVERIFY(err.contains("LZ4"));
        QVERIFY(!QFile::exists(outPath));
    }
    // 截断镜像（hello.txt 数据区不完整）→ 报错，不崩溃
    {
        QByteArray img = buildFlatImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img.left(4 * kBlksz + 5), dir,
                                            QStringLiteral("t.img"));
        QVERIFY(!path.isEmpty());
        imgfs::FsFile f;
        QVERIFY2(f.open(path, &err), qPrintable(err));
        imgerofs::SuperBlock fsb;
        QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));
        const QString outPath = dir.path() + QStringLiteral("/t.out");
        QVERIFY(!imgerofs::extractFileStream(f, fsb, QStringLiteral("hello.txt"), outPath,
                                             {}, &err));
        QVERIFY(err.contains("超出镜像范围"));
    }
}

void TestErofs::streamProgress()
{
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    QString err;

    // 小文件：progress(0) → progress(文件大小)
    {
        const QByteArray img = buildFlatImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("flat.img"));
        QVERIFY(!path.isEmpty());
        imgfs::FsFile f;
        QVERIFY2(f.open(path, &err), qPrintable(err));
        imgerofs::SuperBlock fsb;
        QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));
        QList<quint64> calls;
        const QString outPath = dir.path() + QStringLiteral("/p1.out");
        QVERIFY2(imgerofs::extractFileStream(f, fsb, QStringLiteral("hello.txt"), outPath,
                                             [&](quint64 b) { calls.append(b); }, &err),
                 qPrintable(err));
        QVERIFY(calls.size() >= 2);
        for (int i = 1; i < calls.size(); ++i)
            QVERIFY(calls[i] >= calls[i - 1]);
        QCOMPARE(calls.last(), 13ull);
    }
    // 大文件（2.5MB > chunk 1MB）：多次推进，末值 = 文件大小
    {
        const QByteArray img = buildBigFileImage();
        imgerofs::SuperBlock sb;
        QVERIFY(imgerofs::parseSuper(img, sb));
        const QString path = writeTempImage(img, dir, QStringLiteral("big.img"));
        QVERIFY(!path.isEmpty());
        imgfs::FsFile f;
        QVERIFY2(f.open(path, &err), qPrintable(err));
        imgerofs::SuperBlock fsb;
        QVERIFY2(imgerofs::parseSuperFile(f, fsb, &err), qPrintable(err));
        QList<quint64> calls;
        const QString outPath = dir.path() + QStringLiteral("/p2.out");
        QVERIFY2(imgerofs::extractFileStream(f, fsb, QStringLiteral("big.bin"), outPath,
                                             [&](quint64 b) { calls.append(b); }, &err),
                 qPrintable(err));
        const quint64 fileSize = 640ull * kBlksz;
        QVERIFY(calls.size() >= 4);   // 0 + 1MB + 2MB + 2.5MB（+ 末次）
        for (int i = 1; i < calls.size(); ++i)
            QVERIFY(calls[i] >= calls[i - 1]);
        QCOMPARE(calls.last(), fileSize);
    }
}

void TestErofs::imgfsDispatch()
{
    // 顶层 imgfs 接口：打开文件 → 检测 → 分派到 imgerofs
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    const QByteArray img = buildFlatImage();
    const QString path = writeTempImage(img, dir, QStringLiteral("fs.img"));
    QVERIFY(!path.isEmpty());
    QString err;

    QList<imgfs::FsEntry> out;
    QVERIFY2(imgfs::listTreeLazy(path, QStringLiteral("subdir"), out, &err),
             qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].path, QString("subdir/inner.txt"));

    const QString outPath = dir.path() + QStringLiteral("/h.bin");
    QVERIFY2(imgfs::extractFileStream(path, QStringLiteral("hello.txt"), outPath, {}, &err),
             qPrintable(err));
    QFile fout(outPath);
    QVERIFY(fout.open(QIODevice::ReadOnly));
    QCOMPARE(fout.readAll(), QByteArray("Hello, EROFS!"));

    // 非 fs 镜像 → 明确错误（不崩溃）
    const QString garbagePath = dir.path() + QStringLiteral("/g.bin");
    QFile garbage(garbagePath);
    QVERIFY(garbage.open(QIODevice::WriteOnly));
    garbage.write(QByteArray("not an fs image at all...."));
    garbage.close();
    err.clear();
    QVERIFY(!imgfs::listTreeLazy(garbagePath, QString(), out, &err));
    QVERIFY(err.contains("不是文件系统镜像"));
    err.clear();
    QVERIFY(!imgfs::extractFileStream(garbagePath, QStringLiteral("x"), outPath, {}, &err));
    QVERIFY(err.contains("不是文件系统镜像"));
    err.clear();
    QVERIFY(!imgfs::extractFileStream(dir.path() + QStringLiteral("/missing.img"),
                                      QStringLiteral("x"), outPath, {}, &err));
    QVERIFY(err.contains("无法打开镜像文件"));
}

QTEST_APPLESS_MAIN(TestErofs)
#include "test_fs_erofs.moc"

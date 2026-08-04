#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFileInfo>
#include "image_engine/fs/ext4_reader.h"

// 说明：
// 1) brief 原构造把字段写在 0/4/24/88，但内核布局（fs/ext4/ext4.h
//    struct ext4_super_block）中 superblock 位于文件偏移 1024，字段应落在
//    1024/1028/1048/1112（s_magic @1080=1024+56 不变）。已按 e2fsprogs
//    mke2fs 真实镜像 xxd 核对（inodes_count=2048@1024、log_block_size=2@1048、
//    inode_size=256@1112、magic 53 ef@1080、first_ino=11@1108），据此修正偏移。
// 2) B12：目录项按内核 ext4_dir_entry_2（name_len u8@6、file_type u8@7、
//    name@8；brief 的 name_len u16@6/file_type u8@8 与内核不符，已修正），
//    s_inodes_per_group @40（brief 的 @32 是 s_blocks_per_group），inline 数据
//    前 60B 在 i_block、其余在 "system.data" xattr 值（内核 xattr.h 布局），
//    inline 目录 = [父 inode u32][目录项 @4..]，GDT 在 block 1（1K 块时 block 2），
//    全部经 mke2fs 真实镜像 xxd + debugfs 交叉验证。
class TestExt4 : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseSuper();
    void parseSuper64Bit();
    void parseSuperFields();
    void invalidInput();
    // ---- B12 手工构造镜像 ----
    void listTreeHandBuilt();
    void extractHandBuilt();
    void symlinkHandBuilt();
    void inlineDataHandBuilt();
    void extentDepth1();
    void htreeHandBuilt();
    void oldFormatDir();
    void depthLimit();
    void corruptedInputs();
    void replaceHandBuilt();
    void replaceInvalid();
    // ---- 评审修复：extent 环 / ee_len 截断 / 64BIT 计数双加 ----
    void extentCycle();
    void replaceHugeRun();
    void freeCounts64Bit();
    void replaceSparseFile();
    // ---- B12 真实 mke2fs 镜像 ----
    void realListExtract();
    void realReplaceRoundTrip();
    void realInlineData();
};

// ===================== 构造辅助 =====================

static const int kBlk = 4096;
static const int kTableBlock = 4;              // 手工镜像 inode 表块
static const quint32 kInodesPerGroup = 32;     // 手工镜像恒 1 个块组
static const quint32 kBlocksPerGroup = 32768;

static void put16(QByteArray &d, int off, quint32 v)
{
    d[off] = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}
static void put32(QByteArray &d, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static void put64(QByteArray &d, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static quint16 rd16(const QByteArray &d, int off)
{
    return quint16(uchar(d[off])) | (quint16(uchar(d[off + 1])) << 8);
}
static quint32 rd32(const QByteArray &d, int off)
{
    return quint32(uchar(d[off])) | (quint32(uchar(d[off + 1])) << 8) |
           (quint32(uchar(d[off + 2])) << 16) | (quint32(uchar(d[off + 3])) << 24);
}

// superblock（文件偏移 1024 起）；inodeSize 256 时 extra_isize=32（inline 测试用）。
// 默认带 FILETYPE(0x2) 特性（真实 ext4 恒有；无此特性时目录项为 16 位 name_len 老格式）
static void putSuper(QByteArray &d, quint32 inodeCount, quint32 blockCount,
                     quint32 inodeSize, quint32 incompat = 0x2)
{
    d[1080] = char(0x53); d[1081] = char(0xEF);
    put32(d, 1024, inodeCount);
    put32(d, 1028, blockCount);
    put32(d, 1036, 0xFFFF);                    // free_blocks（replace 测试更新）
    put32(d, 1044, 0);                         // s_first_data_block（4096B 块 = 0）
    d[1048] = char(2);                         // log_block_size → 4096
    put32(d, 1056, kBlocksPerGroup);
    put32(d, 1064, kInodesPerGroup);
    put32(d, 1112, inodeSize);
    put32(d, 1120, incompat);                  // feature_incompat
    put16(d, 1278, (incompat & 0x80) ? 64 : 32);   // s_desc_size
}

// 块组描述符（32B）@block 1
static void putDesc(QByteArray &d, int idx, quint32 bitmap, quint32 inodeBitmap,
                    quint32 inodeTable, quint32 freeBlocks = 0xFFFF)
{
    const int off = kBlk + idx * 32;
    put32(d, off + 0, bitmap);
    put32(d, off + 4, inodeBitmap);
    put32(d, off + 8, inodeTable);
    put16(d, off + 12, freeBlocks & 0xFFFF);
}

// inode 表项（128B）：mode/size_lo/flags；extent 与 inline 数据另行写入
static void putInode128(QByteArray &d, int nid, quint16 mode, quint32 size,
                        quint32 flags, int tableOff = kTableBlock * kBlk)
{
    const int off = tableOff + (nid - 1) * 128;
    put16(d, off + 0, mode);
    put32(d, off + 4, size);
    put32(d, off + 32, flags);
}
// inode 表项（256B，inline 测试用）：extra_isize=32 @128
static void putInode256(QByteArray &d, int nid, quint16 mode, quint32 size,
                        quint32 flags)
{
    const int off = kTableBlock * kBlk + (nid - 1) * 256;
    put16(d, off + 0, mode);
    put32(d, off + 4, size);
    put32(d, off + 32, flags);
    put16(d, off + 128, 32);
}
static int inoOff(int nid, int inodeSize = 128)
{
    return kTableBlock * kBlk + (nid - 1) * inodeSize;
}

// extent 根（depth 0）：header + 若干 extent
static void putExtents(QByteArray &d, int inodeOff,
                       const QList<QPair<quint32, quint32>> &exts)  // (pblock, len)
{
    const int off = inodeOff + 40;
    put16(d, off + 0, 0xF30A);
    put16(d, off + 2, quint32(exts.size()));
    put16(d, off + 4, 4);
    d[off + 6] = char(0);
    put32(d, off + 8, 0);
    for (int i = 0; i < exts.size(); ++i) {
        const int e = off + 12 + i * 12;
        put32(d, e + 0, quint32(i ? 1 : 0));    // ee_block（简化：连续逻辑块）
        put16(d, e + 4, exts[i].second);
        put16(d, e + 6, 0);
        put32(d, e + 8, exts[i].first);
    }
}

struct TestDirEntry { quint32 nid; const char *name; quint8 ft; };
static TestDirEntry de(quint32 nid, const char *name, quint8 ft = 2)
{
    return { nid, name, ft };
}

// 线性目录块：非末项最小 rec_len，末项延伸到块尾；oldFormat=true 时按
// ext2_dir_entry 老格式写 16 位 name_len（bytes 6-7），无 file_type 字节
static void putDirBlock(QByteArray &d, int blockOff,
                        const QList<TestDirEntry> &entries, bool oldFormat = false,
                        int blockSize = kBlk)
{
    int pos = 0;
    for (int i = 0; i < entries.size(); ++i) {
        const int nameLen = int(qstrlen(entries[i].name));
        const bool last = (i == entries.size() - 1);
        const quint32 recLen = last ? quint32(blockSize - pos) : quint32((8 + nameLen + 3) & ~3);
        put32(d, blockOff + pos, entries[i].nid);
        put16(d, blockOff + pos + 4, recLen);
        if (oldFormat) {
            put16(d, blockOff + pos + 6, quint32(nameLen));   // 16 位 name_len
        } else {
            d[blockOff + pos + 6] = char(nameLen);
            d[blockOff + pos + 7] = char(entries[i].ft);
        }
        memcpy(d.data() + blockOff + pos + 8, entries[i].name, nameLen);
        pos += int(recLen);
    }
}

// 块位图标记使用（bit i = block i）
static void markUsed(QByteArray &d, int block)
{
    d[2 * kBlk + block / 8] = char(uchar(d[2 * kBlk + block / 8]) | (0x80u >> (block & 7)));
}

// 最小镜像：root(hello.txt + sub/inner.txt)
//   block0: 引导+super | block1: GDT | block2: 块位图 | block3: inode 位图
//   block4: inode 表（128B×16）| block5: root 目录 | block6: sub 目录
//   block7: hello.txt 数据 | block8: inner.txt 数据
//   block9..12: 空闲（replace 增长测试要分配 3 块 9/10/11，断言 bit12 仍空闲）
static QByteArray buildFlatImage()
{
    QByteArray img(13 * kBlk, 0);
    putSuper(img, 16, 2048, 128);
    putDesc(img, 0, 2, 3, kTableBlock);
    for (int b = 0; b <= 8; ++b)
        markUsed(img, b);
    putInode128(img, 2, 0x41ED, kBlk, 0x80000);          // root 目录
    putInode128(img, 11, 0x81A4, 12, 0x80000);           // hello.txt
    putInode128(img, 12, 0x41ED, kBlk, 0x80000);         // sub 目录
    putInode128(img, 13, 0x81A4, 15, 0x80000);           // inner.txt
    putExtents(img, inoOff(2), { { 5, 1 } });
    putExtents(img, inoOff(11), { { 7, 1 } });
    putExtents(img, inoOff(12), { { 6, 1 } });
    putExtents(img, inoOff(13), { { 8, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."),
                                 de(11, "hello.txt", 1), de(12, "sub") });
    putDirBlock(img, 6 * kBlk, { de(12, "."), de(12, ".."), de(13, "inner.txt", 1) });
    memcpy(img.data() + 7 * kBlk, "Hello, ext4!", 12);
    memcpy(img.data() + 8 * kBlk, "inner file data", 15);
    return img;
}

// 稀疏文件镜像：hello.txt 两个 extent（lb0→块7、lb2→块9），逻辑块 1 为空洞，
// i_size = 8192（2 块）→ 容量 2 块。块 9 因此必须标记占用。
// 旧实现原地替换（容量内）不校验逻辑连续性：数据顺序写进两个 extent 物理块，
// 自校验按 extent 拼接恰好通过 → 静默错误成功（内核视角空洞处读零、内容错乱）。
static QByteArray buildSparseFileImage()
{
    QByteArray img(13 * kBlk, 0);
    putSuper(img, 16, 2048, 128);
    putDesc(img, 0, 2, 3, kTableBlock);
    for (int b = 0; b <= 9; ++b)
        markUsed(img, b);
    putInode128(img, 2, 0x41ED, kBlk, 0x80000);
    putInode128(img, 11, 0x81A4, 2 * kBlk, 0x80000);   // hello.txt：2 逻辑块
    putInode128(img, 12, 0x41ED, kBlk, 0x80000);
    putInode128(img, 13, 0x81A4, 15, 0x80000);
    putExtents(img, inoOff(2), { { 5, 1 } });
    putExtents(img, inoOff(12), { { 6, 1 } });
    putExtents(img, inoOff(13), { { 8, 1 } });
    // hello.txt 手工写 extent：lb0→块7、lb2→块9（逻辑块 1 空洞）
    const int off = inoOff(11) + 40;
    put16(img, off + 0, 0xF30A);
    put16(img, off + 2, 2);
    put16(img, off + 4, 4);
    img[off + 6] = char(0);
    put32(img, off + 8, 0);
    put32(img, off + 12, 0);   // ee_block = 0
    put16(img, off + 16, 1);
    put32(img, off + 20, 7);
    put32(img, off + 24, 2);   // ee_block = 2
    put16(img, off + 28, 1);
    put32(img, off + 32, 9);
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."),
                                 de(11, "hello.txt", 1), de(12, "sub") });
    putDirBlock(img, 6 * kBlk, { de(12, "."), de(12, ".."), de(13, "inner.txt", 1) });
    memcpy(img.data() + 7 * kBlk, "Hello, ext4!", 12);
    memcpy(img.data() + 8 * kBlk, "inner file data", 15);
    memcpy(img.data() + 9 * kBlk, "holey second block", 18);
    return img;
}

// 深层目录链：root -> d1 -> ... -> d{depth}
static QByteArray buildDeepChainImage(int depth)
{
    const int ninodes = depth + 1;
    const int tableBlocks = (ninodes * 128 + kBlk - 1) / kBlk;
    const int dirStart = kTableBlock + tableBlocks;
    // 目录块共 depth+1 个（root + 每层一个）：dirStart..dirStart+depth，
    // 镜像须容纳到 dirStart+depth（+1 块）
    QByteArray img((dirStart + depth + 1) * kBlk, 0);
    putSuper(img, quint32(2 + ninodes), 2048, 128);
    put32(img, 1064, 512);             // inodes_per_group 放大 → 恒 1 个块组
    putDesc(img, 0, 2, 3, kTableBlock);
    for (int i = 0; i < ninodes; ++i) {
        putInode128(img, 2 + i, 0x41ED, 36, 0x80000);
        putExtents(img, inoOff(2 + i), { { quint32(dirStart + i), 1 } });
        if (i == depth)
            putDirBlock(img, (dirStart + i) * kBlk, { de(2 + i, "."), de(2 + i, "..") });
        else
            putDirBlock(img, (dirStart + i) * kBlk,
                        { de(2 + i, "."), de(2 + i, ".."), de(3 + i, "d") });
    }
    return img;
}

// inline 文件（4B，无 system.data）：数据全在 i_block
static QByteArray buildInlinePlainImage()
{
    QByteArray img(7 * kBlk, 0);
    putSuper(img, 16, 2048, 256);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode256(img, 2, 0x41ED, kBlk, 0x80000);          // root 目录（块式）
    putInode256(img, 11, 0x81A4, 4, 0x10000000);         // tiny（inline）
    putExtents(img, inoOff(2, 256), { { 5, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."), de(11, "tiny.txt", 1) });
    memcpy(img.data() + inoOff(11, 256) + 40, "tiny", 4);
    return img;
}

// inline 文件（62B，带 system.data xattr）：60B 在 i_block + 2B 在 xattr 值
//   xattr 体 @160：magic(4B) + 条目(len=4,index=7,offs=88,size=2,"data") + 末条目
//   值 @164+88=252
static QByteArray buildInlineXattrImage()
{
    QByteArray img(7 * kBlk, 0);
    putSuper(img, 16, 2048, 256);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode256(img, 2, 0x41ED, kBlk, 0x80000);
    putInode256(img, 11, 0x81A4, 62, 0x10000000);
    putExtents(img, inoOff(2, 256), { { 5, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."), de(11, "mid.txt", 1) });
    // 62 字节内容：前 60 在 i_block
    static const char kContent[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"; // 62
    memcpy(img.data() + inoOff(11, 256) + 40, kContent, 60);
    // xattr 体
    const int base = inoOff(11, 256) + 160;
    put32(img, base, 0xEA020000);
    img[base + 4] = char(4);            // e_name_len
    img[base + 5] = char(7);            // e_name_index = SYSTEM
    put16(img, base + 6, 88);           // e_value_offs
    put32(img, base + 8, 0);            // e_value_inum
    put32(img, base + 12, 2);           // e_value_size
    memcpy(img.data() + base + 20, "data", 4);   // e_name（条目头 16B + len/index）
    // 末条目 = 4 零字节 @base+20；值 @base+4+88 = 252
    memcpy(img.data() + inoOff(11, 256) + 252, kContent + 60, 2);
    return img;
}

// inline 目录：/sub 目录数据在 i_block（[父 inode u32][目录项 @4..]），
// 内含 f.txt（inode 12，内容 "hi"）
static QByteArray buildInlineDirImage()
{
    QByteArray img(7 * kBlk, 0);
    putSuper(img, 16, 2048, 256);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode256(img, 2, 0x41ED, kBlk, 0x80000);
    putInode256(img, 11, 0x41ED, 20, 0x10000000);        // sub（inline 目录）
    putInode256(img, 12, 0x81A4, 2, 0x10000000);         // f.txt（inline 文件）
    putExtents(img, inoOff(2, 256), { { 5, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."), de(11, "sub") });
    // sub 的 inline 数据：父 inode(2) @0 + "f.txt" 目录项 @4
    const int off = inoOff(11, 256) + 40;
    put32(img, off, 2);
    put32(img, off + 4, 12);
    put16(img, off + 8, 16);
    img[off + 10] = char(5);
    img[off + 11] = char(1);
    memcpy(img.data() + off + 12, "f.txt", 5);
    // f.txt inline 内容
    memcpy(img.data() + inoOff(12, 256) + 40, "hi", 2);
    return img;
}

// depth-1 extent 树文件：root 索引 → 块 6（含叶 extent → 块 7）
static QByteArray buildDepth1Image()
{
    static const char kContent[] = "depth1 file content";   // 19 字节
    QByteArray img(9 * kBlk, 0);
    putSuper(img, 16, 2048, 128);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode128(img, 2, 0x41ED, kBlk, 0x80000);
    putInode128(img, 11, 0x81A4, quint32(sizeof(kContent) - 1), 0x80000);
    putExtents(img, inoOff(2), { { 5, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."), de(11, "big.txt", 1) });
    // 索引块 @6：头(depth=0, entries=1) + 叶 extent [0..0]→7
    const int idx = 6 * kBlk;
    put16(img, idx, 0xF30A);
    put16(img, idx + 2, 1);
    put16(img, idx + 4, 340);
    img[idx + 6] = char(0);
    put32(img, idx + 12, 0);       // ee_block
    put16(img, idx + 16, 1);       // ee_len
    put32(img, idx + 20, 7);       // ee_start_lo
    // root 索引条目 → 块 6
    const int root = inoOff(11) + 40;
    put16(img, root, 0xF30A);
    put16(img, root + 2, 1);
    put16(img, root + 4, 4);
    img[root + 6] = char(1);       // depth = 1
    put32(img, root + 12, 0);      // ei_block
    put32(img, root + 16, 6);      // ei_leaf_lo
    memcpy(img.data() + 7 * kBlk, kContent, sizeof(kContent) - 1);
    return img;
}

// 环状索引块：root → 块 6 → 块 7 → 块 6（环）。深度守卫必须终止递归并报错
// （旧实现 depth 递减 → 守卫恒不成立 → 无限递归栈溢出）
static QByteArray buildCycleImage()
{
    QByteArray img(9 * kBlk, 0);
    putSuper(img, 16, 2048, 128);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode128(img, 2, 0x41ED, kBlk, 0x80000);
    putInode128(img, 11, 0x81A4, 18, 0x80000);
    putExtents(img, inoOff(2), { { 5, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."), de(11, "cyc.txt", 1) });
    // root 索引条目 → 块 6
    const int root = inoOff(11) + 40;
    put16(img, root, 0xF30A);
    put16(img, root + 2, 1);
    put16(img, root + 4, 4);
    img[root + 6] = char(1);       // depth = 1
    put32(img, root + 12, 0);
    put32(img, root + 16, 6);      // ei_leaf_lo → 6
    // 块 6 索引 → 块 7
    const int b6 = 6 * kBlk;
    put16(img, b6, 0xF30A);
    put16(img, b6 + 2, 1);
    put16(img, b6 + 4, 340);
    img[b6 + 6] = char(1);         // depth = 1
    put32(img, b6 + 12, 0);
    put32(img, b6 + 16, 7);        // ei_leaf_lo → 7
    // 块 7 索引 → 块 6（环）
    const int b7 = 7 * kBlk;
    put16(img, b7, 0xF30A);
    put16(img, b7 + 2, 1);
    put16(img, b7 + 4, 340);
    img[b7 + 6] = char(1);         // depth = 1
    put32(img, b7 + 12, 0);
    put32(img, b7 + 16, 6);        // ei_leaf_lo → 6
    return img;
}

// 1K 块宽镜像（单块组 65536 块）：hello.txt 仅 1 块，块 15.. 大片连续空闲
// （>32767 块）。replace 32768 块数据 → findFreeRuns 必须把连续区拆成
// 32767+1 两段 extent（ee_len 低 15 位上限），旧实现直接取 32768 → 截断为 0x8000
// 注意：块位图在 block 3，读取跨 4125 字节（块 3..7）——inode 表等必须放在
// block 7 之后，否则其非零字节会污染位图位（把块 16384+ 误判为占用）
//   block0: boot | block1: super | block2: GDT | block3: 块位图（跨 3..7）
//   block8: inode 位图 | block9..12: inode 表（32×128B）| block13: root 目录
//   block14: hello.txt 数据
static QByteArray buildWideImage()
{
    constexpr int kWideBlk = 1024;
    constexpr int kBlocks = 33000;
    QByteArray img(kBlocks * kWideBlk, 0);
    // superblock @1024（1K 块：first_data_block=1、log_block_size=0）
    putSuper(img, 16, kBlocks, 128);
    img[1048] = char(0);            // log_block_size → 1024
    put32(img, 1044, 1);            // first_data_block（1K 块 = 1）
    put32(img, 1056, 65536);        // blocks_per_group 放大（覆盖全部块）
    // GDT @block 2（blockSize==1024 → gdtBlock=2）
    const int gdt = 2 * kWideBlk;
    put32(img, gdt + 0, 3);         // block bitmap @3
    put32(img, gdt + 4, 8);         // inode bitmap @8
    put32(img, gdt + 8, 9);         // inode table @9
    put16(img, gdt + 12, 0xFF00);   // free_blocks（组内 0x0FFF 块空闲，只取低 16 位值）
    // inode 表 @block 9（inode n 在表内偏移 (n-1)*128）
    const int tbl = 9 * kWideBlk;
    putInode128(img, 2, 0x41ED, kWideBlk, 0x80000, tbl);
    putInode128(img, 11, 0x81A4, 12, 0x80000, tbl);
    putExtents(img, tbl + 128, { { 13, 1 } });            // inode 2（根目录）
    putExtents(img, tbl + 10 * 128, { { 14, 1 } });       // inode 11（hello.txt）
    putDirBlock(img, 13 * kWideBlk,
                { de(2, "."), de(2, ".."), de(11, "hello.txt", 1) }, false, kWideBlk);
    memcpy(img.data() + 14 * kWideBlk, "hello wide!", 12);
    // 块位图 @block 3：块 0..14 占用（字节序 MSB-first 与实现一致；
    // 块 15..32999 空闲 → 连续空闲 32985 块 ≥ 32768）
    for (int b = 0; b <= 14; ++b)
        img[3 * kWideBlk + b / 8] =
            char(uchar(img[3 * kWideBlk + b / 8]) | (0x80u >> (b & 7)));
    return img;
}

// 64BIT 变体：buildFlatImage 改造（incompat|0x80、desc_size=64），组 0 描述符
// free_blocks_lo=0x0002 @+12、hi=0x0100 @+44 → 计数值 (0x0100<<16)|2。
// 增长替换净 -2 块后应为 0x01000000（旧实现 lo/hi 各减 → hi=0x00FE 双加）
static QByteArray build64BitFlatImage()
{
    QByteArray img = buildFlatImage();
    put32(img, 1120, 0x82);         // incompat: FILETYPE | 64BIT
    put16(img, 1278, 64);           // desc_size = 64
    put16(img, kBlk + 12, 0x0002);  // free_blocks_lo
    put16(img, kBlk + 44, 0x0100);  // free_blocks_hi
    return img;
}

// htree 目录：root 块 = dx 根（dot rec_len=12 + dotdot rec_len=4084 覆盖索引区）
// + 两个叶块（线性目录项）
static QByteArray buildHtreeImage()
{
    QByteArray img(9 * kBlk, 0);
    putSuper(img, 16, 2048, 128);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode128(img, 2, 0x41ED, 3 * kBlk, 0x80000 | 0x1000);   // root：EXTENTS|INDEX
    putInode128(img, 11, 0x81A4, 9, 0x80000);
    putInode128(img, 12, 0x81A4, 9, 0x80000);
    putExtents(img, inoOff(2), { { 5, 3 } });                  // 块 5=dx根 6,7=叶
    putExtents(img, inoOff(11), { { 8, 1 } });
    putExtents(img, inoOff(12), { { 8, 1 } });                 // 共享数据块（无碍）
    // dx 根 @块 5
    const int root = 5 * kBlk;
    put32(img, root, 2);          put16(img, root + 4, 12);  img[root + 6] = char(1); img[root + 7] = char(2);
    memcpy(img.data() + root + 8, ".", 1);
    put32(img, root + 12, 2);     put16(img, root + 16, kBlk - 12); img[root + 18] = char(2); img[root + 19] = char(2);
    memcpy(img.data() + root + 20, "..", 2);
    put32(img, root + 24, 0);     // reserved_zero
    img[root + 28] = char(1);     // hash_version = TEA
    img[root + 29] = char(8);     // info_length
    img[root + 30] = char(0);     // indirect_levels
    put16(img, root + 32, 508);   // limit
    put16(img, root + 34, 2);     // count
    put32(img, root + 36, 6);     // entries[0].block = 叶1
    put32(img, root + 44, 7);     // entries[1].block = 叶2
    // 叶块
    putDirBlock(img, 6 * kBlk, { de(2, "."), de(2, ".."), de(11, "aaa.txt", 1), de(12, "bbb.txt", 1) });
    putDirBlock(img, 7 * kBlk, { de(2, "."), de(2, ".."), de(11, "ccc.txt", 1) });
    memcpy(img.data() + 8 * kBlk, "leaf file", 9);
    return img;
}

// 符号链接：快速（<60B，i_block 存目标）与慢速（extent 块存目标）
static QByteArray buildSymlinkImage()
{
    QByteArray img(9 * kBlk, 0);
    putSuper(img, 16, 2048, 128);
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode128(img, 2, 0x41ED, kBlk, 0x80000);
    putInode128(img, 11, 0xA1FF, 10, 0);                 // fast：无 EXTENTS
    putInode128(img, 12, 0xA1FF, 70, 0x80000);           // slow
    putExtents(img, inoOff(2), { { 5, 1 } });
    putExtents(img, inoOff(12), { { 7, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."),
                                 de(11, "fastlink", 7), de(12, "slowlink", 7) });
    memcpy(img.data() + inoOff(11) + 40, "/hello.txt", 10);
    {
        // 前缀 "/very/long/path/to/the/real/target/file/which/exceeds/60" 为 56 字节，
        // 补齐到 70（i_size）：56 + 14 个 'x'
        const QByteArray target =
            QByteArray("/very/long/path/to/the/real/target/file/which/exceeds/60") +
            QByteArray(70 - 56, 'x');
        memcpy(img.data() + 7 * kBlk, target.constData(), 70);
    }
    return img;
}

// ===================== B11 保留用例（补新 superblock 字段） =====================

void TestExt4::detect()
{
    QByteArray s(1082, 0);
    s[1080] = char(0x53); s[1081] = char(0xEF);
    QVERIFY(imgext4::isExt4(s));
    QVERIFY(!imgext4::isExt4(QByteArray("CrAU")));
}

void TestExt4::parseSuper()
{
    QByteArray s(1400, 0);
    s[1080] = char(0x53); s[1081] = char(0xEF);
    auto put32 = [&](int off, quint32 v) { s[off] = char(v); s[off + 1] = char(v >> 8); s[off + 2] = char(v >> 16); s[off + 3] = char(v >> 24); };
    put32(1024, 32);    // inodes_count
    put32(1028, 4096);  // blocks_count_lo
    s[1048] = char(2);  // log_block_size → 4096
    put32(1112, 256);   // inode_size
    put32(1056, 32768); // blocks_per_group（B12 必填）
    put32(1064, 8);     // inodes_per_group（B12 必填）
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(s, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.inodeCount, 32ull);
    QCOMPARE(sb.inodeSize, 256u);
}

void TestExt4::parseSuper64Bit()
{
    QByteArray s(1400, 0);
    s[1080] = char(0x53); s[1081] = char(0xEF);
    auto put32 = [&](int off, quint32 v) { s[off] = char(v); s[off + 1] = char(v >> 8); s[off + 2] = char(v >> 16); s[off + 3] = char(v >> 24); };
    put32(1024, 32);
    put32(1028, 4096);
    s[1048] = char(2);
    put32(1112, 256);
    put32(1056, 32768);
    put32(1064, 8);
    put32(1120, 0x80);  // 64BIT
    put32(1360, 3);     // s_blocks_count_hi
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(s, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.blockCount, (quint64(3) << 32) | 4096ull);
    QCOMPARE(sb.descSize, 64u);   // 64BIT → desc_size ≥ 64
}

void TestExt4::parseSuperFields()
{
    QByteArray img = buildFlatImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QCOMPARE(sb.inodesPerGroup, kInodesPerGroup);
    QCOMPARE(sb.blocksPerGroup, kBlocksPerGroup);
    QCOMPARE(sb.descSize, 32u);
    QCOMPARE(sb.firstDataBlock, 0u);
    QCOMPARE(sb.featureIncompat, 0x2u);
}

void TestExt4::invalidInput()
{
    auto put32 = [&](QByteArray &d, int off, quint32 v) { d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24); };
    auto makeValid = [&](int size) {
        QByteArray d(size, 0);
        d[1080] = char(0x53); d[1081] = char(0xEF);
        put32(d, 1024, 32); put32(d, 1028, 4096);
        d[1048] = char(2); put32(d, 1112, 256);
        put32(d, 1056, 32768); put32(d, 1064, 8);
        return d;
    };
    imgext4::SuperBlock sb;

    QByteArray badMagic = makeValid(1400);
    badMagic[1080] = char(0x54);
    QVERIFY(!imgext4::isExt4(badMagic));
    QVERIFY(!imgext4::parseSuper(badMagic, sb));
    QVERIFY(!imgext4::parseSuper(QByteArray(1082, 0), sb));
    QByteArray logBig = makeValid(1400);
    logBig[1048] = char(7);
    QVERIFY(!imgext4::parseSuper(logBig, sb));
    QByteArray inoSmall = makeValid(1400);
    put32(inoSmall, 1112, 64);
    QVERIFY(!imgext4::parseSuper(inoSmall, sb));
    QByteArray inoBig = makeValid(1400);
    put32(inoBig, 1112, 8192);
    QVERIFY(!imgext4::parseSuper(inoBig, sb));
    // inodes_per_group == 0 → false（B12 校验）
    QByteArray zeroPerGroup = makeValid(1400);
    put32(zeroPerGroup, 1064, 0);
    QVERIFY(!imgext4::parseSuper(zeroPerGroup, sb));
    // 截断输入 [1082, 1363]：magic 有效但字段读取终点不足 → false，不得崩溃
    QByteArray trunc1363(1363, 0);
    trunc1363[1080] = char(0x53); trunc1363[1081] = char(0xEF);
    put32(trunc1363, 1024, 32); put32(trunc1363, 1028, 4096);
    trunc1363[1048] = char(2);
    QVERIFY(imgext4::isExt4(trunc1363));
    QVERIFY(!imgext4::parseSuper(trunc1363, sb));
    QByteArray trunc1113(1113, 0);
    trunc1113[1080] = char(0x53); trunc1113[1081] = char(0xEF);
    QVERIFY(!imgext4::parseSuper(trunc1113, sb));
}

// ===================== B12 手工镜像用例 =====================

void TestExt4::listTreeHandBuilt()
{
    QByteArray img = buildFlatImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgext4::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 3);
    const imgfs::FsEntry *hello = nullptr, *sub = nullptr, *inner = nullptr;
    for (const imgfs::FsEntry &e : out) {
        if (e.path == QLatin1String("hello.txt")) hello = &e;
        else if (e.path == QLatin1String("sub")) sub = &e;
        else if (e.path == QLatin1String("sub/inner.txt")) inner = &e;
    }
    QVERIFY(hello && sub && inner);
    QVERIFY(!hello->isDir);
    QCOMPARE(hello->size, 12ull);
    QVERIFY(sub->isDir);
    QCOMPARE(sub->size, 4096ull);
    QVERIFY(!inner->isDir);
    QCOMPARE(inner->size, 15ull);
}

void TestExt4::extractHandBuilt()
{
    QByteArray img = buildFlatImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QByteArray data;
    QString err;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("Hello, ext4!"));
    QVERIFY2(imgext4::extractFile(img, sb, "sub/inner.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("inner file data"));
    // 目录不能提取
    QVERIFY(!imgext4::extractFile(img, sb, "sub", data, &err));
    QVERIFY(!err.isEmpty());
}

void TestExt4::symlinkHandBuilt()
{
    QByteArray img = buildSymlinkImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgext4::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "fastlink", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("/hello.txt"));
    QVERIFY2(imgext4::extractFile(img, sb, "slowlink", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("/very/long/path/to/the/real/target/file/which/exceeds/60") +
                   QByteArray(14, 'x'));
}

void TestExt4::inlineDataHandBuilt()
{
    QString err;
    QByteArray data;

    // 纯 inline（≤60B，无 system.data）
    QByteArray img1 = buildInlinePlainImage();
    imgext4::SuperBlock sb1;
    QVERIFY(imgext4::parseSuper(img1, sb1));
    QVERIFY2(imgext4::extractFile(img1, sb1, "tiny.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("tiny"));

    // inline + system.data（62B）
    QByteArray img2 = buildInlineXattrImage();
    imgext4::SuperBlock sb2;
    QVERIFY(imgext4::parseSuper(img2, sb2));
    QVERIFY2(imgext4::extractFile(img2, sb2, "mid.txt", data, &err), qPrintable(err));
    QCOMPARE(data.size(), 62);
    // 前 60B 为 kContent[0..59]（60 字符，末位 'x'）
    QCOMPARE(data.left(60), QByteArray("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwx"));
    QCOMPARE(data.mid(60), QByteArray("yz"));

    // inline 目录
    QByteArray img3 = buildInlineDirImage();
    imgext4::SuperBlock sb3;
    QVERIFY(imgext4::parseSuper(img3, sb3));
    QList<imgfs::FsEntry> out;
    QVERIFY2(imgext4::listTree(img3, sb3, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);   // sub + sub/f.txt
    QVERIFY(out[0].isDir && out[0].path == QLatin1String("sub"));
    QVERIFY(!out[1].isDir && out[1].path == QLatin1String("sub/f.txt"));
    QVERIFY2(imgext4::extractFile(img3, sb3, "sub/f.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("hi"));
}

void TestExt4::extentDepth1()
{
    QByteArray img = buildDepth1Image();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QByteArray data;
    QString err;
    QVERIFY2(imgext4::extractFile(img, sb, "big.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("depth1 file content"));
}

void TestExt4::htreeHandBuilt()
{
    QByteArray img = buildHtreeImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    // dx 根块（dot/dotdot 覆盖索引区）线性跳过；两个叶块的条目全部列出
    QVERIFY2(imgext4::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 3);
    QSet<QString> paths;
    for (const imgfs::FsEntry &e : out)
        paths.insert(e.path);
    QVERIFY(paths.contains("aaa.txt"));
    QVERIFY(paths.contains("bbb.txt"));
    QVERIFY(paths.contains("ccc.txt"));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "ccc.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("leaf file"));
}

void TestExt4::oldFormatDir()
{
    // 无 FILETYPE 特性(0x2)的镜像：目录项为 ext2_dir_entry 老格式（16 位 name_len）
    QByteArray img(7 * kBlk, 0);
    putSuper(img, 16, 2048, 128, 0);             // incompat = 0
    putDesc(img, 0, 2, 3, kTableBlock);
    putInode128(img, 2, 0x41ED, kBlk, 0x80000);
    putInode128(img, 11, 0x81A4, 4, 0x80000);   // "old!" 恰 4 字节
    putExtents(img, inoOff(2), { { 5, 1 } });
    putExtents(img, inoOff(11), { { 6, 1 } });
    putDirBlock(img, 5 * kBlk, { de(2, "."), de(2, ".."), de(11, "old.txt", 1) }, true);
    memcpy(img.data() + 6 * kBlk, "old!", 4);
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgext4::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].path, QString("old.txt"));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "old.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("old!"));
}

void TestExt4::depthLimit()
{
    imgext4::SuperBlock sb;
    QString err;
    QList<imgfs::FsEntry> out;
    QByteArray deep = buildDeepChainImage(140);
    QVERIFY(imgext4::parseSuper(deep, sb));
    QVERIFY(!imgext4::listTree(deep, sb, out, &err));
    QVERIFY(err.contains("目录深度超限"));
    QByteArray ok = buildDeepChainImage(100);
    QVERIFY(imgext4::parseSuper(ok, sb));
    err.clear();
    QVERIFY2(imgext4::listTree(ok, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 100);
}

void TestExt4::corruptedInputs()
{
    imgext4::SuperBlock sb;
    QString err;
    QByteArray data;
    QList<imgfs::FsEntry> out;

    QByteArray img = buildFlatImage();
    QVERIFY(imgext4::parseSuper(img, sb));

    // 截断：数据区不完整
    QByteArray trunc = img.left(7 * kBlk + 5);
    err.clear();
    QVERIFY(!imgext4::extractFile(trunc, sb, "hello.txt", data, &err));
    QVERIFY(!err.isEmpty());
    // 截断：inode 表不完整
    QByteArray trunc2 = img.left(4 * kBlk + 40);
    err.clear();
    QVERIFY(!imgext4::listTree(trunc2, sb, out, &err));
    QVERIFY(!err.isEmpty());
    // 空输入
    QVERIFY(!imgext4::listTree(QByteArray(), sb, out, &err));
    // 坏 extent 魔数
    QByteArray badExt = buildFlatImage();
    put16(badExt, inoOff(11) + 40, 0x1234);
    err.clear();
    QVERIFY(!imgext4::extractFile(badExt, sb, "hello.txt", data, &err));
    QVERIFY(err.contains("魔数"));
    // 坏目录 rec_len（root 目录块 @5）
    QByteArray badDir = buildFlatImage();
    put16(badDir, 5 * kBlk + 4, 0);   // "." 的 rec_len = 0
    err.clear();
    QVERIFY(!imgext4::listTree(badDir, sb, out, &err));
    QVERIFY(err.contains("rec_len"));
    // 不存在路径 / 空路径 / '.' 组件
    QVERIFY(!imgext4::extractFile(img, sb, "nope.txt", data, &err));
    QVERIFY(!imgext4::extractFile(img, sb, QString(), data, &err));
    QVERIFY(!imgext4::extractFile(img, sb, "./hello.txt", data, &err));
    // 无效 SuperBlock
    imgext4::SuperBlock badSb = sb;
    badSb.blockSize = 0;
    err.clear();
    QVERIFY(!imgext4::listTree(img, badSb, out, &err));
    QVERIFY(!err.isEmpty());
    // META_BG 特性拒绝
    imgext4::SuperBlock metaSb = sb;
    metaSb.featureIncompat |= 0x10;
    QVERIFY(!imgext4::listTree(img, metaSb, out, &err));
    QVERIFY(err.contains("META_BG"));
}

void TestExt4::replaceHandBuilt()
{
    QString err;
    QByteArray img = buildFlatImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));

    // 变小（13B → 6B，原地写 + i_size 更新）
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", QByteArray("short!"), &err),
             qPrintable(err));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("short!"));

    // 变大（分配新块：10000B → 3 块；旧块 7 释放）
    const QByteArray big(10000, 'x');
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", big, &err), qPrintable(err));
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, big);
    // 位图：旧块 7 已释放；新分配从首个空闲位（块 9）取 3 块
    // （位序 MSB-first：块 n ↔ 0x80>>(n&7)；块 7 在字节 0 的最低位）
    const uchar *bm = reinterpret_cast<const uchar *>(img.constData()) + 2 * kBlk;
    QVERIFY(!(bm[0] & 0x01));          // bit 7（块 7）已清
    QVERIFY((bm[1] & 0x80));           // bit 8（块 8，inner.txt）仍占用
    QVERIFY((bm[1] & 0x40));           // bit 9（块 9）新分配
    QVERIFY((bm[1] & 0x20));           // bit 10（块 10）新分配
    QVERIFY((bm[1] & 0x10));           // bit 11（块 11）新分配
    QVERIFY(!(bm[1] & 0x08));          // bit 12（块 12）仍空闲
    // 空闲计数已更新（superblock @1036 与组描述符 @12）
    QVERIFY(imgext4::extractFile(img, sb, "sub/inner.txt", data, &err));
    QCOMPARE(data, QByteArray("inner file data"));

    // repack 返回完整镜像且可重新解析
    QByteArray packed = imgext4::repack(img, sb);
    QVERIFY(!packed.isEmpty());
    QVERIFY(imgext4::isExt4(packed));
    imgext4::SuperBlock sb2;
    QVERIFY(imgext4::parseSuper(packed, sb2));
    QVERIFY2(imgext4::extractFile(packed, sb2, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, big);
}

void TestExt4::replaceInvalid()
{
    QString err;
    QByteArray img = buildFlatImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    // 不存在的路径
    QVERIFY(!imgext4::replaceFile(img, sb, "nope.txt", QByteArray("x"), &err));
    QVERIFY(!err.isEmpty());
    // 目录不能替换
    QVERIFY(!imgext4::replaceFile(img, sb, "sub", QByteArray("x"), &err));
    QVERIFY(err.contains("目录"));
    // 符号链接不能替换
    QByteArray simg = buildSymlinkImage();
    QVERIFY(imgext4::parseSuper(simg, sb));
    QVERIFY(!imgext4::replaceFile(simg, sb, "fastlink", QByteArray("x"), &err));
    QVERIFY(err.contains("符号链接"));
    // 空数据也允许（替换为空文件）
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", QByteArray(), &err), qPrintable(err));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QVERIFY(data.isEmpty());
}

void TestExt4::extentCycle()
{
    QByteArray img = buildCycleImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QByteArray data;
    QString err;
    // 环状索引块：必须报深度错误而非无限递归（旧实现 depth 递减 → 栈溢出崩溃）
    QVERIFY(!imgext4::extractFile(img, sb, "cyc.txt", data, &err));
    QVERIFY(err.contains("深度超限"));
}

void TestExt4::replaceHugeRun()
{
    // 1K 块宽镜像：hello.txt 1 块 → 替换 32768 块数据（>32767 上限）。
    // findFreeRuns 必须拆成 32767+1 两段（旧实现单段 32768 → ee_len 截断 0x8000
    // → 读回 0 → 自校验失败、镜像已被破坏）
    QByteArray img = buildWideImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QCOMPARE(sb.blockSize, 1024u);
    const QByteArray big(32768 * 1024, 'W');
    QString err;
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", big, &err), qPrintable(err));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, big);
    // extent 头：2 段，ee_len 不截断（32767 + 1）
    const int root = 9 * 1024 + 10 * 128 + 40;   // inode 11 i_block（1K 块，表 @block9）
    QCOMPARE(rd16(img, root + 2), 2u);           // entries
    QCOMPARE(rd16(img, root + 12 + 4), 32767u);  // 第一段 ee_len
    QCOMPARE(rd32(img, root + 12 + 8), 15u);     // 第一段 ee_start_lo
    QCOMPARE(rd16(img, root + 24 + 4), 1u);      // 第二段 ee_len
    QCOMPARE(rd32(img, root + 24 + 8), 32782u);  // 第二段 ee_start_lo
}

void TestExt4::freeCounts64Bit()
{
    QByteArray img = build64BitFlatImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QCOMPARE(sb.descSize, 64u);
    // 增长替换：释放块 7（+1）、占用 9/10/11（-3）→ 净 -2
    const QByteArray big(10000, 'z');
    QString err;
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", big, &err), qPrintable(err));
    // 组 0 free_blocks = (0x0100<<16)|0x0002 - 2 = 0x01000000 → lo=0x0000, hi=0x0100
    // （旧实现 lo/hi 各减 2 → hi=0x00FE，双加）
    QCOMPARE(rd16(img, kBlk + 12), 0x0000u);
    QCOMPARE(rd16(img, kBlk + 44), 0x0100u);
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, big);
}

void TestExt4::replaceSparseFile()
{
    // 含空洞文件（lb0→7、lb2→9，逻辑块 1 空洞）+ 容量内替换：
    // 旧实现原地顺序写两 extent → 内核视角内容错误却自校验通过（自校验按 extent
    // 拼接不建模空洞）→ 静默错误成功。修复后走增长路径重建稠密树。
    QByteArray img = buildSparseFileImage();
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QString err;
    // 2 块数据（跨空洞）：旧实现原地把后半写进 lb2→块9，内核视角 lb1 空洞读零、
    // 内容错乱却自校验通过。修复后走增长路径重建稠密树。
    const QByteArray small(2 * kBlk, 'a');   // ≤ 容量 8192 → 容量内，但稀疏必须走增长路径
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", small, &err), qPrintable(err));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, small);
    // 稠密树：单段 extent，从逻辑块 0 起（原镜像空闲首块 = 块 10，因块 0..9 均占用）
    const int off = inoOff(11) + 40;
    QCOMPARE(rd16(img, off + 2), 1u);            // entries == 1
    QCOMPARE(rd32(img, off + 12), 0u);           // ee_block == 0
    QCOMPARE(rd16(img, off + 16), 2u);           // ee_len == 2（连续覆盖）
    QCOMPARE(rd32(img, off + 20), 10u);          // ee_start_lo == 10
    // 位图：旧块 7、9 已释放；块 10/11 新占用
    const uchar *bm = reinterpret_cast<const uchar *>(img.constData()) + 2 * kBlk;
    QVERIFY(!(bm[0] & 0x01));          // bit 7（旧 extent1）已清
    QVERIFY(!(bm[1] & 0x40));          // bit 9（旧 extent2）已清
    QVERIFY((bm[1] & 0x20));           // bit 10（新分配）占用
    QVERIFY((bm[1] & 0x10));           // bit 11（新分配）占用
    // 其余文件不受影响
    QVERIFY2(imgext4::extractFile(img, sb, "sub/inner.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("inner file data"));
    // 再替换一次（现在已是稠密树 → 原地路径），回读一致
    const QByteArray dense(2048, 'b');
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", dense, &err), qPrintable(err));
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, dense);
}

// ===================== 真实 mke2fs 镜像用例 =====================

// 运行 mke2fs -d srcDir 构造真实镜像（找不到 mke2fs 时返回 false）
static bool buildRealImage(QTemporaryDir &dir, const QString &srcName,
                           const QStringList &extraArgs, QByteArray &imageOut,
                           QString &errMsg)
{
    const QString mke2fs = QStandardPaths::findExecutable(QStringLiteral("mke2fs"));
    if (mke2fs.isEmpty())
        return false;
    const QString srcDir = dir.path() + QLatin1Char('/') + srcName;
    const QString imagePath = dir.path() + QStringLiteral("/fs.img");
    QProcess p;
    p.setWorkingDirectory(dir.path());
    QStringList args;
    args << QStringLiteral("-q") << QStringLiteral("-t") << QStringLiteral("ext4")
         << QStringLiteral("-b") << QStringLiteral("4096") << extraArgs
         << QStringLiteral("-d") << srcName << imagePath << QStringLiteral("8M");
    p.start(mke2fs, args);
    if (!p.waitForFinished(30000)) {
        errMsg = QStringLiteral("mke2fs 超时");
        return false;
    }
    if (p.exitCode() != 0) {
        errMsg = QString::fromLocal8Bit(p.readAllStandardError());
        return false;
    }
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        errMsg = QStringLiteral("无法读取镜像");
        return false;
    }
    imageOut = f.readAll();
    return true;
}

void TestExt4::realListExtract()
{
    QString errMsg;
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    QDir().mkpath(dir.path() + QLatin1String("/src/sub"));
    {
        QFile f(dir.path() + QLatin1String("/src/hello.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello ext4 world\n");
    }
    {
        QFile f(dir.path() + QLatin1String("/src/sub/inner.bin"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("deeper content 12345");
    }
    QFile::link(QLatin1String("/hello.txt"), dir.path() + QLatin1String("/src/link_to_hello"));
    QByteArray img;
    if (!buildRealImage(dir, QStringLiteral("src"), {}, img, errMsg))
        QSKIP(qPrintable(QStringLiteral("mke2fs 不可用: ") + errMsg));

    imgext4::SuperBlock sb;
    QVERIFY2(imgext4::parseSuper(img, sb), qPrintable(errMsg));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgext4::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 5);   // hello.txt + link + sub + sub/inner.bin + lost+found
    const imgfs::FsEntry *hello = nullptr, *link = nullptr, *sub = nullptr, *inner = nullptr;
    for (const imgfs::FsEntry &e : out) {
        if (e.path == QLatin1String("hello.txt")) hello = &e;
        else if (e.path == QLatin1String("link_to_hello")) link = &e;
        else if (e.path == QLatin1String("sub")) sub = &e;
        else if (e.path == QLatin1String("sub/inner.bin")) inner = &e;
    }
    QVERIFY(hello && link && sub && inner);
    QCOMPARE(hello->size, 17ull);
    QCOMPARE(link->size, 10ull);
    QVERIFY(sub->isDir);
    QCOMPARE(inner->size, 20ull);   // "deeper content 12345" = 20 字节

    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("hello ext4 world\n"));
    QVERIFY2(imgext4::extractFile(img, sb, "sub/inner.bin", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("deeper content 12345"));
    QVERIFY2(imgext4::extractFile(img, sb, "link_to_hello", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("/hello.txt"));
}

void TestExt4::realReplaceRoundTrip()
{
    QString errMsg;
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    QDir().mkpath(dir.path() + QLatin1String("/src"));
    {
        QFile f(dir.path() + QLatin1String("/src/hello.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello ext4 world\n");
    }
    QByteArray img;
    if (!buildRealImage(dir, QStringLiteral("src"), {}, img, errMsg))
        QSKIP(qPrintable(QStringLiteral("mke2fs 不可用: ") + errMsg));
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));

    // 等长替换
    QString err;
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt",
                                  QByteArray("REPLACED CONTENT!!"), &err), qPrintable(err));
    // 变长（需分配新块：50KB）
    const QByteArray big(50 * 1024, 'z');
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", big, &err), qPrintable(err));
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, big);
    // 变小（原地写）
    QVERIFY2(imgext4::replaceFile(img, sb, "hello.txt", QByteArray("hi"), &err), qPrintable(err));
    QVERIFY2(imgext4::extractFile(img, sb, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("hi"));
    // repack 后仍可解析/提取
    QByteArray packed = imgext4::repack(img, sb);
    QVERIFY(!packed.isEmpty());
    imgext4::SuperBlock sb2;
    QVERIFY(imgext4::parseSuper(packed, sb2));
    QVERIFY2(imgext4::extractFile(packed, sb2, "hello.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("hi"));
    // 替换后 listTree 正常
    QList<imgfs::FsEntry> out;
    QVERIFY2(imgext4::listTree(packed, sb2, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);   // lost+found + hello.txt
}

void TestExt4::realInlineData()
{
    QString errMsg;
    QTemporaryDir dir;
    if (!dir.isValid())
        QSKIP("无法创建临时目录");
    QDir().mkpath(dir.path() + QLatin1String("/src"));
    {
        QFile f(dir.path() + QLatin1String("/src/tiny.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("tiny");
    }
    {
        QFile f(dir.path() + QLatin1String("/src/mid.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
    }
    QByteArray img;
    if (!buildRealImage(dir, QStringLiteral("src"),
                        QStringList() << QStringLiteral("-O") << QStringLiteral("inline_data"),
                        img, errMsg))
        QSKIP(qPrintable(QStringLiteral("mke2fs 不可用: ") + errMsg));
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(img, sb));
    QList<imgfs::FsEntry> out;
    QString err;
    QVERIFY2(imgext4::listTree(img, sb, out, &err), qPrintable(err));
    QCOMPARE(out.size(), 3);   // tiny + mid + lost+found
    QByteArray data;
    QVERIFY2(imgext4::extractFile(img, sb, "tiny.txt", data, &err), qPrintable(err));
    QCOMPARE(data, QByteArray("tiny"));
    QVERIFY2(imgext4::extractFile(img, sb, "mid.txt", data, &err), qPrintable(err));
    QCOMPARE(data.size(), 62);
    QCOMPARE(data, QByteArray("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"));
}

QTEST_APPLESS_MAIN(TestExt4)
#include "test_fs_ext4.moc"

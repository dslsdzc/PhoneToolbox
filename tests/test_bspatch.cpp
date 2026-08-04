#include <QtTest>
#include "image_engine/bspatch_image.h"
#include "image_engine/compression/bzip2_wrapper.h"

class TestBspatch : public QObject
{
    Q_OBJECT
private slots:
    void applySimple();
    void applyExtraAndOffset();
};

// 小端 64 位写入
static void put64(QByteArray &d, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        d.append(char((v >> (i * 8)) & 0xFF));
}

// 构造 bsdiff patch: 头(32B: BSDIFF40 + ctrl_len + diff_len + new_len) + bzip2(ctrl) + bzip2(diff) + bzip2(extra)。
// 注意第 3 个 64 位字段是 new_len（新数据总长），不是 extra 长度（bspatch.c 语义；
// extra 区长度由 patch 总长 - 32 - ctrl_len - diff_len 隐式得出）。
static QByteArray buildPatch(const QByteArray &ctrl, const QByteArray &diff,
                             const QByteArray &extra, quint64 newLen)
{
    QByteArray ctrlBz = imgcomp::bzip2Compress(ctrl);
    QByteArray diffBz = imgcomp::bzip2Compress(diff);
    QByteArray extraBz = imgcomp::bzip2Compress(extra);
    QByteArray patch;
    patch.append("BSDIFF40");
    put64(patch, static_cast<quint64>(ctrlBz.size()));
    put64(patch, static_cast<quint64>(diffBz.size()));
    put64(patch, newLen);
    patch.append(ctrlBz).append(diffBz).append(extraBz);
    return patch;
}

// 旧 "aaaa"，新 "aaab"（diff=3 字节 "aa"+1 字节 (b-a+256)%256, extra 0）
void TestBspatch::applySimple()
{
    QByteArray oldData("aaaa");
    // diff 块: 'a' ^ 'a'=0, 'a' ^ 'a'=0, 'a' ^ 'a'=0, 'b' ^ 'a'=1
    // ctrl: (4, 0, 0)
    QByteArray ctrl;
    put64(ctrl, 4); put64(ctrl, 0); put64(ctrl, 0);
    QByteArray diff(4, 0); diff[3] = 1;
    QByteArray patch = buildPatch(ctrl, diff, QByteArray(), 4); // newLen=4
    QCOMPARE(imgbspatch::applyBsdiff(oldData, patch), QByteArray("aaab"));
}

// extra 段 + old 位置偏移: 旧 "abcdef" → 新 "abcXYg"
// ctrl1 (3,2,2): diff 3 字节(0,0,0) → "abc"; extra "XY"; oldPos 0→3+2=5
// ctrl2 (1,0,0): diff 1 字节(1) → old[5]='f'+1='g'
void TestBspatch::applyExtraAndOffset()
{
    QByteArray oldData("abcdef");
    QByteArray ctrl;
    put64(ctrl, 3); put64(ctrl, 2); put64(ctrl, 2);
    put64(ctrl, 1); put64(ctrl, 0); put64(ctrl, 0);
    QByteArray diff(4, 0); diff[3] = 1;
    QByteArray extra("XY");
    QByteArray patch = buildPatch(ctrl, diff, extra, 6); // newLen=6
    QCOMPARE(imgbspatch::applyBsdiff(oldData, patch), QByteArray("abcXYg"));
}

QTEST_APPLESS_MAIN(TestBspatch)
#include "test_bspatch.moc"

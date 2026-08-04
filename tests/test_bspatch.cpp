#include <QtTest>
#include <limits>
#include "image_engine/bspatch_image.h"
#include "image_engine/compression/bzip2_wrapper.h"

class TestBspatch : public QObject
{
    Q_OBJECT
private slots:
    void applySimple();
    void applyExtraAndOffset();
    void negativeOldOffsetAccepted();
    void corruptRejected();
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

// 负 old_offset 正例（bspatch.c 0 填充语义）: 旧 "abcdef" → 新 "abcabc"
// ctrl1 (3,0,-3): diff 3 字节(0,0,0) → "abc"；oldPos 0→3，再 += z(-3) → 0
// ctrl2 (3,0,0): diff 3 字节(0,0,0) → 从 old[0..2] 再次取 "abc"
// 回归保护: 旧实现若对 oldPos 做符号检查会把负 old_offset 判为损坏 patch 而拒绝；
// bspatch.c 允许负偏移，越界读由 diff 循环 0 填充守卫兜底（此处 oldPos 恢复为 0，结果 "abcabc"）。
void TestBspatch::negativeOldOffsetAccepted()
{
    QByteArray oldData("abcdef");
    QByteArray ctrl;
    put64(ctrl, 3); put64(ctrl, 0); put64(ctrl, static_cast<quint64>(qint64(-3)));
    put64(ctrl, 3); put64(ctrl, 0); put64(ctrl, 0);
    QByteArray diff(6, 0);
    QByteArray patch = buildPatch(ctrl, diff, QByteArray(), 6); // newLen=6
    QCOMPARE(imgbspatch::applyBsdiff(oldData, patch), QByteArray("abcabc"));
}

// 负向用例: 损坏 patch 必须返回空且 ok=false（不得回绕/越界/负尺寸分配）
void TestBspatch::corruptRejected()
{
    // bad magic
    bool ok = true;
    QVERIFY(imgbspatch::applyBsdiff(QByteArray("aaaa"), QByteArray("XXXX"), &ok).isEmpty());
    QVERIFY(!ok);
    // 头长度越界: ctrl_len 声称超过 patch 总长
    ok = true;
    QByteArray p1;
    p1.append("BSDIFF40");
    put64(p1, 1000); put64(p1, 0); put64(p1, 4); // ctrl_len=1000 > 实际剩余
    QVERIFY(imgbspatch::applyBsdiff(QByteArray("aaaa"), p1, &ok).isEmpty());
    QVERIFY(!ok);
    // newLen 超过 INT_MAX → 拒绝
    ok = true;
    QByteArray p2;
    p2.append("BSDIFF40");
    put64(p2, 0); put64(p2, 0); put64(p2, 1ULL << 40);
    QVERIFY(imgbspatch::applyBsdiff(QByteArray("aaaa"), p2, &ok).isEmpty());
    QVERIFY(!ok);
    // 回绕回归（Critical）: ctrl=(INT64_MAX, INT64_MAX, 0) 必须被值域检查拒绝；
    // 旧实现 newPos+diffLen+extraLen 回绕通过 → 负尺寸 QByteArray + 堆越界写
    ok = true;
    QByteArray ctrl;
    put64(ctrl, static_cast<quint64>(std::numeric_limits<qint64>::max()));
    put64(ctrl, static_cast<quint64>(std::numeric_limits<qint64>::max()));
    put64(ctrl, 0);
    QByteArray patch = buildPatch(ctrl, QByteArray(), QByteArray(), 4);
    QVERIFY(imgbspatch::applyBsdiff(QByteArray("aaaa"), patch, &ok).isEmpty());
    QVERIFY(!ok);
    // 成功路径 ok=true；newLen==0 合法空结果
    ok = false;
    QByteArray ctrl2;
    put64(ctrl2, 0); put64(ctrl2, 0); put64(ctrl2, 0);
    QByteArray patch2 = buildPatch(ctrl2, QByteArray(), QByteArray(), 0);
    QVERIFY(imgbspatch::applyBsdiff(QByteArray("aaaa"), patch2, &ok).isEmpty());
    QVERIFY(ok);
}

QTEST_APPLESS_MAIN(TestBspatch)
#include "test_bspatch.moc"

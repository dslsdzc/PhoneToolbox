// tests/test_eub_samsung_mode.cpp
//
// 三态认领纯函数（eub::samsungModeFor）的**离线可测面**。本机无任何 Exynos 设备（facts §F1）：
// 用例只覆盖纯函数，检测层接入（device_detector.cpp 的 0x04E8 分支）与真机路径**未验证**。
#include <QtTest>
#include "core/eub/samsung_mode.h"
#include "core/eub/eub_libusb_transport.h"        // 加固断言直接核两个判据（见 slot 1）
#include "core/odin/odin_libusb_transport.h"

class TestEubSamsungMode : public QObject
{
    Q_OBJECT

private slots:
    // facts §E2：PID 0x1234 不在 Odin 兜底表里，但若接口是 0x0A 类 + 批量 in/out，
    // isOdinDevice 也会命中 —— 顺序写反就会把 EUB 设备当成 Download 模式设备。
    void eubWinsOverOdinWhenBothPredicatesMatch()
    {
        // **前提写成断言**（加固，见报告 §加固）：本 slot 是"顺序"的回归钉，而"有顺序可言"
        // 要求两个判据对**同一组入参各自都命中**。前提若不写成断言，日后 Odin 判据一放宽
        // （例如不再要求 0x0A 类），本 slot 就退化成恒真、顺序写反也照样绿 —— 断言把这条
        // 前提钉在案上，前提没了就红给维护者看，而不是静默失效。
        QVERIFY(eub::LibusbEubTransport::isEubDevice(0x04E8, 0x1234));
        QVERIFY(odin::LibusbOdinTransport::isOdinDevice(0x04E8, 0x1234, {0x0A}, true));

        QCOMPARE(eub::samsungModeFor(0x04E8, 0x1234, {0x0A}, true), eub::SamsungMode::Eub);
    }

    void eubWinsWithoutAnyDescriptorHints()
    {
        // 本 case 是**无歧义**的 EUB 认领（Odin 判据在此为假），它对认领顺序**不敏感** ——
        // 顺序的回归钉在上一条。同样把前提钉住：Odin 判据一旦放宽到"认 VID 不认描述符"，
        // 本 slot 的含义就变了，届时前提断言会先红。
        QVERIFY(!odin::LibusbOdinTransport::isOdinDevice(0x04E8, 0x1234, {}, false));

        QCOMPARE(eub::samsungModeFor(0x04E8, 0x1234, {}, false), eub::SamsungMode::Eub);
    }

    void odinUnchangedForCdcDevices()
    {
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6860, {0x0A, 0x06}, true), eub::SamsungMode::Odin);
    }

    void odinUnchangedForLegacyPids()
    {
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6601, {}, false), eub::SamsungMode::Odin);
    }

    void nonSamsungIsNotClaimed()
    {
        QCOMPARE(eub::samsungModeFor(0x18D1, 0x4EE0, {0x0A}, true), eub::SamsungMode::NotSamsung);
        QCOMPARE(eub::samsungModeFor(0x0E8D, 0x0003, {}, false), eub::SamsungMode::NotSamsung);
    }

    void samsungVidWithoutOdinShapeIsNotClaimed()
    {
        // 正常开机/充电的三星手机：VID 0x04E8 但接口是 MTP(0x06)/ADB(0xFF)，无 CDC_DATA
        // （odin_libusb_transport.h:19-20 的同款理由）
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6860, {0x06, 0xFF}, true),
                 eub::SamsungMode::NotSamsung);
    }

    // **加固新增**（离线实验发现上面 6 条不足以钉住"委托共享判据"这件事，见报告 §加固）：
    // 把 Odin 路径**在本函数里手写一份更宽松的判据**（实验一：兜底 PID 不看 VID；实验二：
    // 只看类不看批量 in/out），上面 6 条照样全绿。下面两条把共享判据的边界钉住 ——
    // samsung_mode.h 头注释的抽取理由就是"一次判断只有一处"，检测层不该有第二份口径。
    void odinVerdictKeepsSharedPredicateBoundaries()
    {
        // ① 兜底 PID 表是**三星 VID 之内**的兜底：非三星带老 PID 也不算 Odin
        //    （odin_libusb_transport.cpp:101-102 "非三星一律不认：高通 EDL（0x05C6）/Google（0x18D1）"）
        QCOMPARE(eub::samsungModeFor(0x18D1, 0x6601, {}, false), eub::SamsungMode::NotSamsung);
        // ② 类判据是"0x0A(CDC_DATA) 类 **且** 批量 in/out"（odin_libusb_transport.h:22）：
        //    只有类、没有批量端点对的不算（Thor Linux.cs:120-134 要求同接口内成对）
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6860, {0x0A}, false), eub::SamsungMode::NotSamsung);
    }
};

QTEST_APPLESS_MAIN(TestEubSamsungMode)
#include "test_eub_samsung_mode.moc"

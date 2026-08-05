#include <QtTest>
#include <QStringList>
#include "core/engineer_mode.h"

// 任务 E1：工程模式入口映射模块测试
// 覆盖：品牌归一、已知品牌入口非空、未知品牌返回空 Entry、
//       入口字段合法性（拨号码以 *# 开头或 Activity 为 包名/ 形式）、芯片回退。
class TestEngMode : public QObject
{
    Q_OBJECT

private slots:
    void detectBrand_normalizesKnownAliases();
    void lookup_knownBrands_nonEmpty();
    void lookup_unknownBrand_empty();
    void lookup_chipFallback_mtk();
    void lookup_chipFallback_qualcomm();
    void lookup_blankInputs_noCrash();
    void isValid_rejectsEmptyAcceptsEntry();
};

void TestEngMode::detectBrand_normalizesKnownAliases()
{
    // 大小写归一
    QCOMPARE(engmode::detectBrand(QStringLiteral("Xiaomi")), QStringLiteral("Xiaomi"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("xiaomi")), QStringLiteral("Xiaomi"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("samsung")), QStringLiteral("Samsung"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("HUAWEI")), QStringLiteral("Huawei"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("honor")), QStringLiteral("Honor"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("oppo")), QStringLiteral("OPPO"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("oneplus")), QStringLiteral("OnePlus"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("VIVO")), QStringLiteral("vivo"));
    // 子品牌/别名归一到母品牌
    QCOMPARE(engmode::detectBrand(QStringLiteral("Redmi")), QStringLiteral("Xiaomi"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("POCO")), QStringLiteral("Xiaomi"));
    QCOMPARE(engmode::detectBrand(QStringLiteral("iQOO")), QStringLiteral("vivo"));
    // 空输入 → 空串（全局契约：失败返回合理默认，不崩溃）
    QCOMPARE(engmode::detectBrand(QString()), QString());
    QCOMPARE(engmode::detectBrand(QStringLiteral("  ")), QString());
}

void TestEngMode::lookup_knownBrands_nonEmpty()
{
    // 已验证入口的品牌必须返回非空合法入口
    const QStringList brands = {
        QStringLiteral("Xiaomi"),  QStringLiteral("Samsung"),
        QStringLiteral("Huawei"),  QStringLiteral("Honor"),
        QStringLiteral("OPPO"),    QStringLiteral("OnePlus"),
        QStringLiteral("realme"),  QStringLiteral("vivo"),
        QStringLiteral("Sony"),    QStringLiteral("Google"),
        QStringLiteral("Lenovo"),
    };
    for (const QString &b : brands) {
        engmode::Entry e = engmode::lookup(b, QString(), QString());
        QVERIFY2(engmode::isValid(e), qPrintable(QStringLiteral("brand %1 应返回有效入口").arg(b)));
        QVERIFY2(!e.name.isEmpty(), qPrintable(QStringLiteral("brand %1 的入口应有名称").arg(b)));
    }
}

void TestEngMode::lookup_unknownBrand_empty()
{
    // 未知品牌 → 空 Entry（无入口降级）
    const engmode::Entry e = engmode::lookup(QStringLiteral("FoobarBrand"), QStringLiteral("whatever"), QString());
    QVERIFY(!engmode::isValid(e));
    QVERIFY(e.dialCode.isEmpty());
    QVERIFY(e.activity.isEmpty());
}

void TestEngMode::lookup_chipFallback_mtk()
{
    // 未知品牌 + MTK 芯片 → MTK 工程模式（hardware 含 mtk/mediatek）
    const QStringList hws = {
        QStringLiteral("mt6765"), QStringLiteral("MT6765"),
        QStringLiteral("mediatek mt6893"), QStringLiteral("mtk6893"),
    };
    for (const QString &hw : hws) {
        engmode::Entry e = engmode::lookup(QStringLiteral("NoNameBrand"), hw, QString());
        QVERIFY2(engmode::isValid(e), qPrintable(QStringLiteral("hw %1 应命中 MTK 入口").arg(hw)));
        QVERIFY2(e.dialCode.contains(QStringLiteral("3646633")),
                 qPrintable(QStringLiteral("MTK 拨号码应含 3646633，实际 %1").arg(e.dialCode)));
    }
}

void TestEngMode::lookup_chipFallback_qualcomm()
{
    // 未知品牌 + 高通芯片 → 系统 Testing（hardware 含 qcom/sm/sdm）
    const QStringList hws = {
        QStringLiteral("sm8250"), QStringLiteral("SM8250"),
        QStringLiteral("sdm660"), QStringLiteral("kona"),
        QStringLiteral("qcom"),   QStringLiteral("lahaina"),
    };
    for (const QString &hw : hws) {
        engmode::Entry e = engmode::lookup(QStringLiteral("NoNameBrand"), hw, QString());
        QVERIFY2(engmode::isValid(e), qPrintable(QStringLiteral("hw %1 应命中 Qualcomm/Testing 入口").arg(hw)));
        QVERIFY2(e.dialCode.contains(QStringLiteral("4636")),
                 qPrintable(QStringLiteral("高通/系统拨号码应含 4636，实际 %1").arg(e.dialCode)));
    }
}

void TestEngMode::lookup_blankInputs_noCrash()
{
    // 全局契约：空输入不崩溃，返回空 Entry
    const engmode::Entry e = engmode::lookup(QString(), QString(), QString());
    QVERIFY(!engmode::isValid(e));
}

void TestEngMode::isValid_rejectsEmptyAcceptsEntry()
{
    engmode::Entry empty;
    QVERIFY(!engmode::isValid(empty));

    // 仅拨号码（以 *# 开头）→ 有效
    engmode::Entry dial;
    dial.dialCode = QStringLiteral("*#0*#");
    QVERIFY(engmode::isValid(dial));

    // 联想已验证代码为 ## 前缀（####1111#）→ 同样有效
    engmode::Entry dialSharp;
    dialSharp.dialCode = QStringLiteral("####1111#");
    QVERIFY(engmode::isValid(dialSharp));

    // 仅 Activity（包名/ 形式）→ 有效
    engmode::Entry act;
    act.activity = QStringLiteral("com.mediatek.engineermode/.EngineerMode");
    QVERIFY(engmode::isValid(act));

    // 非法字段：拨号码不以 *# 开头、Activity 无包名斜杠 → 无效
    engmode::Entry badDial;
    badDial.dialCode = QStringLiteral("#3646633");
    QVERIFY(!engmode::isValid(badDial));

    engmode::Entry badAct;
    badAct.activity = QStringLiteral(".EngineerMode");
    QVERIFY(!engmode::isValid(badAct));
}

QTEST_APPLESS_MAIN(TestEngMode)
#include "test_engineer_mode.moc"

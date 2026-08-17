#include "engineer_mode.h"

#include <QMap>

namespace engmode {

namespace {

// 品牌别名 → 规范品牌名（键为小写）。仅收录"有入口或可归并"的品牌；
// 未收录品牌（如 Meizu/Nothing 等）由 detectBrand 返回空串，走芯片回退。
const QMap<QString, QString> &brandAliases()
{
    static const QMap<QString, QString> m = {
        { "xiaomi",  "Xiaomi"  }, { "redmi", "Xiaomi" }, { "poco", "Xiaomi" },
        { "samsung", "Samsung" },
        { "huawei",  "Huawei"  },
        { "honor",   "Honor"   },
        { "oppo",    "OPPO"    },
        { "oneplus", "OnePlus" },
        { "realme",  "realme"  },
        { "vivo",    "vivo"    }, { "iqoo", "vivo" },
        { "google",  "Google"  },
        { "sony",    "Sony"    },
        { "lenovo",  "Lenovo"  },
    };
    return m;
}

// 品牌入口表。每条注释含验证来源 URL（2026-08-05 联网核实，交叉核对）。
const QMap<QString, Entry> &brandEntries()
{
    static const QMap<QString, Entry> m = {
        // 小米/红米/POCO — CIT 硬件测试（官方售后故障排查用代码）
        // 来源: https://gadgets.beebom.com/guides/xiaomi-secret-security-codes-list
        //       https://drfone.wondershare.com/android-xiaomi/how-to-use-the-hidden-cit-menu-on-xiaomi-phone-to-test-hardware.html
        //       https://www.mi.com/tw/support/faq/details/KA-266552/ （官方）
        // activity 包名（com.longcheer.cit / com.xiaomi.cit）未能核实 → 仅拨号入口。
        { "Xiaomi", { "CIT 硬件测试", "*#*#6484#*#*", QString(),
                      "已验证(Beebom/drfone/小米官方)；activity 未验证，仅拨号" } },

        // 三星 — FactoryTest 硬件测试菜单
        // 来源: https://gadgets.beebom.com/guides/samsung-secret-codes-list
        //       https://us.community.samsung.com/t5/Fold-Flip-Phones/System-Diagnostics-Menu-Not-Working-0/td-p/2973826
        // 包名 com.sec.android.app.hwmoduletest 已验证（Universal-Debloater issue #800），
        // Activity 类名未核实 → 仅拨号入口。
        { "Samsung", { "FactoryTest 硬件测试", "*#0*#", QString(),
                       "已验证(Beebom/三星官方社区)；activity 类名未验证，仅拨号" } },

        // 华为 — 工程菜单 Project Menu（拨号后需再按拨号键）
        // 来源: https://club-api.c.hihonor.com/thread-8457896-1-1.html （华为/荣耀俱乐部指令大全）
        //       https://product.pconline.com.cn/itbk/sjtx/sjwt/1534/15348280.html
        //       https://blog.csdn.net/arieshao/article/details/76619676 （logcat 工程模式）
        { "Huawei", { "工程菜单 (Project Menu)", "*#*#2846579#*#*", QString(),
                      "已验证(华为/荣耀俱乐部+多来源)；activity 未验证，仅拨号" } },

        // 荣耀 — 沿用华为项目菜单代码
        { "Honor", { "工程菜单 (Project Menu)", "*#*#2846579#*#*", QString(),
                     "已验证(荣耀俱乐部/多来源)；activity 未验证，仅拨号" } },

        // OPPO — Engineer Mode（ColorOS 家族）
        // 来源: https://aiot.csdn.net/69df66290a2f6a37c5a02860.html （*#899# 简介）
        //       https://gadgets.beebom.com/guides/android-secret-codes-list
        // 备选：*#36446337#（部分机型）。
        { "OPPO", { "工程模式 (Engineer Mode)", "*#899#", QString(),
                    "已验证(CSDN/Beebom)；备选 *#36446337#；activity 未验证" } },

        // 一加 — 工程模式（旧机型 *#899# 为工程模式，新机型可能为售后界面）
        // 来源: https://technastic.com/oneplus-secret-codes-hidden-settings/
        //       https://gadgets.beebom.com/guides/oneplus-secret-codes-list
        //       https://xdaforums.com/t/codes-documented-secret-codes-for-oneplus-5t.4186175/
        { "OnePlus", { "工程模式 (Engineer Mode)", "*#899#", QString(),
                       "已验证(Technastic/Beebom/XDA)；备选 *#36446337#/*#808#" } },

        // realme — Engineer Mode（RealmeUI 沿用 ColorOS 家族代码）
        // 来源: https://aiot.csdn.net/69df66290a2f6a37c5a02860.html
        //       https://page.sm.cn/blm/midpage-317/index?id=11_9764165450a4683bdb202d080a3e171a
        { "realme", { "工程模式 (Engineer Mode)", "*#899#", QString(),
                      "已验证(CSDN/多来源)；部分机型可用 *#*#6484#*#*" } },

        // vivo/iQOO — 工厂测试（部分 OriginOS 需先开启调试端口）
        // 来源: https://blog.csdn.net/sunny_day_day/article/details/107451956 （指令代码大全）
        //       https://www.php.cn/faq/2303006.html （iQOO 工程模式）
        //       https://ask.zol.com.cn/x/16396594.html （vivo 拨号盘隐藏代码）
        { "vivo", { "工程测试 (工厂测试)", "*#558#", QString(),
                    "已验证(CSDN/php.cn/ZOL 多来源)；activity 未验证，仅拨号" } },

        // 索尼 Xperia — Service Menu（*#*#SERVICE#*#*）
        // 来源: https://zh.ifixit.com/Guide/translate/24223/61959/zh （Service/Test Menu 指南）
        //       https://fdroid.gitlab.io/jekyll-fdroid/en/packages/sony.hidden.servicemenu/
        //       https://lineageos.github.io/lineage_wiki/devices/nicki/install/
        { "Sony", { "Service Menu 服务菜单", "*#*#7378423#*#*", QString(),
                    "已验证(iFixit/F-Droid/LineageOS Wiki)；activity 未验证" } },

        // 联想 — 功能测试/工程模式（机型差异大：部分型号 ####1040# 才是工程模式）
        // 来源: https://cloud.tencent.com.cn/developer/article/2179161
        //       https://m.zol.com.cn/sjbbs/d1763_144122.html （联想手机指令 工程模式）
        //       https://ng.d.cn/wangyouzonghequ/news/detail_34157_1.html （联想TD800）
        { "Lenovo", { "功能测试/工程模式", "####1111#", QString(),
                      "已验证(ZOL/腾讯云/多来源)；机型差异大，备选 ####1040#" } },

        // Google Pixel — 无厂商工程模式，回退 AOSP 系统 Testing 菜单
        // 来源: https://mobilespecs.net/phone/codes/Google/Google_Pixel_XL.html
        //       https://www.getdroidtips.com/google-pixel-7-pro-secret-codes/
        { "Google", { "系统测试模式 (Testing)", "*#*#4636#*#*", QString(),
                      "已验证(mobilespecs/getdroidtips)；Pixel 无厂商工程模式" } },
    };
    return m;
}

const Entry &mtkEntry()
{
    // MTK 工程模式 — 拨号 *#*#3646633#*#* + Activity 双入口
    // 来源: https://appuals.com/how-to-increase-maximum-volume-on-mediatek-based-android-devices-through-engineer-mode/
    //       https://blog.csdn.net/July_king/article/details/130325611
    //       （adb shell am start -n com.mediatek.engineermode/.EngineerMode，已核实）
    static const Entry e = {
        "MTK 工程模式", "*#*#3646633#*#*",
        "com.mediatek.engineermode/.EngineerMode",
        "已验证(Appuals/CSDN 双来源)；拨号+Activity 均可用",
    };
    return e;
}

const Entry &qualcommEntry()
{
    // 高通/系统 Testing 菜单 — 拨号 *#*#4636#*#* + AOSP Settings TestingSettings
    // 来源: https://www.makeuseof.com/android-testing-menu-shortcode-trick/
    //       https://www.phonearena.com/news/How-to-access-your-Android-phones-hidden-Testing-menu_id73160
    //       AOSP Settings 源码 TestingSettingsBroadcastReceiver（secret code host=4636，
    //       https://git.replicant.us/replicant-next/packages_apps_Settings/log/... 核实）
    static const Entry e = {
        "系统测试模式 (Testing)", "*#*#4636#*#*",
        "com.android.settings/.TestingSettings",
        "已验证(makeuseof/phonearena/AOSP 源码)；系统自带 Testing",
    };
    return e;
}

// 芯片回退：hardware 含 MTK 标识 → MTK 工程模式；高通平台 → 系统 Testing。
const Entry *findChipEntry(const QString &hardware)
{
    const QString h = hardware.trimmed().toLower();
    if (h.isEmpty())
        return nullptr;
    // MTK：mtk/mediatek 字样，或真实硬件串 mt+数字（如 mt6765/mt6893，见
    // xiaomi-mt6853-devs 等设备树命名）；"mt"后须为数字防误判
    if (h.contains("mtk") || h.contains("mediatek")
        || (h.startsWith("mt") && h.size() >= 3 && h.at(2).isDigit()))
        return &mtkEntry();
    // 高通 SoC：qcom 字样、SMxxxx/SDMxxx 平台号、常见代号（kona=SM8250、lahaina=SM8350、taro=SM8450）
    if (h.contains("qcom") || h.startsWith("sm") || h.startsWith("sdm")
        || h.contains("kona") || h.contains("lahaina") || h.contains("taro"))
        return &qualcommEntry();
    return nullptr;
}

} // namespace

QString detectBrand(const QString &roProductBrand)
{
    const QString t = roProductBrand.trimmed();
    if (t.isEmpty())
        return QString();
    const auto it = brandAliases().constFind(t.toLower());
    return it == brandAliases().constEnd() ? QString() : it.value();
}

Entry lookup(const QString &brand, const QString &hardware, const QString &model)
{
    Q_UNUSED(model); // 保留参数位（调用方可传 ro.product.model）；当前表按品牌/芯片足够

    // 1) 品牌表优先
    const QString b = detectBrand(brand);
    if (!b.isEmpty()) {
        const auto it = brandEntries().constFind(b);
        if (it != brandEntries().constEnd())
            return it.value();
    }
    // 2) 芯片回退（MTK/高通）
    if (const Entry *chip = findChipEntry(hardware))
        return *chip;
    // 3) 无已知入口降级
    Entry empty;
    empty.note = QStringLiteral("未识别品牌/芯片，无已知工程模式入口");
    return empty;
}

bool isValid(const Entry &e)
{
    // 拨号码：*# 开头（常见）或 ## 开头（联想 ####1111# 已验证）
    const QString d = e.dialCode.trimmed();
    const bool dialOk = d.startsWith(QStringLiteral("*#")) || d.startsWith(QStringLiteral("##"));
    // Activity：组件形式"包名/类名"（包名含点号、类名非空）
    const QString a = e.activity.trimmed();
    bool actOk = false;
    if (!a.isEmpty()) {
        const int slash = a.indexOf(QLatin1Char('/'));
        if (slash > 0 && slash < a.size() - 1) {
            const QString pkg = a.left(slash);
            const QString cls = a.mid(slash + 1);
            actOk = pkg.contains(QLatin1Char('.')) && !cls.isEmpty();
        }
    }
    return dialOk || actOk;
}

} // namespace engmode

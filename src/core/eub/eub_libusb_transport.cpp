// src/core/eub/eub_libusb_transport.cpp
//
// 结构照抄 Phase C 的 src/core/odin/odin_libusb_transport.cpp（本仓已过审的同款真机实现：
// open 的枚举→筛选→打开→claim 骨架、claim→detach 内核驱动→再 claim、短写拒绝、超时判定），
// 差异只有头文件所列三处（readDeviceInfo / 无 ZLP / open 重新枚举设备）。
//
// ⚠️ 真机行为（枚举顺序 / claim / 端点发现 / 时序 / 回显）**离线无法验证**：本机无任何
// Exynos 设备（facts §F1）。本文件只到"逐条对齐参照实现"这一层证据，真机留给持机人。
#include "eub_libusb_transport.h"

#include <libusb.h>

#include <QStringList>
#include <limits>   // std::numeric_limits（writeBulk 的长度上限判定）

namespace eub {
namespace {

constexpr quint16 kEubVid = 0x04e8;   // facts §A1
constexpr quint16 kEubPid = 0x1234;   // facts §A1
// 出处写全仓库段：reference/ 下有 exynos-usbdl/ 与 exynos-usbdl-vdavid003/ 两份同名
// exynos-usbdl.c，只写文件名会解析到错的那份（后者同位置是别的内容）。
constexpr int kFallbackEpOut = 0x02;  // reference/exynos-usbdl/exynos-usbdl.c:60 / dltool.c:339 / hubble.py:110
constexpr int kFallbackEpIn  = 0x81;  // reference/exynos-usbdl/exynos-usbdl.c:242 / hubble.py:115

QString hex4(quint16 v)
{
    return QString::number(v, 16).rightJustified(4, QLatin1Char('0'));
}

QString usbErr(int rc)
{
    return QString::fromLatin1(libusb_error_name(rc));
}

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

// 字符串描述符（失败/为空 → 空串，**不置 error**：老 SoC 可能没有这些串，facts §A4）
QString readString(libusb_device_handle *dev, quint8 index)
{
    if (index == 0)
        return {};   // 索引 0 = "本设备没有这个串"（libusb 语义），直接发请求只会白等一次
    unsigned char buf[256];
    const int n = libusb_get_string_descriptor_ascii(dev, index, buf, sizeof(buf));
    if (n <= 0)
        return {};
    return QString::fromLatin1(reinterpret_cast<const char *>(buf), n);
}

// 一次描述符巡检的结果。
struct Endpoints {
    int iface = -1;
    int epOut = 0;
    int epIn = 0;
    bool fromDescriptor = false;   // false = 描述符里没有可用的批量对（调用方回退常数 + 记 notes）
};

// 优先 interface 0 的批量对；否则第一个带批量对的接口；都没有 → fromDescriptor=false。
// 为什么优先 interface 0：hubble 显式 claim interface 0（facts §B1，hubble.py:304-308）。
// 为什么**不**挑类（对比 odin 的 CDC 0x0A 判据）：EUB 的身份只在 PID 上（facts §A1），
// 三个参照实现谁都没有按接口类筛选过（facts §B2 全是写死端点）。
Endpoints resolveEndpoints(libusb_device *dev)
{
    Endpoints out;
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != LIBUSB_SUCCESS || !cfg)
        return out;   // 描述符读不到（未配置/权限）→ 调用方回退常数端点

    for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
        for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
            const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
            int epIn = 0;
            int epOut = 0;
            for (int e = 0; e < int(alt->bNumEndpoints); ++e) {
                const libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;   // 只看批量端点（EUB 的帧全走 bulk）
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                    epIn = ep->bEndpointAddress;
                else
                    epOut = ep->bEndpointAddress;
            }
            if (epIn == 0 || epOut == 0)
                continue;   // 只带单方向的接口不是下载接口

            // 第一个同时带批量 in/out 的接口作退路；interface 0 命中时**覆盖**它（接口 0 是
            // hubble 的认领对象）。描述符里的接口顺序不保证按号升序，故两个条件都留着。
            if (out.iface < 0 || alt->bInterfaceNumber == 0) {
                out.iface = alt->bInterfaceNumber;
                out.epOut = epOut;
                out.epIn  = epIn;
                out.fromDescriptor = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 纯函数：设备身份与超时换算
// ---------------------------------------------------------------------------

bool LibusbEubTransport::isEubDevice(quint16 vid, quint16 pid)
{
    // facts §A1：VID 0x04E8（三星公共 VID，正常开机/充电的机器也是它）+ PID 0x1234，全 SoC 一致。
    // **两侧都要精确匹配**：EUB 的身份没有接口类可依赖（与 Odin 的"类 0x0A + 批量端点"判据不同），
    // 而 PID 0x1234 不在 Odin 的老 PID 兜底表里 —— 这正是 facts §E2 的交叠面，认领顺序由
    // samsung_mode.h（Task 2）固化：EUB 判据必须**先于** Odin 判据执行。
    return vid == kEubVid && pid == kEubPid;
}

QString LibusbEubTransport::noDeviceError()
{
    // 文案给**现实前提**（facts §A5 进不去 EUB / §A7 eFuse 已封；§F6 前置条件由用户承担）。
    // 不写 "facts §X" 这类内部编号 —— 这句话是给用户看的。
    return QStringLiteral("未找到三星 Exynos EUB（USB-Boot）设备：期望 VID 0x%1 / PID 0x%2。"
                          "先确认设备确实回退到了 EUB —— 主引导失败才会进入 USB 下载模式；"
                          "若机型较新，还可能已被 eFuse 永久封堵（三星 2025-04 起的更新）。")
        .arg(hex4(kEubVid), hex4(kEubPid));
}

int LibusbEubTransport::effectiveTimeoutMs(int requested)
{
    // libusb 的 0 = **无限等待**（sync.c "For an unlimited timeout, use value 0"），照搬传 0 会让
    // 一次"读回显"在真机上永久阻塞（会话跑在 UI 线程时就是整个界面卡死）。取 1 ms：本接口的读
    // 只有"段后回显"一种用途（facts §C7/§C8），1 ms 足够问一次端点上有没有字节。
    // 与 odin/edl 同款红线（odin_libusb_transport.h:73-76）。公开仅为单测。
    return requested > 0 ? requested : 1;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

LibusbEubTransport::LibusbEubTransport() = default;

LibusbEubTransport::~LibusbEubTransport()
{
    close();
}

bool LibusbEubTransport::open(QString *error)
{
    if (m_dev) close();     // 契约：close 后可再 open（重复 open 先收尾，避免拿两个句柄）
    // 每次 open 都是**重新枚举**：EUB 设备在段间可能重枚举、地址会变（头文件差异 ③ / facts §B8）。
    // notes 也在这里清：新一次打开不得带出上一次的说明（close() 不清，见那里的注释）。
    m_notes.clear();

    int ret = libusb_init(&m_ctx);
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("libusb_init 失败：%1").arg(usbErr(ret)));
        m_ctx = nullptr;
        return false;
    }

    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(m_ctx, &list);
    if (count < 0) {
        setErr(error, QStringLiteral("枚举 USB 设备失败：%1").arg(usbErr(int(count))));
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    bool found = false;
    for (ssize_t i = 0; i < count && !found; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;
        if (!isEubDevice(desc.idVendor, desc.idProduct))
            continue;    // 判据不符：连端点都不必解析（facts §A1）

        const Endpoints eps = resolveEndpoints(list[i]);
        int iface = eps.iface;
        int epOut = eps.epOut;
        int epIn  = eps.epIn;
        QStringList notes;   // 该候选自己的说明；只有**打开成功**才提交给 m_notes
        if (!eps.fromDescriptor) {
            // 描述符里没有可用的批量对（老机型不带、或被系统按别的类枚举）→ 用参照实现的常数。
            // 这是**回退**不是等价选择：三个参照都硬编码这对值（facts §B2），但"它在别的机型上
            // 是否成立"我们无法离线验证 → 写进 notes() 让会话转进日志（用户看得见这次打开用了回退）。
            iface = 0;
            epOut = kFallbackEpOut;
            epIn  = kFallbackEpIn;
            notes << QStringLiteral("EUB 端点回退：描述符里没有批量 in/out 对，改用参照实现的常数 "
                                    "OUT 0x%1 / IN 0x%2（reference/exynos-usbdl/exynos-usbdl.c:60,242 / "
                                    "dltool.c:339 / hubble.py:110,115）")
                     .arg(kFallbackEpOut, 2, 16, QLatin1Char('0'))
                     .arg(kFallbackEpIn, 2, 16, QLatin1Char('0'));
        }

        if (libusb_open(list[i], &m_dev) == LIBUSB_SUCCESS) {
            m_iface = iface;
            m_epOut = epOut;
            m_epIn  = epIn;
            m_bus     = int(libusb_get_bus_number(list[i]));
            m_address = int(libusb_get_device_address(list[i]));
            m_vid = desc.idVendor;
            m_pid = desc.idProduct;
            m_notes = notes;
            found = true;
        } else {
            m_dev = nullptr;   // 该设备打不开（权限/被占用）→ 继续找下一个
        }
    }
    libusb_free_device_list(list, 1);

    if (!found) {
        setErr(error, noDeviceError());
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    // 配置态：未配置的设备 claim/bulk 都无从谈起。dltool 是无条件 set_configuration(1)
    // （dltool.c:305），我们**只在**"读不到活动配置或值 == 0"时补设 —— 无条件的代价不是 BUSY
    // 而是设备状态被重置（libusb api-1.0 `libusb_set_configuration` 文档：对已配置设备再设会
    // "act as a lightweight device reset"，altsetting 归零、端点 halt 清除、toggle 复位）。
    // 补设必须发生在 claim **之前**（同一份文档："It is advised to set the desired configuration
    // before claiming interfaces"；claim 之后再设会得到 BUSY = "interfaces are currently claimed"）。
    int cfgValue = 0;
    const int cfgRead = libusb_get_configuration(m_dev, &cfgValue);
    if (cfgRead != LIBUSB_SUCCESS || cfgValue == 0) {
        const int sc = libusb_set_configuration(m_dev, 1);
        if (sc != LIBUSB_SUCCESS) {
            if (cfgRead == LIBUSB_SUCCESS) {
                // 值确实读到 0：设备**确定**未配置，补设又失败 → 后面每一步都不可能成功，硬失败。
                setErr(error, QStringLiteral("设置 EUB 设备配置 1 失败：%1").arg(usbErr(sc)));
                close();
                return false;
            }
            // 读失败 = 只是"不知道"：记一笔继续 —— 真有硬伤会在 claim 处如实报错（不制造假失败）。
            m_notes << QStringLiteral("读取活动配置失败（%1），补设配置 1 也失败（%2）—— 继续尝试认领接口")
                       .arg(usbErr(cfgRead), usbErr(sc));
        } else {
            // 补设成功同样要留痕：这**不是空操作** —— libusb 文档明说对设备重发 SET_CONFIGURATION
            // 相当于一次轻量复位（altsetting 归零、端点 halt 清除、toggle 复位）。"补设"这个动作
            // 本身有副作用，正是 notes() 存在的理由（失败才记会漏掉"我们动了设备状态"这一事实）。
            m_notes << QStringLiteral("读活动配置未确认已配置，已显式补设配置 1：libusb 文档说明这会重发 "
                                      "SET_CONFIGURATION，相当于一次轻量复位 —— altsetting 归零、"
                                      "端点 halt 清除、toggle 复位");
        }
    }

    if (!claimInterface(error)) {
        close();                  // 半开状态不留：失败即回到"未打开"
        return false;
    }
    return true;
}

bool LibusbEubTransport::claimInterface(QString *error)
{
    // claim → 失败则 detach 内核驱动 → 再 claim（既有顺序，odin_libusb_transport.cpp:202-218 同款）：
    // EUB 接口在 Linux 上可能被内核驱动占用（hubble 的路径显式 detach，facts §B1 / hubble.py:304-308），
    // 第一次 claim 会 LIBUSB_ERROR_BUSY。
    int ret = libusb_claim_interface(m_dev, m_iface);
    if (ret != LIBUSB_SUCCESS) {
        const int det = libusb_detach_kernel_driver(m_dev, m_iface);
        if (det == LIBUSB_SUCCESS || det == LIBUSB_ERROR_NOT_FOUND)
            ret = libusb_claim_interface(m_dev, m_iface);
    }
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("认领 EUB 接口 %1 失败：%2").arg(m_iface).arg(usbErr(ret)));
        return false;
    }
    return true;
}

void LibusbEubTransport::close()
{
    if (m_dev) {
        if (m_iface >= 0)
            libusb_release_interface(m_dev, m_iface);
        libusb_close(m_dev);
        m_dev = nullptr;
    }
    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
    // 接口/端点/地址回到"未知"：open() 的候选选择必须每次重新枚举，不能留上次的值（facts §B8）
    m_iface = -1;
    m_epOut = 0;
    m_epIn  = 0;
    m_bus = 0;
    m_address = 0;
    m_vid = 0;
    m_pid = 0;
    // m_notes 故意**不**在这里清：open 失败后 close 再打日志仍要能读到当次说明；清点在 open() 开头。
}

// ---------------------------------------------------------------------------
// 设备自述信息
// ---------------------------------------------------------------------------

bool LibusbEubTransport::readDeviceInfo(EubDeviceInfo &out, QString *error)
{
    if (!m_dev) {
        setErr(error, QStringLiteral("读取设备信息失败：EUB 设备未打开"));
        return false;
    }

    libusb_device_descriptor desc{};
    const int ret = libusb_get_device_descriptor(libusb_get_device(m_dev), &desc);
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("读取设备描述符失败：%1").arg(usbErr(ret)));
        return false;
    }

    out = EubDeviceInfo{};   // 覆盖式写入：本次读不到的字段必须是**空**，不带上次的残留（facts §A4）
    out.socName = readString(m_dev, desc.iProduct);                   // facts §A2（如 "Exynos9610"）
    const QString serial = readString(m_dev, desc.iSerialNumber);     // facts §A3
    out.socId  = serial.left(15);        // hubble.py:219 的 [0:15]
    out.chipId = serial.mid(15, 16);     // hubble.py:220 的 [15:31]

    // 接口串（USB Booting Version）：活动配置的 interface[0].altsetting[0].iInterface，取 [12:16]
    // （hubble.py:211 取串、hubble.py:223 取 [12:16]）
    QString ifStr;
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(m_dev), &cfg) == LIBUSB_SUCCESS && cfg) {
        if (cfg->bNumInterfaces > 0 && cfg->interface[0].num_altsetting > 0)
            ifStr = readString(m_dev, cfg->interface[0].altsetting[0].iInterface);
        libusb_free_config_descriptor(cfg);
    }
    out.usbBootVersion = ifStr.mid(12, 4);

    out.vid = m_vid;                  // 本次打开时记下的身份（不是重新枚举的结果）
    out.pid = m_pid;
    out.bus = quint8(m_bus);
    out.address = quint8(m_address);
    return true;
}

// ---------------------------------------------------------------------------
// 数据面
// ---------------------------------------------------------------------------

bool LibusbEubTransport::writeBulk(const QByteArray &data, QString *error)
{
    if (data.isEmpty()) {
        // EUB **没有 ZLP 语义**（头文件差异 ② / facts §B7：一段一帧、发完即走，三家都没有逐帧
        // 应答、也没有"空写表示结束"）—— 空数组在这里只可能是调用方出错（帧至少 10 字节头尾），
        // 明确拒绝好过静默发一个 0 长度传输：后者在真机上是一次无人预期的总线事务。
        // （排在未打开守卫之前：空帧判据不依赖设备状态，离线可断言该行为）
        setErr(error, QStringLiteral("写入失败：EUB 不接受空帧（本协议没有 ZLP 语义，facts §B7）"));
        return false;
    }
    if (!m_dev) {
        setErr(error, QStringLiteral("写入失败：EUB 设备未打开"));
        return false;
    }

    // 单帧长度必须塞得进 libusb 的 `int length`。今天这条由上游定表保证不可达（一帧 = 一段
    // sboot 切片，"切段"在载荷层完成），留着是因为将来若改大块/64 位缓冲，这里必须**拒绝**
    // 而不是窄化（odin_libusb_transport.cpp:262-270 同款；Qt6 的 QByteArray::size() 是 qsizetype）。
    if (data.size() > std::numeric_limits<int>::max()) {
        setErr(error, QStringLiteral("写入失败：单帧 %1 字节超过 libusb 的 int 长度上限")
                          .arg(data.size()));
        return false;
    }

    int transferred = 0;
    const int ret = libusb_bulk_transfer(m_dev, m_epOut,
                                         reinterpret_cast<unsigned char *>(const_cast<char *>(data.constData())),
                                         static_cast<int>(data.size()), &transferred, kWriteTimeoutMs);
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("写入 OUT 端点 0x%1 失败：%2")
                          .arg(m_epOut, 2, 16, QLatin1Char('0')).arg(usbErr(ret)));
        return false;
    }
    if (transferred != data.size()) {
        // 短写：bulk 传输在超时时可能少发 —— 绝不能当成功（会话会继续发下一段，设备侧错位）
        setErr(error, QStringLiteral("写入 OUT 端点短写：已发 %1/%2 字节")
                          .arg(transferred).arg(data.size()));
        return false;
    }
    return true;
}

QByteArray LibusbEubTransport::readBulk(int maxBytes, int timeoutMs, QString *error)
{
    if (!m_dev) {
        // 未打开时**连读不到东西也报错**（调用方拿它诊断"谁没 open"，而不是把空返回当"设备没回显"）
        // —— 与 odin_libusb_transport.cpp:290-297 同款。
        setErr(error, QStringLiteral("读取失败：EUB 设备未打开"));
        return {};
    }
    if (maxBytes <= 0)
        return {};

    QByteArray buf(maxBytes, Qt::Uninitialized);
    int transferred = 0;
    // ⚠️ 绝不把 0 直接交给 libusb（0 = 无限等待，见 effectiveTimeoutMs 的注释）
    const int effective = effectiveTimeoutMs(timeoutMs);
    const int ret = libusb_bulk_transfer(m_dev, m_epIn,
                                         reinterpret_cast<unsigned char *>(buf.data()),
                                         maxBytes, &transferred, effective);
    if (ret == LIBUSB_ERROR_TIMEOUT) {
        // 本接口**没有** odin 的"timeout<=0 = 轮询、不置 error"分支：EUB 的读回显只有"有超时的
        // 读"一种用途（facts §C7/§C8，hubble.py:115 的 50 ms）→ 超时一律空返回 + error。
        // 报**换算后**的值：0 已被换成 1 ms，日志要跟线上实际发生的事一致。
        setErr(error, QStringLiteral("读取 IN 端点超时（%1 ms）").arg(effective));
        return {};
    }
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("读取 IN 端点 0x%1 失败：%2")
                          .arg(m_epIn, 2, 16, QLatin1Char('0')).arg(usbErr(ret)));
        return {};
    }
    buf.truncate(transferred);
    return buf;
}

} // namespace eub

#pragma once

// MTK BROM 协议自研（计划 F1-1）
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175 源码逐条核实（见计划文档"协议核实记录"）：
//   • 命令号/响应码 → Library/mtk_preloader.py Preloader.Cmd / Rsp
//   • 握手          → Library/Port.py run_handshake()
//   • echo/状态字   → Library/Port.py echo() / mtk_cmd()
//   • get_target_config / SEND_DA / JUMP_DA → mtk_preloader.py 对应函数
//   • 枚举白名单    → config/usb_ids.py default_ids
// 关键约定：多数字段大端；命令回显模式；状态字 2B BE <=0xFF 为成功。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <memory>

namespace mtkbrom {

// ---- BROM 命令号（对照 Preloader.Cmd）----
enum Cmd : quint8 {
    CMD_SEND_PARTITION_DATA = 0x70,
    CMD_JUMP_TO_PARTITION = 0x71,
    CMD_READ16 = 0xD0,
    CMD_READ32 = 0xD1,
    CMD_WRITE16 = 0xD2,
    CMD_WRITE16_NO_ECHO = 0xD3,
    CMD_WRITE32 = 0xD4,
    CMD_JUMP_DA = 0xD5,
    CMD_JUMP_BL = 0xD6,
    CMD_SEND_DA = 0xD7,
    CMD_GET_TARGET_CONFIG = 0xD8,
    CMD_SEND_ENV_PREPARE = 0xD9,
    CMD_JUMP_DA64 = 0xDE,
    CMD_GET_HW_SW_VER = 0xFC,
    CMD_GET_HW_CODE = 0xFD,
    CMD_GET_BL_VER = 0xFE,
    CMD_GET_VERSION = 0xFF,
};

// ---- BROM 响应码（对照 Preloader.Rsp）----
enum Rsp : quint8 {
    RSP_CONF = 0x69,
    RSP_ACK = 0x5A,
    RSP_NACK = 0xA5,
    RSP_STOP = 0x96,
};

// 状态字特值（对照 send_da()）：0x1D0D = 需要 SLA 认证
constexpr quint16 kStatusSlaRequired = 0x1D0D;

struct BromDevice {
    int vid = 0;
    int pid = 0;
    QString portName; // usb-<bus>-<addr>，仅日志展示
};

// get_target_config 位域（对照 get_target_config()）
struct TargetConfig {
    bool sbc = false;      // bit0
    bool sla = false;      // bit1
    bool daa = false;      // bit2
    bool epp = false;      // bit3
    bool cert = false;     // bit4
    bool memread = false;  // bit5
    bool memwrite = false; // bit6
    bool cmdC8 = false;    // bit7
};

// get_hw_sw_ver(0xFC) 的 8B 响应（对照 mtk_preloader.py:928-930 的 unpack(">HHHH")；
// 末字段上游未使用 = 保留）
struct HwSwVer {
    quint16 hwSubCode = 0;
    quint16 hwVer = 0;
    quint16 swVer = 0;
};

// 抽象 USB 通道（协议层只依赖此接口；mock 注入单测，libusb 生产实现）
class IBromUsb {
public:
    virtual ~IBromUsb() = default;
    virtual bool open(QString *error) = 0;
    virtual bool write(const QByteArray &data, QString *error) = 0;
    virtual bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) = 0;
    // 精确读：读到**恰好 len 字节**。**默认实现 = 单次 read + 长度校验**（mock 与既有"短包即失败"的
    // 断言保持原样，判别力不变）；libusb 实现**覆盖为累加循环**（预算 = timeoutMs，分段 transfer，
    // 读满即返回）—— 对齐上游 usblib.usbread 的循环语义。真机拆包行为未验证（见 T12 诚实边界）。
    virtual bool readExact(QByteArray &out, int len, int timeoutMs, QString *error);
    // ---- 控制传输（BROM 利用注入路径；对照上游 Exploit/*.py 的 ctrl_transfer）----
    // 前四参**逐字**对应 libusb_control_transfer 的 (bmRequestType, bRequest, wValue, wIndex)：
    // requestType 就是 USB 规范的 bmRequestType（bit7 方向：0x80=设备→主机 IN；bit6..5 类型；
    // bit4..0 接收方），libusb 用同一编码（`LIBUSB_ENDPOINT_IN = 0x80`），故实现
    // **原样透传、不重新解释**。数据长度没有独立参数 —— wLength 即 data.size()。
    // data 的取值约定（本接口唯一的一处语义约定）：
    //   • **空 = 无数据阶段**（wLength 0）：上游的触发路径就是这一形
    //     —— 例 kamakiri.py:44 `ctrl_transfer(0xA1, 0, 0, self.var1, 0)`：0xA1 带 IN 位但长度为 0；
    //   • **非空 = 输出阶段**（主机→设备，wLength = data.size()）—— 例 kamakiri2.py:30 的 `0x21` 写。
    //
    // ⚠️ **失败怎么解释由调用方定，本接口不做承诺**（T1 审查 M-2 的取材缺口，2026-09-23 补记）：
    //   上游 kamakiri.py:43-47 把这个**触发传输**包在 `try/except usb.core.USBError` 里、记下
    //   `lasterror` 后 `:48 仍 return True` —— 即上游对该次传输是 **fire-and-forget，失败也报成功**
    //   （此时载荷其实可能已在跑）。本仓的调用方（`runBromExploit`）**有意更严**：失败即响亮中止
    //   （理由与逐条对照见 mtk_brom_exploit.h 的"与上游有意不同的姿态"第 1 条）。
    //   ⇒ 读本接口的人不要从"上游吞了"反推"本仓也该吞"；两处口径不同是裁决过的，不是抄漏。
    //
    // ⚠️ 已知的、**有意**的限制（2026-09-20 控制方裁决：本签名不改）。上游三族控制传输里，
    //    前两族是本仓取材要的、本签名表达得了，第三族表达不了：
    //     (1) **0 长度 IN 触发**（要）：kamakiri.py:44 `(0xA1, 0, 0, var1, 0)`，返回值被丢弃
    //         ⇒ 可表达（`data` 空）；
    //     (2) **OUT 写**（要）：kamakiri2.py:30、kamakiripl.py:28/34/46/52/59/84 的 `0x21`
    //         ⇒ 可表达（`data` 空或非空；其中 kamakiripl.py:34/52/59 长度为 0 = 无数据阶段，
    //         与 kamakiri.py:44 同形只是方向不同）；
    //     (3) **IN + 带长度读**（本仓取材不需要）：即 `requestType & 0x80` 且 `data` 非空。
    //         归类**只看"IN + wLength > 0"**（本接口没有接收缓冲区 ⇒ 表达不了），与调用方是否读
    //         返回值**无关**。出处：`0xA1` 带长度 —— kamakiri2.py:28/70/88（赋给 `self.linecode`）、
    //         kamakiripl.py:22（赋给 `get_val` 但**从未被读**，外层 `for i in range(5)` 只是把该传输
    //         重复 5 次）、:40、:78（**返回值丢弃**，:77 是注释掉的 `# value =`）；
    //         `0x80` 标准请求读 —— kamakiri2.py:31（wIndex=0）、kamakiripl.py:69/93（wIndex=0xFFFF），
    //         两处均 wLength=9。
    //         其中**真正读了返回值**的只有两处：kamakiri2.py:28（其值在 :30 的 OUT 写里被使用）与
    //         kamakiripl.py:40（其值在 :50 用作 `data_or_w_length`）；其余处返回值丢弃或赋值后从未被读。
    //         **表达不了**：`data` 是 const 引用，读回的字节无处安放。
    //    该形**两个实现都明确报错** —— 与其让上层拿到空数据却以为成功（静默丢数据），不如响亮失败。
    //    **将来要用 IN 读**（例如支持 preloader 变体 kamakiripl）**必须先扩签名**（如 `QByteArray *inOut`
    //    或拆成读/写两条）：那会让本接口的**两个实现者再改一轮**，故必须在动手前先定。
    //
    //   "够用"不只靠上面两族：上游的成功判据在 exploit_handler.py:126-141 `runpayload()` ——
    //   `usbread(4)` 比对 `0xA1A2A3A4`，是 **bulk 读**不是控制传输；载荷本体走 `port.usbwrite`（bulk，
    //   已由本接口的 write() 覆盖）、载荷地址走 `preloader.write32`。
    // 无数据阶段两个实现都传 `nullptr`（配 `wLength` 0）。⚠️ 安全性**依赖 `wLength == 0`**：本机
    // libusb 1.0.30 的 IN 回填路径对 `data` **没有 NULL 守卫**（审查者反汇编：`memcpy(data, buffer+8,
    // actual_length)`，`data` 直接来自第 6 实参），0 长度时两次拷贝都不解引用故无恙。本机探针实测
    // `memcpy(NULL, ·, 0)` 不崩，**未在其它 libusb 版本验证**。
    // 真机时序/超时行为未验证（无 MTK 真机；本仓相位只到"离线可验证"）。
    virtual bool controlTransfer(quint8 requestType, quint8 request, quint16 value, quint16 index,
                                 const QByteArray &data, int timeoutMs, QString *error) = 0;
    virtual int maxPacketSize() const { return 0x400; } // SEND_DA 分块上限
    virtual bool close() = 0;
};

// libusb 枚举（对照 usb_ids.py 白名单）
bool enumerateUsb(QList<BromDevice> &out, QString *error);

// 白名单判据：BROM 与 **preloader** 都在表内（与 enumerateUsb 的 kUsbIds **同一份表**）。
// 检测层（device_detector.cpp 的协议通道判据）也调它 —— 那里不再自己列第二份 PID 表。
//
// ⚠️ 本函数只回答"是不是 MTK 协议通道设备"，**不回答是不是 BROM**：BROM / preloader 的身份要
// 握手之后由 GET_BL_VER（0xFE）的回值判定（上游 mtk_preloader.py:664-673；本仓 getBlVer() 只记录
// 不分支）。检测层不得据此把 preloader 说成 BROM。
bool isKnownMtkUsbId(quint16 vid, quint16 pid);

// 打开 libusb 通道（ch 接管所有权；失败时 ch 保持 null）
bool openLibusbUsb(const BromDevice &dev, std::unique_ptr<IBromUsb> &ch, QString *error);

class BromSession {
public:
    BromSession(std::unique_ptr<IBromUsb> usb, const BromDevice &dev);
    ~BromSession();

    bool connect(QString *error);   // open + 握手
    bool isConnected() const { return m_connected; }

    bool echoCmd(quint8 cmd, QString *error);
    bool readStatus(quint16 &status, QString *error);
    bool getTargetConfig(TargetConfig &out, QString *error);

    // ---- D1: BROM 芯片信息（对照 mtk_preloader.py get_hwcode() / get_hw_sw_ver()）----
    // 0xFD：回 4B >I，hwCode = 高 16 位、hwVer = 低 16 位（上游 :190-191 的 regular 拆分）。
    // IoT 芯片上游改走 A2 寄存器读（:182-187 的 iot 分支）—— D1 不实现，调用方按芯片表 iot 位明确拒绝。
    bool getHwCode(quint16 &hwCode, quint16 &hwVer, QString *error);
    // 0xFC：回 8B >HHHH = (hwSubCode, hwVer, swVer, 保留)
    bool getHwSwVer(HwSwVer &out, QString *error);
    bool sendCommand(quint8 cmd, const QByteArray &payload, QByteArray &reply,
                     int replyMaxLen, QString *error);
    bool sendDa(quint32 address, quint32 length, quint32 sigLen,
                const QByteArray &data, QString *error);
    bool jumpDa(quint32 addr, QString *error);
    bool jumpDa64(quint32 addr, QString *error);

    // ---- D1-T6: BROM 版本（对照 mtk_preloader.py:657-662 get_bromver() / :664-673 get_blver()）----
    // 写 1B 命令 → 读 1B 值；**无回显校验**（与 echoCmd 不同）。blver 读到 0xFE 表示仍在 BROM
    // （上游据此置 is_brom），本实现只记录不分支。
    bool getBromVer(quint8 &out, QString *error = nullptr);
    bool getBlVer(quint8 &out, QString *error = nullptr);

    // ---- F1-2: BROM 内存协议（对照 mtk_preloader.py read()/write()）----
    bool readMemory(quint32 addr, quint32 dwords, QByteArray &out, QString *error);
    bool writeMemory(quint32 addr, const QByteArray &data, QString *error);

    bool close(QString *error);
    bool isClosed() const { return m_closed; }

    // 纯函数（供单测/复用）
    static quint32 calcDaChecksum(const QByteArray &data);
    static QByteArray expectedHandshakeEcho();

    // F1-2 扩展点
    IBromUsb *usb() { return m_usb.get(); }

private:
    bool handshake(QString *error);
    bool echoBe32(quint32 v, QString *error); // 发 4B BE 并校验回显

    std::unique_ptr<IBromUsb> m_usb;
    BromDevice m_dev;
    bool m_connected = false;
    bool m_closed = false;
};

} // namespace mtkbrom

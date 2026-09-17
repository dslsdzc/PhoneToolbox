// src/core/eub/eub_transport.h
//
// EUB 传输抽象：设备自述信息 + 纯字节管道（唯一设备依赖点是它的 libusb 实现）。
// 语义与措辞对齐 src/core/odin/odin_transport.h（同款分层：会话只经本接口碰设备）。
//
// 与 Odin 传输的三处差异：
//   ① 多一个 readDeviceInfo：EUB 设备用字符串描述符自报 SoC 名 / SoC ID / Chip ID /
//      USB Booting Version（facts §A2/§A3），会话靠它选布局表；
//   ② 没有 ZLP 语义：EUB 的下载是一段一帧、发完即走（facts §B7），不存在"空写表示结束"；
//   ③ open() 会**重新枚举并打开**设备 —— EUB 设备在段间可能重枚举（facts §B8/§B9），
//      所以每次 open 都是全新查找，"打开后再打开"必须先 close（实现里这么做）。
//
// ⚠️ 真机路径未验证：本机没有任何 Exynos 设备（facts §F1），本文件的实现与注释只到
// "对齐参照实现"这一层证据；枚举/claim/时序/回显的真机行为留持机人。
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace eub {

// 设备自述信息（打开后读一次；读不到的字段留空 —— 老 SoC 可能没有某些串，facts §A4）
struct EubDeviceInfo {
    QString socName;         // iProduct，如 "Exynos9610"（facts §A2）
    QString socId;           // iSerialNumber[0:15]（facts §A3）
    QString chipId;          // iSerialNumber[15:31]（facts §A3）
    QString usbBootVersion;  // iInterface[12:16]（facts §A3）
    quint16 vid = 0;
    quint16 pid = 0;
    quint8  bus = 0;
    quint8  address = 0;
};

class IEubTransport
{
public:
    virtual ~IEubTransport() = default;

    // 查找并打开 EUB 设备（VID/PID，facts §A1）；close 后可再 open。
    // **不锁 bus/addr**：段间设备会重枚举，地址会变（facts §B8）。
    virtual bool open(QString *error) = 0;
    virtual void close() = 0;                       // 幂等；未打开时 no-op
    virtual bool readDeviceInfo(EubDeviceInfo &out, QString *error) = 0;
    // 一次写整帧（facts §B7：hubble/dltool 都是整帧一次写；libusb 内部按包长切分）。
    // **空数组不是合法帧**（本协议没有 ZLP 语义，见文件头差异 ②）→ 实现拒绝并置 error。
    virtual bool writeBulk(const QByteArray &data, QString *error) = 0;
    // 读回显（仅 responseSupport 的表项会调，facts §C7/§C8）；超时/无数据 → 空返回 + error。
    virtual QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) = 0;

    // 打开过程中的非致命说明（如端点回退），会话把它转发到日志。默认空。
    virtual QStringList notes() const { return {}; }
};

} // namespace eub

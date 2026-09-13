// src/core/edl/edl_transport.h
#pragma once
#include <QByteArray>
#include <QString>

namespace edl {

// EDL 传输抽象：唯一的设备依赖点（真机 = LibusbEdlTransport；测试 = MockEdlTransport）
//
// 本文件是全仓唯一允许被协议模块看到的"设备缝"：sahara/firehose/edl_session 只依赖本接口，
// 不得直接碰 libusb（spec §3.1 依赖方向：sahara → IEdlTransport ← edl_libusb_transport）。
class IEdlTransport
{
public:
    virtual ~IEdlTransport() = default;
    virtual bool       open(QString *error) = 0;                            // 打开 9008 设备
    virtual void       close() = 0;                                         // 幂等
    virtual bool       write(const QByteArray &data, QString *error) = 0;   // 裸写 OUT
    virtual QByteArray read(int maxBytes, int timeoutMs, QString *error) = 0; // 裸读 IN；超时→空+error
    virtual bool       resetDevice(QString *error) = 0;                     // 退出 EDL/复位
    virtual bool       waitReenumerate(int timeoutMs, QString *error) = 0;  // Sahara→Firehose 转折
    virtual int        maxPacketSize() const = 0;                           // OUT 端点最大包长（ZLP 判定）
};

} // namespace edl

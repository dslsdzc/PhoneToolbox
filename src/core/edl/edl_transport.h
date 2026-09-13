// src/core/edl/edl_transport.h
#pragma once
#include <QByteArray>
#include <QString>

namespace edl {

// EDL 传输抽象：唯一的设备依赖点（真机 = LibusbEdlTransport；测试 = MockEdlTransport）
//
// 本文件是全仓唯一允许被协议模块看到的"设备缝"：sahara/firehose/edl_session 只依赖本接口，
// 不得直接碰 libusb（spec §3.1 依赖方向：sahara → IEdlTransport ← edl_libusb_transport）。
//
// 生命周期契约（Task 6 控制方裁定，会话与真机传输据此实现；**签名不变，只补语义**）：
//   * `close()` **幂等**；`close()` 之后**允许再次 `open()`**（Sahara→Firehose 期间设备重枚举：
//     旧句柄失效 → 必须显式 close、等新设备、再 open，这个"设备消失又回来"的硬件事件由会话
//     显式表达，才能在 mock 里按时序断言——忘掉 close 恰恰是真机上重枚举永远等不到的最常见原因）。
//   * `waitReenumerate`：**调用前须已 `close()`；返回 true 时设备已重新 `open()`**（即重枚举后的
//     重新打开由传输实现完成，调用方**不要**再 open 一次）。`timeoutMs` 是**总预算**，轮询间隔
//     由传输自定（既有经验值 3s：src/core/modes/edl_handler.cpp:589-614 的 3s + 15 次重试）。
//   * `read(maxBytes, timeoutMs=0)` = **非阻塞轮询**（drain 语义）：立即返回端点里已有的字节，
//     没有则空返回且**不置 error**。会话据此在每笔写之前清空 IN 端点残留
//     （reference/qdl/src/firehose.c:249-252：不消费完，后续写会超时）。
class IEdlTransport
{
public:
    virtual ~IEdlTransport() = default;
    virtual bool       open(QString *error) = 0;                            // 打开 9008 设备（close 后可再开）
    virtual void       close() = 0;                                         // 幂等
    virtual bool       write(const QByteArray &data, QString *error) = 0;   // 裸写 OUT
    virtual QByteArray read(int maxBytes, int timeoutMs, QString *error) = 0; // 裸读 IN；超时→空+error；0=轮询
    virtual bool       resetDevice(QString *error) = 0;                     // 退出 EDL/复位
    virtual bool       waitReenumerate(int timeoutMs, QString *error) = 0;  // Sahara→Firehose；true ⇒ 已重开
    virtual int        maxPacketSize() const = 0;                           // OUT 端点最大包长（ZLP 判定）
};

} // namespace edl

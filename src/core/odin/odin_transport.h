// src/core/odin/odin_transport.h
#pragma once
#include <QByteArray>
#include <QString>

namespace odin {

// Odin 传输抽象：唯一的设备依赖点（真机 = libusb 传输，Task 7；测试 = MockOdinTransport）
//
// 本文件是全仓唯一允许被协议模块看到的"设备缝"：odin_session 只依赖本接口，
// 不得直接碰 libusb（依赖方向：odin_session → IOdinTransport ← 真机传输实现）。
//
// 生命周期契约（与 src/core/edl/edl_transport.h 同款措辞，语义逐条对齐）：
//   * `close()` **幂等**；`close()` 之后**允许再次 `open()`**（会话失败后用户重试：
//     句柄必须能重新拿到，否则"失败不结束会话、留在 Odin 模式便于重试"的语义就是空话）。
//   * `write(data)`：**裸写 OUT 端点**，data **原样**（不补包、不分帧 —— 1024 控制包与
//     1 MiB 数据片都由会话层按协议构造）。`data` 为空 = **ZLP**（zero-length packet）：
//     Odin 的"结束序列前空写"（D7）就是它的唯一用例 —— 真机传输**必须**真的发出一个 0 长度
//     bulk 传输，**不得**静默当成"无事发生"。
//   * `read(maxBytes, timeoutMs)`：**裸读 IN 端点**，最多 maxBytes 字节（真实 USB 语义：
//     短包提前返回，"超出请求长度的部分留待下次"，见 mock 的同款注释）。
//     `timeoutMs == 0` = **非阻塞轮询**（drain 语义）：立即返回端点里已有的字节，没有则
//     空返回**且不置 error**；`timeoutMs > 0` 超时 → 空返回 + *error 非空（error 可为 nullptr）。
//   * `reset()`：句柄退场/复位，**best-effort** —— 调用方不得因它返回 false 而改变刷写结论
//     （对比：EDL 的 `resetDevice` 是"退出 EDL"的正经收尾命令，Odin 的收尾走 `0x67`，
//     本接口的 `reset` 只负责让句柄干净退场，不承担协议语义）。
//   * `maxPacketSize()`：OUT 端点包长（0 = 未打开/未知）；会话用它做 ZLP 判定，
//     `kControlPacketSize` 的 1024 不依赖它。
class IOdinTransport
{
public:
    virtual ~IOdinTransport() = default;
    virtual bool       open(QString *error) = 0;                             // 打开 Odin 下载模式设备；close 后可再开
    virtual void       close() = 0;                                          // 幂等
    virtual bool       write(const QByteArray &data, QString *error) = 0;    // 裸写 OUT；**空数组 = ZLP**
    virtual QByteArray read(int maxBytes, int timeoutMs, QString *error) = 0; // 裸读 IN；0 = 非阻塞轮询（空返回不置 error）
    virtual bool       reset(QString *error) = 0;                            // 句柄退场/复位（best-effort）
    virtual int        maxPacketSize() const = 0;                            // OUT 端点包长（0 = 未打开/未知）
};

} // namespace odin

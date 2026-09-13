// tests/mock_edl_transport.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include "core/edl/edl_transport.h"

namespace edl {

// 脚本化 transport：reads 队列按序出队；writes 全量记录；可注入失败
//
// 对照先例：tests/test_mtk_brom.cpp:8-44 的 MockUsbChannel、tests/test_spd_flash.cpp:8-47。
// "无真机可验证"的核心断言手段 = 记录的写字节（命令帧 + 数据切片）逐字节比对，
// 而非"没崩"。
//
// Task 6 追加（会话层要断言"失败路径不发 reset""成功路径 reset 在最后"，见 task-6-brief）：
//   * `calls`  —— 调用序列（"open"/"close"/"write"/"read"/"drain"/"reset"/"waitReenumerate"），
//     顺序保留；writes 只是数据，缺了顺序就无法表达"reset 在最后"。
//   * `residual` —— IN 端点里的残留字节，即"设备在 <response> 之后又发了东西"。qdl 的注释明确
//     "不消费完（读到超时）后续写会超时"（reference/qdl/src/firehose.c:249-252）—— 这里把该设备
//     行为变成可断言的红线：**有残留未 drain 时 write() 直接失败**，会话若不 drain 就会当场炸，
//     而不是在真机上表现为随机超时。
//   * `read(timeoutMs == 0)` = 非阻塞轮询（drain 语义）：只吐 `residual`，**不动** reads 队列；
//     空返回 = 端点静默。真机侧对应 libusb_bulk_transfer(timeout=0) 立即返回（Task 7）。
//     一次性 drain 若还有剩余，剩余留在队列里，下一次轮询再取。
//   * `waitReenumerate` 按 edl_transport.h 的生命周期契约建模：成功时记录一次 `open`
//     （"返回 true ⇒ 设备已重新 open()，会话不再 open"），失败时不记；预算存进 lastReenumTimeoutMs。
// 既有用例（Task 4/5）不设 residual、不读 calls，行为与本文件历史版本逐字节一致。
class MockEdlTransport : public IEdlTransport
{
public:
    QList<QByteArray> reads;          // 每次 read() 出队一个（空队列 → 返回空 + error）
    QList<QByteArray> writes;         // 每次 write() 追加（含 ZLP 的空写）
    QList<QByteArray> residual;       // IN 端点残留字节（每个元素 = 一次轮询可取的量）
    QStringList calls;                // 调用序列（顺序保留；drain 轮询逐次记录）
    int  failWriteAt = -1;            // 第 N 次 write 失败（0 基），-1=不失败
    bool openResult = true;
    bool reenumerateResult = true;
    int  maxPacket = 1024;
    int  lastReenumTimeoutMs = -1;    // 最近一次 waitReenumerate 收到的预算（断言"预算来自 FlashOptions"）

    bool open(QString *error) override { Q_UNUSED(error); calls << QStringLiteral("open"); return openResult; }
    void close() override { calls << QStringLiteral("close"); }
    bool write(const QByteArray &data, QString *error) override {
        if (!residual.isEmpty()) {
            // 复刻 qdl 观察到的设备行为：IN 端点没读干净 → 后续写超时（firehose.c:249-252）
            if (error)
                *error = QStringLiteral("注入的写失败：IN 端点仍有 %1 段残留未 drain（会话必须在写前 drain）")
                             .arg(residual.size());
            return false;
        }
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        calls << QStringLiteral("write");
        writes.append(data);
        return true;
    }
    QByteArray read(int maxBytes, int timeoutMs, QString *error) override {
        if (timeoutMs == 0) {                       // drain 轮询：绝不动 reads（见类注释）
            calls << QStringLiteral("drain");
            if (residual.isEmpty()) return {};      // 端点静默：空返回且不置 error
            const QByteArray head = residual.takeFirst();
            if (head.size() > maxBytes) {
                residual.prepend(head.mid(maxBytes));
                return head.left(maxBytes);
            }
            return head;
        }
        Q_UNUSED(maxBytes);
        calls << QStringLiteral("read");
        if (reads.isEmpty()) { if (error) *error = QStringLiteral("读超时（mock 队列空）"); return {}; }
        return reads.takeFirst();
    }
    bool resetDevice(QString *error) override { Q_UNUSED(error); calls << QStringLiteral("reset"); return true; }
    bool waitReenumerate(int timeoutMs, QString *error) override {
        Q_UNUSED(error);
        lastReenumTimeoutMs = timeoutMs;
        calls << QStringLiteral("waitReenumerate");
        if (!reenumerateResult)
            return false;                             // 设备没回来 → 会话必须中止且不发 reset
        // 契约（edl_transport.h 顶部）：返回 true 时设备已重新 open() —— 重枚举后的重新打开由**传输**
        // 完成，会话不再 open。mock 用一次 "open" 记录表示它，使"谁负责重开"在时序断言里可见。
        calls << QStringLiteral("open");
        return true;
    }
    int maxPacketSize() const override { return maxPacket; }
};

} // namespace edl

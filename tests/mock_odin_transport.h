// tests/mock_odin_transport.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include "core/odin/odin_transport.h"

namespace odin {

// 脚本化传输：reads 队列按序出队；writes 全量记录；calls 保留调用顺序。
// "无真机可验证"的断言手段 = **写出去的字节逐条比对**（命令帧 + 数据分片），以及调用序列
// （失败路径不发 0x67、close 一定在最后、轮询用的是 timeout 0）。
// 对照先例：tests/mock_edl_transport.h（同一套建模思路）。
//
// ⚠️ **轮询（timeoutMs == 0）只消费 `residual`、永不消费 `reads`** —— 与 Phase B 已过审的
//    tests/mock_edl_transport.h 逐字同款语义（"drain 只吃 residual、**从不消费 reads**"）。
//    依据：会话在握手后做的 `read(64, 0)` 是**清端点** —— 真实设备此刻并没有"应答"要交给我们。
//    若把清端点建模成"从应答脚本里出队"，Step 2 的读脚本（"LOKE" 之后紧跟起会话应答）与
//    Step 3 的 handshake 就**互相矛盾**（脚本 / 实现 / 断言三者不能同时成立：清端点会吃掉
//    beginAck，整串读错位）。要模拟"端点里已有残留字节"就往 `residual` 里放（本任务的用例
//    目前都不需要，保持为空）。**已知缺口**（与 EDL mock 的结构性盲区①同款）：本 mock 因此
//    表达不了真机上"设备抢发/响应早到、被清端点吃掉"这类缺陷。
class MockOdinTransport : public IOdinTransport
{
public:
    QList<QByteArray> reads;       // 正超时 read 的脚本化响应（FIFO）；空队列 → 返回空 + error
    QList<QByteArray> residual;    // IN 端点里的残留字节：**只有轮询（timeoutMs==0）才消费它**
    QList<QByteArray> writes;      // 每次 write() 追加（**含空写 = ZLP**）
    QStringList calls;             // "open"/"close"/"write"/"read"/"poll"/"reset"
    QList<int> readTimeouts;       // 每次 read 的 timeoutMs（钉住「轮询 = 0」）
    int  failWriteAt = -1;         // 第 N 次 write 失败（0 基），-1 = 不失败
    bool openResult = true;
    int  maxPacket = 512;

    bool open(QString *error) override { Q_UNUSED(error) calls << QStringLiteral("open"); return openResult; }
    void close() override { calls << QStringLiteral("close"); }

    bool write(const QByteArray &data, QString *error) override {
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        calls << QStringLiteral("write");
        writes.append(data);
        return true;
    }

    QByteArray read(int maxBytes, int timeoutMs, QString *error) override {
        readTimeouts.append(timeoutMs);
        if (timeoutMs == 0) {                           // 轮询（清端点）：只吃 residual，见类注释
            calls << QStringLiteral("poll");
            if (residual.isEmpty()) return {};          // 端点静默：空返回且**不置 error**
            QByteArray head = residual.takeFirst();
            if (head.size() > maxBytes) { residual.prepend(head.mid(maxBytes)); head.truncate(maxBytes); }
            return head;
        }
        calls << QStringLiteral("read");
        if (reads.isEmpty()) { if (error) *error = QStringLiteral("读超时（mock 队列空）"); return {}; }
        QByteArray head = reads.takeFirst();
        if (head.size() > maxBytes) {                   // 超出请求长度的部分留待下次（真实 USB 语义）
            reads.prepend(head.mid(maxBytes));
            head.truncate(maxBytes);
        }
        return head;
    }

    bool reset(QString *error) override { Q_UNUSED(error) calls << QStringLiteral("reset"); return true; }
    int maxPacketSize() const override { return maxPacket; }
};

} // namespace odin

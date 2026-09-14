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
// ⚠️ **轮询（timeoutMs == 0）不动 `reads` 队列** —— 与 MockEdlTransport 的 drain 语义一致
//    （见该文件 "read(timeoutMs == 0) = 非阻塞轮询 …只吐 residual，**不动** reads 队列"）。
//    任务书 Step 2 的脚本（"LOKE" 之后紧跟起会话应答）与任务书 Step 3 的 handshake
//    （握手后 `read(64, 0)` 清端点）组合起来，若轮询也出队，就会把 beginAck 吃掉、
//    整串读错位 —— 那是**任务书自身材料的一处内部矛盾**（脚本/实现/断言三者不能同时成立）。
//    取"轮询不出队"是唯一能同时满足三者且与真机一致的解：真机上握手刚完设备还没发东西，
//    非阻塞轮询本就该空返回；且轮询吃掉一条**已到达**的响应恰是 MockEdlTransport 头注释
//    记下的结构性盲区①（"设备抢发/响应早到"），不该由 mock 主动注入到每条用例里。
//    本 mock 因此**不建模**"端点已有残留字节"（odin 会话不做握手后 drain 之外的清端点，
//    PIT 末片后的空包由脚本里的空元素表达，见 queuePitDump）——已知缺口，与 EDL 同款。
class MockOdinTransport : public IOdinTransport
{
public:
    QList<QByteArray> reads;       // 每次正超时 read() 出队一个；空队列 → 返回空 + error
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
        calls << (timeoutMs == 0 ? QStringLiteral("poll") : QStringLiteral("read"));
        if (timeoutMs == 0)
            return {};                                  // 轮询：端点静默，空返回**且不置 error**（见类注释）
        if (reads.isEmpty()) {
            if (error) *error = QStringLiteral("读超时（mock 队列空）");
            return {};
        }
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

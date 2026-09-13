// tests/mock_edl_transport.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "core/edl/edl_transport.h"

namespace edl {

// 脚本化 transport：reads 队列按序出队；writes 全量记录；可注入失败
//
// 对照先例：tests/test_mtk_brom.cpp:8-44 的 MockUsbChannel、tests/test_spd_flash.cpp:8-47。
// "无真机可验证"的核心断言手段 = 记录的写字节（命令帧 + 数据切片）逐字节比对，
// 而非"没崩"。
class MockEdlTransport : public IEdlTransport
{
public:
    QList<QByteArray> reads;          // 每次 read() 出队一个（空队列 → 返回空 + error）
    QList<QByteArray> writes;         // 每次 write() 追加
    int  failWriteAt = -1;            // 第 N 次 write 失败（0 基），-1=不失败
    bool openResult = true;
    bool reenumerateResult = true;
    int  maxPacket = 1024;

    bool open(QString *error) override { Q_UNUSED(error); return openResult; }
    void close() override {}
    bool write(const QByteArray &data, QString *error) override {
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        writes.append(data);
        return true;
    }
    QByteArray read(int maxBytes, int timeoutMs, QString *error) override {
        Q_UNUSED(maxBytes); Q_UNUSED(timeoutMs);
        if (reads.isEmpty()) { if (error) *error = QStringLiteral("读超时（mock 队列空）"); return {}; }
        return reads.takeFirst();
    }
    bool resetDevice(QString *error) override { Q_UNUSED(error); return true; }
    bool waitReenumerate(int timeoutMs, QString *error) override {
        Q_UNUSED(timeoutMs); Q_UNUSED(error); return reenumerateResult;
    }
    int maxPacketSize() const override { return maxPacket; }
};

} // namespace edl

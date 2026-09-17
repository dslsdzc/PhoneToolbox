// tests/mock_eub_transport.h
//
// 脚本化 EUB 传输：写入全量记录、调用顺序保留、open 可注入连续失败
// （模拟"设备还没出现"与"段间重枚举"两种时序，facts §B8/§B9）。
// 本线无真机（facts §F1），断言手段 = **写出的帧逐字节比对**。
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/eub/eub_transport.h"

namespace eub {

class MockEubTransport : public IEubTransport
{
public:
    QList<QByteArray> writes;      // 每次 writeBulk 追加（一帧一条）
    QStringList       calls;       // "open"/"close"/"info"/"write"/"read"
    int  openFailures = 0;         // 前 N 次 open 失败（0 = 一次就成功）
    // 从第 N 次 open 调用**起**全部失败（0 基，按已发生的 open 数计；-1 = 不注入）。与 openFailures
    // （从头数）互补：后者会被前面的段消耗掉，够不到"段发完了、后续阶段 open 才失败"这类时序。
    int  failOpenFromOpenIndex = -1;
    // 注：与 writes.size() 比对，而 writes 只记**成功**的写 → 这个注入是**粘性**的：
    // 一旦命中，本次 mock 对象的后续所有 writeBulk 都会失败。需要"只失败一次"的用例请自行
    // 在失败后把 failWriteAt 复位为 -1（或改用只读断言）。
    int  failWriteAt = -1;         // 第 N 次**成功**写入之前插入失败（0 基），-1 = 不失败
    bool infoResult = true;
    EubDeviceInfo info;            // readDeviceInfo 返回内容
    QByteArray    response;        // readBulk 返回内容
    QStringList   noteList;        // notes() 返回内容

    bool open(QString *error) override {
        calls << QStringLiteral("open");
        const int openIndex = int(calls.count(QStringLiteral("open"))) - 1;   // 含本次
        if (failOpenFromOpenIndex >= 0 && openIndex >= failOpenFromOpenIndex) {
            if (error) *error = QStringLiteral("注入的打开失败（按序号）");
            return false;
        }
        if (openFailures > 0) {
            --openFailures;
            if (error) *error = QStringLiteral("注入的打开失败");
            return false;
        }
        return true;
    }
    void close() override { calls << QStringLiteral("close"); }
    bool readDeviceInfo(EubDeviceInfo &out, QString *error) override {
        calls << QStringLiteral("info");
        if (!infoResult) { if (error) *error = QStringLiteral("注入的信息读取失败"); return false; }
        out = info;
        return true;
    }
    bool writeBulk(const QByteArray &data, QString *error) override {
        // 与真机语义对齐（T1 审查交接）：空帧**无条件拒绝**（EUB 无 ZLP），且不依赖是否已打开 ——
        // 否则 mock 与真机在"未打开 + 空帧"这一格上不一致（真机那条路径
        // 见 tests/test_eub_transport.cpp:49-58 的 emptyWriteIsRejectedEvenWhenClosed）。
        if (data.isEmpty()) {
            if (error) *error = QStringLiteral("空帧");
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
    QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) override {
        Q_UNUSED(maxBytes) Q_UNUSED(timeoutMs) Q_UNUSED(error)
        calls << QStringLiteral("read");
        return response;
    }
    QStringList notes() const override { return noteList; }
};

} // namespace eub

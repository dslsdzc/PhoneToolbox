// tests/mock_eub_transport.h
//
// 脚本化 EUB 传输：写入全量记录（成功的 + 试过但失败的）、调用顺序保留、open 可注入连续失败、
// 带"打开状态"（未打开时 describe/write/read 都按真机文案失败，见 isOpen）
//
// 记账口径（不对称，用之前先读这一条）：`calls`/`attemptedWrites` 只记**过了请求校验**的调用 ——
// 空帧、未打开这两类"请求本身不合法"的拒绝**不留痕**（也不进 attemptedWrites），与真机上"压根
// 没碰设备"一致；而"已交到设备手上但失败"（failWriteAt 注入）**会**记 calls 与 attemptedWrites。
// 故判断"调用方是否真的发起过写"要用 attemptedWrites（或 calls 里的 "write"），不要用 writes。
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
    QList<QByteArray> writes;      // 每次**成功**的 writeBulk 追加（一帧一条）
    // 每次**交到设备手上**的 writeBulk 追加（= 过了"非空 + 已打开"两道请求校验之后，含随后在
    // 注入点失败的那次）。与 writes 的差集就是"写过但失败"—— 那一段以前在 mock 里完全不可观测。
    QList<QByteArray> attemptedWrites;
    QStringList       calls;       // "open"/"close"/"info"/"write"/"read"
    int  openFailures = 0;         // 前 N 次 open 失败（0 = 一次就成功）
    // 从第 N 次 open 调用**起**全部失败（0 基，按已发生的 open 数计；-1 = 不注入）。与 openFailures
    // （从头数）互补：后者会被前面的段消耗掉，够不到"段发完了、后续阶段 open 才失败"这类时序。
    int  failOpenFromOpenIndex = -1;
    // 注：与 writes.size() 比对，而 writes 只记**成功**的写 → 这个注入是**粘性**的：
    // 一旦命中，本次 mock 对象的后续所有 writeBulk 都会失败。需要"只失败一次"的用例请自行
    // 在失败后把 failWriteAt 复位为 -1（或改用只读断言）。
    int  failWriteAt = -1;         // 第 N 次**成功**写入之前插入失败（0 基），-1 = 不失败
    // 打开状态模型（A-13）：真机的 writeBulk/readBulk 都以 !m_dev 守卫打头并报"未打开"，
    // 而 mock 以前在从未 open 时也照常收下写、返回回显 —— 会话用例里"忘了 open"会静默通过。
    bool isOpen = false;           // open 成功置真、close 置假（open 先复位：失败时 = 未打开，同真机）
    bool infoResult = true;
    EubDeviceInfo info;            // readDeviceInfo 返回内容
    QByteArray    response;        // readBulk 返回内容
    bool    readResult = true;     // false = readBulk 失败（注入）：空返回 + error，同真机超时/读错路径
    QString readError;             // 注入的失败原因；空 = 用默认文案
    QStringList   noteList;        // notes() 返回内容

    bool open(QString *error) override {
        calls << QStringLiteral("open");
        isOpen = false;         // 与真机同形：open 先收尾旧句柄；任一失败路径出来后都是"未打开"
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
        isOpen = true;
        return true;
    }
    void close() override { calls << QStringLiteral("close"); isOpen = false; }
    bool readDeviceInfo(EubDeviceInfo &out, QString *error) override {
        calls << QStringLiteral("info");
        if (!isOpen) {   // 真机同款守卫与文案（eub_libusb_transport.cpp 的 readDeviceInfo）
            if (error) *error = QStringLiteral("读取设备信息失败：EUB 设备未打开");
            return false;
        }
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
        if (!isOpen) {   // 真机同款守卫与文案（eub_libusb_transport.cpp 的 writeBulk）
            if (error) *error = QStringLiteral("写入失败：EUB 设备未打开");
            return false;
        }
        // 过了请求校验：这一次确实交到了设备手上（之后失败也要留痕 —— 见 attemptedWrites 的注释）
        calls << QStringLiteral("write");
        attemptedWrites << data;
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        writes.append(data);
        return true;
    }
    QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) override {
        Q_UNUSED(maxBytes) Q_UNUSED(timeoutMs)
        calls << QStringLiteral("read");
        if (!isOpen) {   // 真机同款守卫与文案（eub_libusb_transport.cpp 的 readBulk 第一条早退）
            if (error) *error = QStringLiteral("读取失败：EUB 设备未打开");
            return {};
        }
        // 读失败注入：真机上同形（超时/端点错都是空返回 + error，见 eub_libusb_transport.cpp 的
        // readBulk）→ 会话对"读失败"与"设备没回话（空回显、无 error）"两种情形必须分得开。
        if (!readResult) {
            if (error) *error = readError.isEmpty() ? QStringLiteral("注入的读失败") : readError;
            return {};
        }
        return response;
    }
    QStringList notes() const override { return noteList; }
};

} // namespace eub

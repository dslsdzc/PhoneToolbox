// tests/mtk_mock_usb.h
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include "core/modes/mtk_brom.h"

// MTK 帧层测试共用的 IBromUsb 假通道。此前 6 个测试文件各留一份副本，分两族：
//   • LEGACY 侧（test_mtk_brom / test_mtk_payload）= 带 failOpen、无写失败注入；
//   • 帧层侧（xflash_session / xflash_payload）= 带 failWriteAt、无 failOpen；
//     xml_session / xml_payload 两份与帧层侧逐字相同（只差尾注释）。
// 本头取两族的并集。**read/write 语义是既有用例的判据来源，逐字沿用**：
//   • reads 空队列 → read 返回 false；队列里的**空条目**同样返回 false（空包 = 设备没答），
//     prologueReads 等夹具的"0xFC 降级继续"用例就靠这一条（空条目被消耗、后续队列不受影响）。
//   • write() 恒成功，除非 failWriteAt 命中；writes 是拼接字节、writeFrames 是逐笔（**含空包**）。
//   • read 不校验请求长度：给少了照收、给多了按 maxLen 截断（裸 read 调用点的上层守卫各自负责，
//     帧层路径已有 readExact 的严格长度校验）。
//
//   • 控制传输（controlTransfer）：默认**恒成功**，请求按序追加进 controlTransfers 供断言；
//     failControlAt 与 failWriteAt 同为**一次性**注入，失败的那次不进 controlTransfers。
//     **不建模**生产侧对"IN 方向 + 非空 data"的拒绝（取值约定见 mtk_brom.h）—— 该形的调用在 mock 里
//     照常记录并返回 true，只有真机通道才报错；依赖它的用例等于没测到那层守卫。
//
// 注入开关：failOpen 管打开失败（openError 为报错文案）；failWriteAt 是**一次性**写失败
// （0 基，按**含失败在内**的 write() 调用序计；失败的那次不进 writes/writeFrames），-1 = 不注入，
// writeCalls = 已发生的调用数（含失败）。与 mock_eub_transport.h 的 failWriteAt（按**成功**数计、
// 且粘性）不同：这边是一次性的，后续写入照常成功 —— "写坏一次就够"的用例不需要复位动作。
class MockUsbChannel : public mtkbrom::IBromUsb
{
public:
    QByteArray writes;              // 全部写入字节（拼接；既有断言用）
    QList<QByteArray> writeFrames;  // 逐笔（每次 write() 一笔，**含空包**）—— 帧级断言用
    QList<QByteArray> reads;        // 按序弹出的读取响应；空队列 / 空条目 → read 返回 false
    QList<int> readRequests;        // 每次 read() 的请求长度（maxLen）—— 读侧可观测：欠读/超给
                                    // 由用例拿请求长度与实际给出的字节对照（裸 read 无长度校验）
    bool failOpen = false;
    QString openError;
    int pktSize = 0x400;
    int failWriteAt = -1;
    int writeCalls = 0;
    // 读注入：第 N 次 read() **返回 true 但 0 字节**（0 基，按 read() 调用序计；-1 = 不注入，一次性）。
    // 默认语义下这不 reachable（非 0 maxLen 收空条目 → false），但真机通道可能"成功但没给数据"，
    // 而 `emmcRead` 那类"按已收字节推进"的循环会因此**原地打转**—— 没有这个开关那条路径测不到。
    // 注入的那次**不消耗** reads 队列（方便用例断言"队列没被动过"）。readCalls = 已发生的调用数。
    int readEmptySuccessAt = -1;
    int readCalls = 0;
    // 控制传输：一笔请求一条记录，按 controlTransfer() 调用序追加（裸字段，不设 getter）
    struct ControlTransfer {
        quint8 requestType = 0;
        quint8 request = 0;
        quint16 value = 0;
        quint16 index = 0;
        QByteArray data;   // 空 = 无数据阶段；非空 = 输出阶段（与生产实现同一约定，见 mtk_brom.h）
        int timeoutMs = 0;
    };
    QList<ControlTransfer> controlTransfers;
    // 一次性控制传输失败（0 基，按**含失败在内**的 controlTransfer() 调用序计；失败的那次不进
    // controlTransfers），-1 = 不注入。风格同 failWriteAt。controlCalls = 已发生的调用数（含失败）。
    int failControlAt = -1;
    int controlCalls = 0;

    bool open(QString *error) override
    {
        if (failOpen) {
            if (error) *error = openError;
            return false;
        }
        return true;
    }
    bool write(const QByteArray &data, QString *error) override
    {
        if (writeCalls++ == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写入失败（第 %1 次 write）").arg(writeCalls);
            return false;
        }
        writes += data;
        writeFrames << data;
        return true;
    }
    bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) override
    {
        Q_UNUSED(timeoutMs)
        Q_UNUSED(error)
        readRequests << maxLen;
        if (readCalls++ == readEmptySuccessAt) { out.clear(); return true; }   // 注入：成功但 0 字节
        if (reads.isEmpty()) { out.clear(); return false; }
        const QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty() || maxLen == 0;
    }
    bool controlTransfer(quint8 requestType, quint8 request, quint16 value, quint16 index,
                         const QByteArray &data, int timeoutMs, QString *error) override
    {
        if (controlCalls++ == failControlAt) {
            if (error) *error = QStringLiteral("注入的控制传输失败（第 %1 次 controlTransfer）").arg(controlCalls);
            return false;
        }
        controlTransfers.append(ControlTransfer{requestType, request, value, index, data, timeoutMs});
        return true;
    }
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

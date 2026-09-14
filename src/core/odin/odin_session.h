// src/core/odin/odin_session.h
//
// Odin 会话编排：把 SamsungPlan（Task 4）按 Odin/Heimdall 协议写到设备上。
// 设备访问只经 IOdinTransport（odin_transport.h）—— 本文件不碰 libusb、不解析 XML、不拆包。
// 帧构造与应答判定在 odin_protocol（Task 5），计划构建在 samsung_plan（Task 4）。
#pragma once
#include <QString>
#include <functional>

#include "odin_transport.h"
#include "odin_protocol.h"
#include "samsung_plan.h"

namespace odin {

struct OdinOptions {
    bool dumpDevicePit = true;      // 读设备 PIT 并以其为准（失败 → 告警 + 回退包内 PIT）
    bool reboot = true;             // 收尾 0x67/0x01 重启（best-effort）
    bool queryDeviceType = true;    // 0x64/0x01 机型查询（best-effort，只记日志）
    int controlTimeoutMs = 10000;   // odin4-llucs/src/usb/usb_device.h:16（USB_TIMEOUT_CONTROL）
    int handshakeTimeoutMs = 1000;  // Heimdall BridgeManager.cpp:306（"ODIN" 握手超时）
};

// 进度回报。stage 取值（UI 依赖这组字符串，**不要**改名/新增而不通知 UI 层）：
//   "handshake" / "session" / "info" / "pit" / "validate" / "write" / "end" / "done"
// percent 在单次 run 内**单调不减**（用 m_writtenBytes + 条目内已写字节算，见 odin_session.cpp）。
struct OdinProgress { QString stage; QString detail; int percent = 0; };
using OdinProgressFn = std::function<void(const OdinProgress &)>;

class OdinSession
{
public:
    explicit OdinSession(IOdinTransport &transport, OdinProgressFn progress = {});

    // 完整刷写：握手 → 起会话（版本协商）→ 机型查询 → 总字节 → 读设备 PIT（best-effort）
    //           → 对账（以设备 PIT 为准）→ 逐条目写入 → 结束会话/重启。
    //
    // 失败语义：
    //   * 任何一步失败 → 立即返回 false + 中文 *error（带阶段名/分区名/已写字节）；
    //   * **不发结束会话（0x67）、不复位** —— 设备留在 Odin 模式便于重试。
    //     Heimdall 无论成败都发 EndSession（reference/heimdall/.../FlashAction.cpp:570），
    //     本实现**不采纳**：一次失败后再补一发 EndSession 会把设备踢出 Odin 模式，
    //     用户只能重新进下载模式才能重试（Phase B 的 EDL 会话同一口径）。
    //   * 收尾（0x67/0x00、0x67/0x01）失败**不判失败**，只落日志（Phase B 的 reset 同款口径）。
    //   * 句柄：**所有路径都恰好在最后 close() 一次**（包括空计划提前返回 —— 那条路径不 open，
    //     也不 close）。
    // 前置：plan 由 buildSamsungPlan 成功产出（plan.entries 非空）；失败后的 plan 不得传入。
    bool run(const SamsungPlan &plan, const OdinOptions &opt, QString *error);

private:
    // 一条已解析的待刷写项：计划条目 + （可选）设备 PIT 覆盖后的落点信息。
    struct ResolvedEntry {
        QString partition; QString imageFile; QString path;
        quint64 offset = 0; quint64 size = 0; PitEntry pit;
    };

    // 设备 PIT 读取的三种结局（审查修复：**中途**失败不得回退包内 PIT）。
    // 分界线 = 第一笔 `framePitPartRequest` 是否已经写出：设备只在该请求之后才进入数据流，
    // 而一旦进入，主机就无法知道它是否已经把余下的片发进 IN 端点 —— 此时改道包内 PIT 继续刷写，
    // 会把设备留在"以为还在传 PIT"的半开会话里（比直接失败更糟：用户重试时对不上状态）。
    enum class DumpOutcome {
        Ok,                 // 读全 + 解析成功
        RejectedNoStream,   // **可回退**：设备未进入数据阶段（请求失败/被拒、回报大小不合理），
                            //   或数据流已按 size 对齐消费完而只剩内容问题（长度不符/解析失败）
                            //   —— 这两种情况端点里都没有"设备还在发"的字节。
        StreamInterrupted   // **不可回退**：已进入数据阶段后中断（分片请求写失败/分片读空/
                            //   结束请求或结束应答失败）→ run() 直接失败，错误文案写明需重试。
    };

    bool handshake(QString *error);
    bool beginSession(QString *error);
    bool readAck(quint32 expectedId, bool allowProgressCodes, const QString &context, QString *error);
    void queryDeviceType();
    bool setTotalBytes(quint64 total, QString *error);
    DumpOutcome dumpDevicePit(PitTable &out, QString *error);
    bool resolveEntries(const SamsungPlan &plan, const PitTable *devicePit,
                        QList<ResolvedEntry> &out, QString *error);
    bool writeEntry(const ResolvedEntry &r, QString *error);
    void endSession(bool reboot);
    void report(const QString &stage, const QString &detail, int percent);
    int  percentFor(quint64 writtenBytes) const;

    IOdinTransport &m_t;
    OdinProgressFn  m_progress;
    OdinOptions     m_opt;
    TransferProfile m_profile;
    quint64         m_totalBytes = 0;      // 进度分母（plan.totalBytes）
    quint64         m_writtenBytes = 0;    // 已完成条目的字节数（条目内进度另加 sent）
};

} // namespace odin

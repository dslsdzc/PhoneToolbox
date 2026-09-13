// src/core/edl/edl_session.h
//
// EDL 刷写会话编排 + 数据面（spec §4 执行流）。纯逻辑：无 QObject、不依赖 libusb ——
// 只通过 `IEdlTransport&` 碰设备（依赖方向见 edl_transport.h）。
//
// 分层：sahara（载 programmer）/ firehose（命令构造 + 响应解析 + configure）/ flash_plan
// （计划模型 + 校验）都在各自文件里；本文件只负责**顺序、时机与字节**：什么时候发、发多少、
// 何时等 ACK、失败时怎么收手。数据面的每一条不变量都有参照出处（见 edl_session.cpp 顶部）。
#pragma once
#include <QByteArray>
#include <QString>
#include <QList>
#include <functional>
#include "edl_transport.h"
#include "flash_plan.h"

namespace edl {

struct FlashOptions {
    bool verifyAfterWrite = true;      // 边写边算 sha256，写完比对（不符 → 错误，不回滚）
    bool fullVerifyBeforeWrite = false;// 刷前完整校验（GB 级慢，默认关）
    // Sahara→Firehose 重枚举的**总预算**（传给 IEdlTransport::waitReenumerate）。
    // 默认 45000 沿用既有实现的量级：重写前的 edl_handler.cpp（提交 515cc57 的
    // `connectSahara()` 段）是 `msleep(3000)` 初等 + `retries=15` 次重试、每次再 `msleep(2000)`
    // ⇒ 上限 33000 ms，45000 是在其之上取整留的余量（**不是**精确等值）。改这个数之前先用真机
    // 复核：预算偏短会让重枚举慢的机型直接失败在 Firehose 门口。轮询间隔由传输实现决定；
    // 做成显式旋钮是为了让"预算"可配置、可测，而不是散落的魔数。
    int reenumerateTimeoutMs = 45000;
};

// 进度上报。`stage` 取值（Task 8 的 UI 依赖这组字符串）：
//   "sahara" / "reenumerate" / "configure" / "getstorageinfo" / "validate" /
//   "write" / "bootable" / "reset" / "done" / "read"
// `percent`：写入路径 = 已推送字节 / plan.totalBytes（**最后必然到 100**）；读回路径 = 已完成请求数 / 总数。
// `detail`：条目名（"write"）、LUN（"getstorageinfo"）等人类可读上下文。
struct SessionProgress { QString stage; QString detail; int percent = 0; };
using ProgressFn = std::function<void(const SessionProgress &)>;

// 读回请求（备份/校验）
struct ReadRequest { quint32 lun = 0; quint64 startSector = 0; quint64 numSectors = 0;
                     quint32 sectorSize = 4096; QString outputPath; };

class EdlSession
{
public:
    explicit EdlSession(IEdlTransport &transport, ProgressFn progress = {});

    // 完整刷写：Sahara(载 programmer) → 等重枚举 → configure → getstorageinfo → validatePlan
    //           → 逐条目执行 → setbootablestoragedrive（若计划含 xbl/sbl1）→ reset
    // 失败语义（spec §4 表）：任何一步失败 → 立即返回 false + 中文 *error（带阶段名/条目名）
    // + **不发 reset**（设备留在 EDL 便于重试）；reset 只在全部成功的路径发一次。
    // 设备句柄由本函数自己 open/close（Sahara 阶段结束会 close 一次：设备随即重枚举）。
    bool run(const FlashPlan &plan, const QByteArray &programmer,
             const FlashOptions &opt, QString *error);

    // 进入 Firehose 会话：configure 协商（= run() 的第 ③ 步，Phase B Task 7 拆出为公开入口）。
    // 前置：设备已在 Firehose 且传输句柄可用（本函数不做 Sahara、不重枚举）。
    // `memoryName` 入参 = 首选存储类型（"emmc"/"ufs"），出参 = **实际生效**的类型（NAK "Not support
    // configure MemoryName" 时 firehoseConfigure 会换型重试一次，见 firehose.h）；
    // `maxPayloadBytes` 出参 = 设备回报的载荷上限（**0 = 设备未回报**，此时会话按保守默认分块）。
    // 协商结果留在会话内（m_maxPayload），供 writePlan 的数据面分块 —— 同一次会话不重复 configure。
    // 失败语义：false + 中文 *error（**不**关句柄、**不**发 reset —— 收手由调用方决定）。
    bool beginFirehose(QString &memoryName, quint32 &maxPayloadBytes, QString *error);

    // 已在 Firehose 会话内的写入路径（Phase B Task 7 公开入口：run() 与 EDLHandler 共用）。
    // 前置：设备已在 Firehose；configure 由 beginFirehose 完成（未协商过时按保守默认分块）。
    // 内部：getstorageinfo（逐计划 LUN）→ validatePlan（不过则拒刷）→ 逐条目 Program/Erase/Patch
    //       → setbootablestoragedrive（计划含 xbl/sbl1 时）。
    // **不**发 reset、**不**做 Sahara/重枚举 —— reset 只属 run() 的成功路径
    // （既有 UI 流程"edlConnect 后停在 Firehose"走的正是本函数，见 Task 8 的 oppo-edl 通道）。
    // 句柄：成功路径保持占用（调用方接着用）；**每条走到设备侧的失败路径都会 close()**
    // （edl_session.cpp 的 writePlan：getstorageinfo ×3 / validatePlan / 条目写入 / setbootable ×2
    // 各处 return 前都 close；唯一例外是"空计划"——它在任何设备命令之前就拒了，不碰句柄）。
    // 之所以这么选：走到这里的失败多半意味着设备已掉线或状态可疑，留着旧句柄只会把错误推迟到
    // 下一次调用才爆；代价是**调用方必须同步自己的连接记账**（EDLHandler 就把 m_connected /
    // m_configured 置 false），需要继续操作时自行重连。
    // 失败语义：false + 中文 *error（带阶段名/条目名），设备留在 EDL 便于重试（句柄需重开）。
    bool writePlan(const FlashPlan &plan, const FlashOptions &opt, QString *error);

    // 只读回（不写入、不复位）。
    // ⚠️ 前置条件：设备**已在 Firehose 模式**且传输句柄可用 —— 本函数没有 programmer 参数，
    //    无法自行做 Sahara 引导（那是 run() 的职责）。`device` 提供逐 LUN 几何，用于**发送前**
    //    的范围校验（越界/未知 LUN 一律拒，一条命令都不发）。回读数据写到 outputPath；
    //    中途失败会删掉写了一半的文件（半截备份比没有更危险）。
    bool readBack(const QList<ReadRequest> &reqs, const QList<StorageInfo> &device, QString *error);

private:
    bool writeEntry(const PlanEntry &e, const FlashOptions &opt, QString *error);   // 单条目：XML + 数据面 + 等 ACK
    int  percentFor(quint64 writtenBytes) const;   // 进度百分比（分母 = m_totalBytes）
    IEdlTransport &m_t;
    ProgressFn     m_progress;
    quint32        m_maxPayload = 0;
    quint64        m_writtenBytes = 0;   // 进度分子累计（已推送到设备的数据字节，含补零）
    quint64        m_totalBytes = 0;     // 进度分母（= plan.totalBytes；0 → 视作 100）
};

} // namespace edl

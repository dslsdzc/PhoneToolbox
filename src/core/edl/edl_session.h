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

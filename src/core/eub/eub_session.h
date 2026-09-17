// src/core/eub/eub_session.h
//
// EUB 救援编排：识别 → 查表 → 切段 → 逐段(重开设备 → 发送 →（可选）读回显) → 收尾。
// 设备访问只经 IEubTransport —— 本文件不碰 libusb、不构造帧（帧在 eub_protocol）、不切段
// （切段在 eub_loadout）。
//
// 段间"重开设备"是参照的实测路径（facts §B8：shell 脚本每段都是一次全新的 exynos-usbdl 调用 +
// sleep 1；cfg 注释写作 "Wait Re-Enumeration"）。关闭再打开天然容忍设备重枚举与地址变化
// （facts §B9 明说重连会失败，故本层带**重试**，比参照脚本更稳）。
//
// notes（传输层的非致命说明，如"端点回退到常数 0x02/0x81"）的作用域是**单次 open**：传输层在
// 每次 open 开头清空、close 不清（eub_libusb_transport.cpp:153）。故本层按**批次变化**转发
// （见 forwardNotes），**不是**"一次会话只转一次" —— 会话级"只转一次"标志会让 identify 之后的
// run 里 N 次 open 的说明全部静默丢弃，而真机线索（端点回退）往往正是那时才出现。
//
// ⚠️ 真机路径未验证：本机没有任何 Exynos 设备（facts §F1；真样本已就位、见 §H），本层只到"按 spec 编排 + mock 覆盖"
// 这一层证据；段间时序、重枚举窗口、回显内容留持机人。
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#include "eub_loadout.h"
#include "eub_transport.h"

namespace eub {

// 段后读回显的超时（ms）：facts §B2 + hubble.py:115（`timeout=50`，回显只读一次）。
// 与真机传输层的 LibusbEubTransport::kReadTimeoutMs 同值 —— 本层**故意不 include**传输实现头
// （本目标的构建图里没有 libusb 源，见 CMakeLists 的 test_eub_session 分支），故会话内自带一份
// 常量；改这个数要连 eub_libusb_transport.h 一起改。
constexpr int kReadEchoTimeoutMs = 50;

// 回显最多读的字节数（spec §6 时序 / §7：readBulk(512, 50ms)）
constexpr int kReadEchoMaxBytes = 512;

struct EubOptions {
    int  segmentGapMs     = 1000;   // 段间基础等待（参照脚本用 sleep 1，facts §B8）
    int  revolveAttempts  = 20;     // open 重试次数；总上限 ≈ revolveAttempts × revolvePollMs
    int  revolvePollMs    = 500;    // 重试间隔
    bool readResponse     = true;   // responseSupport 的表项：每段后读一次回显（facts §C7/§C8）
    // 注入用：空 = QThread::msleep。用例注入空实现 → 重试/等待逻辑**不依赖真实时间**
    // （超时用**次数**而非墙钟，见 revolveAttempts）。等待时长为 0 时**不调用**（无意义的睡眠）。
    std::function<void(int)> sleepFn;
};

// stage 取值（UI 依赖这组字符串，不要改名/新增而不通知 UI 层）：
//   "identify" / "send" / "done"
struct EubProgress { QString stage; QString detail; int percent = 0; };
using EubProgressFn = std::function<void(const EubProgress &)>;

class EubSession
{
public:
    explicit EubSession(IEubTransport &t, const EubOptions &opt = {}, EubProgressFn progress = {});

    // 打开设备 → 读自述 → 查表 → 关闭（不持有句柄：设备可能瞬态消失，facts §A6）。
    // SoC 名为空（极老 SoC 只报 "SEC S5PC210 Test B/D"，facts §A4）→ 失败，
    // 由调用方用 detectSocFromImage(sboot) 兜底后自行查表。
    // 成功时 out 填好表项；**失败时 out 被清空**（fail-closed：入口先清，见 .cpp —— 调用方忽略
    // 返回值也不会拿到上一次的表项）。
    bool identify(EubLoadout &out, QString *error);

    // 完整救援（**不带额外文件**的形态）：等价于 extras 传空表，只对不带 extraFiles 的表项成立
    // —— 带该字段的表项（9830）会因数量不符在入口 fail-closed（见下面那个重载）。
    bool run(const EubLoadout &lo, const QByteArray &sboot, QString *error);

    // 完整救援：入口校验 → 切段（失败即中止，**不写任何字节**）→ 逐段 open(带重试) → 发送 →
    // 可选读回显 → close →（表项带 extraFiles 时）额外文件阶段。
    // 入口校验（**在切段与碰设备之前**）：extras 的数量必须与该表的 lo.extraFiles 相等，且每个载荷
    // 非空 —— 不符即失败、零字节。理由是"半完成状态比什么都没发更糟"：若等到段都发完才发现文件
    // 没备齐，设备会被留在"分段已发、文件未发"的状态（终审 I1 的同一顾虑）。
    // 第 1 段发送前核对设备**身份**：拿设备当前自报的 SoC 名与 **identify() 时自报的那个**比
    //（防"识别与开始之间换了设备"）—— 基准是设备自己的历史自述，**不是** lo.soc：兜底路径
    //（设备不自报 / 自报的名字无表时按镜像反推选表，facts §A4）产出的 lo.soc 与设备自报串
    // 永不相等，拿 lo.soc 当基准会让"预检能过、点开始必被拒"。本次会话未先 identify()（run 是
    // 公开 API，可单独调用）→ 无基准可比，**不阻断**，只落一条日志。
    // 失败文案含段序号/段名/偏移长度、额外文件的序号/名字/字节数；**不支持从中间续传**（引导链必须
    // 从第一段起，spec §7）。
    // 额外文件阶段（段全部发完后）：按 lo.extraFiles 的顺序逐个
    // open(带重试) → 发送（**与分段同款的一帧**：hubble 的 extra 与分段共用 send_part_to_device，
    // facts §C7）→（该表 responseSupport 且选项开启时）读一次回显 → close → 与段间同款的等待。
    // extras[i] 与 lo.extraFiles[i] 一一对应（调用方用 loadNamedEntriesFromTar 按同一份名单从
    // BL 包里取，见 eub_payload.h）。
    // **与参照的有意分歧**（facts §B8 记录的两种做法）：hubble 在分段后**不重开**设备（同一句柄
    // 连发，hubble.py:310-341）；本仓选"每文件重开"——与本仓分段阶段一致，且容忍段间重枚举
    // （facts §B9）。设备身份核对仍只在第 1 段做，extra 阶段不重做。
    // 进度沿用 "send" stage（percent 把 extra 计入分母：段发完不再直接到 100），detail 点明是额外文件。
    // 证据等级**单源**（hubble，只有 9830 有该字段）：本仓无真机；真样本已就位（facts §H），"9830 需要这两个文件"
    // 是参照流程的要求，不是本仓的实测结论。
    bool run(const EubLoadout &lo, const QByteArray &sboot,
             const QList<QByteArray> &extras, QString *error);

private:
    // 最多 m_opt.revolveAttempts 次；每次失败后（除最后一次）等待 m_opt.revolvePollMs。
    // 失败时 error 保留**最后一次**的原因。
    bool openWithRetry(QString *error);
    // 把 m_t.notes()（端点回退等非致命说明）转进进度回调。notes 是**每次 open** 级的：
    // 传输层每次 open 清空、close 不清 —— 故本函数按**批次变化**转发：与上次相同的批次不重复
    // 刷（run 有 N 段 ＝ N 次 open，重复刷会把日志淹掉），批次一变（如 run 阶段才出现的
    // "已补设配置 1"、或换了端点）立即转出，不漏线索。
    void forwardNotes();
    void sleepMs(int ms) const;
    void report(const QString &stage, const QString &detail, int percent);
    // 段后回显的 best-effort 读取（facts §C7/§C8）：只在该表 responseSupport 且选项开启时读一次，
    // **读不到不判失败**（spec §7）。返回可直接拼进日志的备注（"；回显：…"/"；未读到回显（不判失败）"
    // /"；回显读取失败（不判失败）：…"）。段与额外文件两个阶段共用（同一份行为，不再抄第二遍）。
    QString echoNoteText(const EubLoadout &lo);

    IEubTransport &m_t;
    EubOptions     m_opt;
    EubProgressFn  m_progress;
    int  m_lastPercent = 0;         // 最近一次 report 的 percent（notes 转发沿用它保持单调）
    QStringList m_lastNotes;        // 上次已转出的 notes 批次（笔记是**每次 open** 级的：传输层每次 open 清空）

    // identify() 读到的设备自述 SoC 名，作为 run() 换设备守卫的基准（见 run 的头注释）。
    // **查表之前**就记录（见 .cpp）：兜底路径正是"已读过设备自述、但查表失败"，只有在查表前
    // 落下这一笔，run 阶段才有可比的设备自报值。
    QString m_identifySocName;
    bool    m_haveIdentifyName = false;
    // identify() 是否**被调用过**（无论是否读到自述）：run 的"无基准"日志要分清
    // "压根没识别"与"识别了但读不到设备自述"——两者成因不同，混为一谈会把用户带偏。
    bool    m_identifyAttempted = false;
};

} // namespace eub

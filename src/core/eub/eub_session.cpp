// src/core/eub/eub_session.cpp
//
// 编排本体。步骤与文案的出处：
//   * 段序/切段 → eub_loadout（表来自 spec §5.4）
//   * 段间"重开设备 + 等待" → facts §B8①（shell 脚本路径：每段一次全新的 exynos-usbdl 调用 +
//     sleep 1）；重枚举会失败（facts §B9）→ 本层用**次数**重试而不是照抄脚本的"碰运气"
//   * 失败语义（含"不支持从中间续传"）→ spec §7
//   * 段后的额外文件（9830 的 ldfw.img/tzsw.img）→ reference/hubble/hubble.py:329-341 +
//     ExynosData/Exynos9830.json:3（facts §C7）——**证据等级单源**，且只是"参照流程的要求"，
//     本仓无真机；真样本已下载（reference/eub-samples/，facts §H）并由 Task 1b 的 gated 用例实跑核对
// 本文件不碰 libusb、不构造帧、不切段 —— 只按表驱动 IEubTransport（依赖契约见 CMakeLists 的
// test_eub_session 分支：该目标不含任何 libusb 源）。
//
// ⚠️ 真机路径未验证（facts §F1）：本层只到"按 spec 编排 + mock 全覆盖"这一层证据。
#include "eub_session.h"

#include <QThread>
#include <utility>

namespace eub {
namespace {
void setErr(QString *error, const QString &msg) { if (error) *error = msg; }
} // namespace

EubSession::EubSession(IEubTransport &t, const EubOptions &opt, EubProgressFn progress)
    : m_t(t), m_opt(opt), m_progress(std::move(progress))
{
}

void EubSession::report(const QString &stage, const QString &detail, int percent)
{
    m_lastPercent = percent;
    if (m_progress)
        m_progress({stage, detail, percent});
}

void EubSession::forwardNotes()
{
    const QStringList notes = m_t.notes();
    // 空 = 本次打开没有说明：不动 m_lastNotes，后续 open 若产生说明仍能转出。
    // 与上次**相同**的批次也不再转（run 有 N 段 ＝ N 次 open，重复刷只会淹日志）。
    // 用"批次是否变化"而不是会话级"只转一次"：notes 的作用域是**单次 open**（传输层每次
    // open 清空、close 不清，见 eub_libusb_transport.cpp:153）—— 会话级标志会让 identify
    // 之后的 run 里出现的说明（端点回退 / 已补设配置 1）全部静默丢弃，丢的正是笔记存在的理由。
    if (notes.isEmpty() || notes == m_lastNotes)
        return;
    m_lastNotes = notes;        // 记的是"已转出过的批次"：与上面转发的内容保持同一份文本
    for (const QString &n : notes)
        report(QStringLiteral("identify"), QStringLiteral("设备说明：%1").arg(n), m_lastPercent);
}

void EubSession::sleepMs(int ms) const
{
    if (ms <= 0)
        return;                 // 无意义的等待不落进注入回调（用例的 0 值配置不留噪声）
    if (m_opt.sleepFn)
        m_opt.sleepFn(ms);
    else
        QThread::msleep(static_cast<unsigned long>(ms));
}

bool EubSession::openWithRetry(QString *error)
{
    QString lastErr;
    for (int i = 0; i < m_opt.revolveAttempts; ++i) {
        if (m_t.open(&lastErr))
            return true;
        if (i + 1 < m_opt.revolveAttempts)
            sleepMs(m_opt.revolvePollMs);   // 最后一次失败后不再等
    }
    setErr(error, QStringLiteral("设备在 %1 次尝试内未出现（每次间隔 %2 ms）：%3 —— "
                                 "设备可能已进入 Download 模式（请检查）或需要重新进入 EUB")
                      .arg(m_opt.revolveAttempts).arg(m_opt.revolvePollMs)
                      .arg(lastErr.isEmpty() ? QStringLiteral("（原因未知）") : lastErr));
    return false;
}

bool EubSession::identify(EubLoadout &out, QString *error)
{
    m_lastPercent = 0;
    // fail-closed：入口先清出参 —— 打开失败/自述读失败/SoC 名为空这三条早退路径若不在这里清，
    // 调用方忽略返回值时会拿到**上一次**的表项，把"识别失败"当成"识别成功"直接进 run
    //（eubLoadoutFor 只在它自己失败时清，见 eub_loadout.h:44-46）。
    out = EubLoadout{};
    m_identifyAttempted = true;      // 供 run 区分"没识别"与"识别了但读不到自述"（见 .h）
    QString err;
    if (!openWithRetry(&err)) {
        // 打开失败的现场说明同样要转（传输层的 close 不清 notes，见其注释）：真机上
        // "回退了固定端点" 这类线索常常正是失败的上下文。
        forwardNotes();
        setErr(error, err);
        return false;
    }
    forwardNotes();

    EubDeviceInfo info;
    const bool haveInfo = m_t.readDeviceInfo(info, &err);
    // 先关句柄再查表：identify 结束**不持有句柄**（设备可能瞬态消失，facts §A6），
    // 且查表失败这条路径也不能漏关（故 close 在两个早退之前）。
    m_t.close();

    if (!haveInfo) {
        setErr(error, QStringLiteral("读取设备自述失败：%1").arg(err));
        return false;
    }
    // 换设备守卫的基准值：**必须在查表之前**记录 —— 兜底流程正是"已读过设备自述、但查表失败"
    //（SoC 名为空 facts §A4 / 名字无表），跑到查表之后就永远记不下这一笔，run 里的守卫会退化
    // 成"本次会话没识别过"而不再核对设备身份。
    m_identifySocName = info.socName;
    m_haveIdentifyName = true;
    if (info.socName.isEmpty()) {
        setErr(error, QStringLiteral(
            "设备未自报 SoC 名（iProduct 为空）。极老 SoC 只报 \"SEC S5PC210 Test B/D\"（facts §A4）——"
            "请先用 detectSocFromImage(sboot) 从镜像反推 SoC 名，再自行查表。"));
        return false;
    }
    if (!eubLoadoutFor(info.socName, out, &err)) {
        setErr(error, err);     // eubLoadoutFor 的文案已列出支持的 SoC，并已清空 out（fail-closed）
        return false;
    }
    report(QStringLiteral("identify"),
           QStringLiteral("已识别 %1：布局表 %2 段（证据：%3）")
               .arg(info.socName).arg(out.segments.size()).arg(out.evidence),
           100);
    return true;
}

// 不带额外文件的形态：等价于 extras 传空表。带 extraFiles 的表项走这里会因数量不符 fail-closed
//（见下个重载的入口校验）—— 这正是旧契约调用者（run(lo, sboot, &err)）该有的行为。
bool EubSession::run(const EubLoadout &lo, const QByteArray &sboot, QString *error)
{
    return run(lo, sboot, {}, error);
}

bool EubSession::run(const EubLoadout &lo, const QByteArray &sboot,
                     const QList<QByteArray> &extras, QString *error)
{
    m_lastPercent = 0;          // 本次 run 的进度锚点：新操作从头计

    // ⓪ fail-closed（入口，**在切段与碰设备之前**）：带 extraFiles 的表项（9830）在参照流程里要求
    //"分段发完后**另发**BL 包内的文件"（hubble.py:329-341 + ExynosData/Exynos9830.json:3，facts §C7，
    // 证据等级**单源**），调用方必须把载荷按同一份名单备齐（见 eub_payload.h 的
    // loadNamedEntriesFromTar）。数量不符 → 直接拒，一个字节都不写，也不碰设备（句柄尚未打开）。
    // 放在切段之前是刻意的：若挪到"段都发完之后"再发现缺文件，设备会被留在"分段已发、文件未发"的
    // 半完成状态 —— 宁可让用户看到"文件没备齐"，也不给错误的完成保证。
    if (lo.extraFiles.size() != extras.size()) {
        if (lo.extraFiles.isEmpty())
            setErr(error, QStringLiteral(
                "布局表 \"%1\" 没有额外文件（参照流程不要求段后另发），本次却提供了 %2 个载荷"
                "—— 拒绝执行（不发送任何字节）").arg(lo.soc).arg(extras.size()));
        else
            setErr(error, QStringLiteral(
                "该 SoC（%1）的参照流程要求在分段之后另发 %2 个文件（%3），本次提供了 %4 个"
                "—— 拒绝执行（不发送任何字节）")
                          .arg(lo.soc)
                          .arg(lo.extraFiles.size())
                          .arg(lo.extraFiles.join(QStringLiteral("、")))
                          .arg(extras.size()));
        return false;
    }
    // 空载荷的 extra 同样在这里拒掉：若拖到发送阶段，段已经发完了 —— 又是半完成状态。
    // （loadNamedEntriesFromTar 不会产出空载荷；但 run 是公开 API，调用方可以直接传字节。）
    for (int i = 0; i < extras.size(); ++i) {
        if (extras[i].isEmpty()) {
            setErr(error, QStringLiteral("额外文件 %1/共 %2「%3」的载荷是空的（0 字节）"
                                         "—— 拒绝执行（不发送任何字节）")
                              .arg(i + 1).arg(extras.size()).arg(lo.extraFiles[i]));
            return false;
        }
    }

    // ① 先切段：失败即中止，**一个字节都不写**，也**不碰设备**（句柄尚未打开）
    QList<QPair<QString, QByteArray>> parts;
    QString err;
    if (!splitSboot(sboot, lo, parts, &err)) {
        setErr(error, err);
        return false;
    }
    if (parts.isEmpty()) {
        // 防御（fail-closed）：空表会"发 0 段然后报成功"，用户以为刷过了 —— 对救援工具是最坏的
        // 假成功。8 张内置表都非空（tests/test_eub_loadout.cpp 钉住），但 run 是公开 API，
        // 表可以由调用方自造。
        setErr(error, QStringLiteral("布局表 \"%1\" 没有任何段：拒绝执行（不发送任何字节）").arg(lo.soc));
        return false;
    }

    const int n = int(lo.segments.size());
    const int m = int(lo.extraFiles.size());
    // 进度分母把额外文件也算进去：否则段一发完进度就到 100%（用户以为完事了，extra 还在写）。
    // m == 0 时与旧行为逐值相同（不带 extraFiles 的表项：total == n）。
    const int total = n + m;
    for (int i = 0; i < n; ++i) {
        const EubSegment &seg = lo.segments[i];

        // ② 每段都重新打开（段间设备可能重枚举，facts §B8/§B9）；失败按次数重试
        if (!openWithRetry(&err)) {
            // open 失败的现场说明同样要转（与 identify 的打开失败分支同因）：传输层的 close
            // 不清 notes，真机上"回退了固定端点"这类线索常常正是失败的上下文 —— 不打日志就只剩一句
            // "设备在 N 次尝试内未出现"，用户无从判断是没插好还是端点不对。
            forwardNotes();
            setErr(error, QStringLiteral("第 %1 段 \"%2\" 发送前打开设备失败：%3")
                              .arg(i + 1).arg(seg.name).arg(err));
            return false;       // open 失败不留句柄（IEubTransport 契约），无需 close
        }
        forwardNotes();

        // ③ 仅第 1 段核对设备**身份**：识别不持有句柄（见 identify），用户可能在两步之间换了设备。
        // 基准是 identify() 记下的**设备自报**串，不是 lo.soc —— 兜底路径（设备不自报 / 名字无表，
        // 由调用方按镜像反推选表，facts §A4）下 lo.soc 与设备自报串永不相等，拿它当基准等于
        // "预检能过、点开始必被拒"。把占位串白名单塞进本层属跨层硬编码，故不采纳。
        if (i == 0) {
            EubDeviceInfo info;
            if (!m_t.readDeviceInfo(info, &err)) {
                m_t.close();
                setErr(error, QStringLiteral("第 1 段发送前读取设备自述失败：%1").arg(err));
                return false;
            }
            QString idNote;             // 追加到下面那行日志尾部（保持"开始发送"这条锚点只有一行）
            if (!m_haveIdentifyName) {
                // 没有可比基准 → 不阻断（run 是公开 API，可单独调用）。成因要分开说：
                // "压根没识别"是调用方顺序，"识别了但读不到设备自述"是设备/连接问题 ——
                // 混成一句会把后者说成前者，用户会去查自己的操作顺序（T7 复审意见）。
                idNote = m_identifyAttempted
                    ? QStringLiteral("；识别时未读到设备自述（无基准），跳过换设备核对")
                    : QStringLiteral("；未先识别设备，跳过换设备核对");
            } else if (info.socName.compare(m_identifySocName, Qt::CaseInsensitive) != 0) {
                m_t.close();
                setErr(error, QStringLiteral(
                    "设备已更换：识别时自报 %1，现在自报 %2 —— 请等设备稳定后重新识别，再从头开始")
                              .arg(m_identifySocName.isEmpty() ? QStringLiteral("（空）") : m_identifySocName,
                                   info.socName.isEmpty() ? QStringLiteral("（空）") : info.socName));
                return false;
            } else if (!info.socName.isEmpty()
                       && info.socName.compare(lo.soc, Qt::CaseInsensitive) != 0) {
                // 身份没变，但设备自报名与所用布局表不同名 —— 兜底路径的常态（表是镜像反推来的）。
                // 不阻断（挡在这里等于把兜底路径又堵死），但要在日志里点明"表可能不匹配本机"。
                idNote = QStringLiteral("；注意：设备自报 %1 与布局表 %2 不一致，表可能不匹配本机")
                             .arg(info.socName, lo.soc);
            } else if (info.socName.isEmpty()) {
                // 两边都是空串：比较形式上成立、实质上**无可比身份**（facts §A4 的极老 SoC）。
                // 不点明这一句，对话框那句"发送前仍会核对设备身份"就是空话（T7 复审意见）。
                idNote = QStringLiteral("；设备未自报 SoC 名，无法核对设备身份");
            }
            report(QStringLiteral("identify"),
                   QStringLiteral("设备自报 %1，开始发送 %2 段%3")
                       .arg(info.socName.isEmpty() ? QStringLiteral("（空）") : info.socName)
                       .arg(n).arg(idNote),
                   m_lastPercent);
        }

        // ④ 一段一帧（facts §B7：整帧一次写；不重发、不续传）
        if (!sendSegment(m_t, parts[i].second, lo.style, &err)) {
            m_t.close();        // 失败也要收干净句柄
            // 文案格式固定（spec §7；序号**从 1 起**）：测试按 "第 2 段" 断言
            setErr(error, QStringLiteral("第 %1 段 \"%2\"（0x%3/0x%4）写入失败：%5")
                              .arg(i + 1).arg(seg.name)
                              .arg(seg.offset, 0, 16).arg(seg.length, 0, 16)
                              .arg(err));
            return false;       // 引导链必须从第一段起（spec §7：不支持从中间续传）
        }

        // ⑤ 回显 best-effort（facts §C7/§C8）：落日志，**读不到不判失败**（spec §7）
        const QString echoNote = echoNoteText(lo);

        m_t.close();
        report(QStringLiteral("send"),
               QStringLiteral("第 %1/%2 段 \"%3\" 已发送（0x%4/0x%5）%6")
                   .arg(i + 1).arg(n).arg(seg.name)
                   .arg(seg.offset, 0, 16).arg(seg.length, 0, 16).arg(echoNote),
               int((i + 1) * 100 / total));

        // ⑥ 段间等待（facts §B8 的 sleep 1，等设备重枚举）；**最后一段后不睡**
        if (i + 1 < n)
            sleepMs(m_opt.segmentGapMs);
    }

    // ⑦ 额外文件阶段（表项带 extraFiles 时；目前只有 9830）：参照流程要求在**段全部发完之后**按
    // files_to_send 的顺序另发 BL 包内的文件（hubble.py:329-341，facts §C7；证据等级**单源**）。
    // 每文件都是本仓分段阶段的同款一轮：open(带重试) → 发送（同 lo.style 的一帧）→（可选）读回显
    // → close → 段间同款等待。
    // **与参照的有意分歧**（facts §B8 记录的两种做法）：hubble 在分段后**不重开**设备（同一句柄连发）；
    // 本仓选"每文件重开"——与自己的分段阶段一致，且容忍段间重枚举（facts §B9）。
    // 设备身份核对不在这里重做（参照也不做；真要换设备，第 1 段的核对或 open 失败会先拦下）。
    for (int j = 0; j < m; ++j) {
        const QString &name = lo.extraFiles[j];
        // 值拷贝而非引用（T1 审查 M2）：`extras[j]` 越界是 UB，而本层的内存安全**只**靠上面那条
        // 数量校验。`value(j)` 越界得空载荷 → 走下面既有的"空载荷拒发"路径 —— 失败形态从
        // 进程崩溃（变异 S1 实测为 SEGV_MAPERR）变成一条可归因的断言失败。
        const QByteArray data = extras.value(j);

        if (!openWithRetry(&err)) {
            forwardNotes();     // 与段阶段的 open 失败同因：传输层的现场说明是唯一线索
            setErr(error, QStringLiteral("额外文件 %1/共 %2「%3」发送前打开设备失败：%4")
                              .arg(j + 1).arg(m).arg(name).arg(err));
            return false;       // open 失败不留句柄（IEubTransport 契约），无需 close
        }
        forwardNotes();

        // 一段一帧的同一函数（facts §C7：hubble 的 extra 与分段共用 send_part_to_device）
        if (!sendSegment(m_t, data, lo.style, &err)) {
            m_t.close();        // 失败也要收干净句柄
            setErr(error, QStringLiteral("额外文件 %1/共 %2「%3」（%4 字节）写入失败：%5")
                              .arg(j + 1).arg(m).arg(name).arg(data.size()).arg(err));
            return false;       // 失败即停、不续传（与段阶段同款语义）
        }

        const QString echoNote = echoNoteText(lo);
        m_t.close();
        report(QStringLiteral("send"),
               QStringLiteral("额外文件 %1/共 %2「%3」（%4 字节）已发送%5")
                   .arg(j + 1).arg(m).arg(name).arg(data.size()).arg(echoNote),
               int((n + j + 1) * 100 / total));

        // 与段间同款等待（最后一项后不睡）：等设备重枚举（facts §B8）
        if (j + 1 < m)
            sleepMs(m_opt.segmentGapMs);
    }

    if (m > 0)
        report(QStringLiteral("done"),
               QStringLiteral("全部 %1 段 + %2 个额外文件（%3）已发送：设备应已进入 Download 模式，"
                              "请继续刷写")
                   .arg(n).arg(m).arg(lo.extraFiles.join(QStringLiteral("、"))), 100);
    else
        report(QStringLiteral("done"),
               QStringLiteral("全部 %1 段已发送：设备应已进入 Download 模式，请继续刷写").arg(n), 100);
    return true;
}

QString EubSession::echoNoteText(const EubLoadout &lo)
{
    // 回显 best-effort（facts §C7/§C8）：只在该表 responseSupport 且选项开启时读一次，**读不到不判
    // 失败**（spec §7）—— 落日志即可。段与额外文件两阶段共用本函数（同一份行为，不再抄第二遍）。
    if (!lo.responseSupport || !m_opt.readResponse)
        return QString();
    QString readErr;
    const QByteArray echo = m_t.readBulk(kReadEchoMaxBytes, kReadEchoTimeoutMs, &readErr);
    if (echo.isEmpty())
        return readErr.isEmpty() ? QStringLiteral("；未读到回显（不判失败）")
                                 : QStringLiteral("；回显读取失败（不判失败）：%1").arg(readErr);
    return QStringLiteral("；回显：%1").arg(QString::fromUtf8(echo));
}

} // namespace eub

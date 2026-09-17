// src/ui/eub_recovery_dialog.cpp
//
// 救援对话框本体。分工：本文件**不做**协议/切段/查表（那些在 core/eub/*），只负责
//   ① 把设备的识别结果与布局表摊开到人眼前（含**证据等级**与**帧风格出处** —— facts §B6 要求
//      UI 说明"照抄自哪个实现"，本类承接 T3 无 UI 改动的遗留）；
//   ② 把 shai 对照讲清楚（spec §D6：不硬拒绝，但要让用户看到"两边前 8 位"）；
//   ③ 门控"开始"（未验证勾选惯例，spec §D6）与交接提示（§D10）。
//
// ⚠️ 真机路径未验证：本机无任何 Exynos 设备（facts §F1）—— 识别/发送/回显的真机行为留持机人；
// 本层只到"注入 IEubTransport 后按 spec 编排"这一层证据（用例见 tests/test_eub_recovery_dialog.cpp）。
#include "eub_recovery_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/eub/eub_payload.h"

namespace {

// 字节序列 → "1B 44 4E 57"（大写十六进制，空格分隔）。帧风格出处行用它把表里的头/尾摊开
//（facts §B4/§B5 的三实现对照表就是这种写法，用户可拿去与参照源码逐字节核）。
QString hexBytes(const QByteArray &b)
{
    QStringList parts;
    parts.reserve(b.size());
    for (unsigned char c : b)
        parts << QStringLiteral("%1").arg(c, 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(' '));
}

} // namespace

EubRecoveryDialog::EubRecoveryDialog(eub::IEubTransport &transport, QWidget *parent)
    : QDialog(parent)
    , m_transport(transport)
      // m_opt 保持默认（段间 1s / 重试 20×500ms ≈ 10s，与 spec §D4 的"默认超时 10s"一致）。
      // 本对话框不跑在 worker 线程里：长等待靠下面的 processEvents 保活（见进度回调）。
    , m_session(transport, m_opt, [this](const eub::EubProgress &p) {
          log(QStringLiteral("[%1 %2%] %3")
                  .arg(p.stage, QString::number(p.percent), p.detail));
          setWindowTitle(QStringLiteral("EUB 救援 — %1 %2%").arg(p.stage).arg(p.percent));
          // 会话在 UI 线程里同步跑：identify 最长 20×500ms、run 每段还等 1s —— 不 processEvents
          // 整窗假死（日志也刷不出来）。与 FlashPanel 刷写期间同款：只重绘、不吃用户输入
          //（flash_panel.cpp:24 的 QEventLoop::ExcludeUserInputEvents 惯例）。
          QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      })
{
    setWindowTitle(QStringLiteral("EUB 救援（Exynos USB Boot）"));
    resize(720, 640);

    QVBoxLayout *layout = new QVBoxLayout(this);

    // 诚实提示（facts §F1/§F6 + spec §11）：本流程**没有**真机验证过，布局表与帧格式都是照抄公开
    // 实现。写进界面而不是只写进注释 —— 用户据此决定"要不要拿它救砖"。
    QLabel *unverifiedNote = new QLabel(
        QStringLiteral("注意：本救援流程未在真机上验证（开发机没有 Exynos 设备，facts §F1）；"
                       "布局表与帧格式均照抄公开实现，证据等级与未定字段见下方段表。"), this);
    unverifiedNote->setObjectName(QStringLiteral("eubUnverifiedNote"));
    unverifiedNote->setWordWrap(true);
    layout->addWidget(unverifiedNote);

    m_deviceLabel = new QLabel(QStringLiteral("设备：未识别（选择 sboot 来源后自动识别）"), this);
    m_deviceLabel->setObjectName(QStringLiteral("eubDeviceLabel"));
    m_deviceLabel->setWordWrap(true);
    layout->addWidget(m_deviceLabel);

    QPushButton *pickBtn = new QPushButton(QStringLiteral("选择 sboot 来源…"), this);
    pickBtn->setObjectName(QStringLiteral("eubPickSourceButton"));
    connect(pickBtn, &QPushButton::clicked, this, &EubRecoveryDialog::onPickSource);
    layout->addWidget(pickBtn);

    m_sourceLabel = new QLabel(QStringLiteral("来源：未选择"), this);
    m_sourceLabel->setWordWrap(true);
    layout->addWidget(m_sourceLabel);

    m_shaLabel = new QLabel(QStringLiteral("sboot 修订对照：（未选择载荷）"), this);
    m_shaLabel->setWordWrap(true);
    layout->addWidget(m_shaLabel);

    m_segmentView = new QPlainTextEdit(this);
    m_segmentView->setObjectName(QStringLiteral("eubSegmentView"));
    m_segmentView->setReadOnly(true);
    m_segmentView->setPlainText(QStringLiteral("（未选择载荷：选择后这里显示按 SoC 布局表切出的分段）"));
    layout->addWidget(m_segmentView);

    m_confirmBox = new QCheckBox(
        QStringLiteral("我理解该布局表绑定特定固件修订，分段偏移可能与我的固件不符"), this);
    m_confirmBox->setObjectName(QStringLiteral("eubConfirmCheck"));
    connect(m_confirmBox, &QCheckBox::toggled, this, &EubRecoveryDialog::refreshStartEnabled);
    layout->addWidget(m_confirmBox);

    m_logView = new QPlainTextEdit(this);
    m_logView->setObjectName(QStringLiteral("eubLogView"));
    m_logView->setReadOnly(true);
    layout->addWidget(m_logView);

    QDialogButtonBox *buttons = new QDialogButtonBox(this);
    m_startBtn = buttons->addButton(QStringLiteral("开始救援"), QDialogButtonBox::AcceptRole);
    m_startBtn->setObjectName(QStringLiteral("eubStartButton"));
    connect(m_startBtn, &QPushButton::clicked, this, &EubRecoveryDialog::onStart);
    QPushButton *closeBtn = buttons->addButton(QDialogButtonBox::Close);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttons);

    refreshStartEnabled();      // 初始关闸（未预检 + 未勾选）
    log(QStringLiteral("EUB 救援：本流程只用你自备的原厂 BL 引导设备进入 Download 模式；"
                       "不写任何存储，完成后仍需正常刷写（facts §F5）。"));
}

void EubRecoveryDialog::log(const QString &line, bool isError)
{
    m_logView->appendPlainText(isError ? QStringLiteral("!! %1").arg(line) : line);
    if (m_logSink)
        m_logSink(line, isError);
}

void EubRecoveryDialog::refreshStartEnabled()
{
    // extraFiles（9830）一票否决：勾选也不能解除 —— 本仓本期没有"段后另发"路径，run 会 fail-closed
    // 拒绝执行（见 prepare 与该字段注释），让按钮亮着等于给用户一个必然失败的入口。
    m_startBtn->setEnabled(m_prepared && m_confirmBox->isChecked() && !m_extraFilesBlocked);
}

bool EubRecoveryDialog::isStartEnabledForTest() const
{
    // 读**真实控件**：若这里另算一份 (m_prepared && isChecked())，那么"忘了调 refreshStartEnabled"
    // 的实现照样能过门控用例 —— 用例钉的就是这个控件状态（见头注释）。
    return m_startBtn->isEnabled();
}

void EubRecoveryDialog::refreshPreview(const QString &deviceLine, const QString &sourceDescription)
{
    m_deviceLabel->setText(deviceLine);
    m_sourceLabel->setText(sourceDescription.isEmpty()
        ? QStringLiteral("来源：未提供描述（按注入的字节直接预检）")
        : QStringLiteral("来源：%1").arg(sourceDescription));

    if (m_loadout.segments.isEmpty()) {
        // 预检未通过（表被清空）：不给段表、也不给 sha1 对照 —— 空表的"对照"只会误导
        m_segmentView->setPlainText(QStringLiteral("（未通过预检：没有可展示的布局表）"));
        m_shaLabel->setText(QStringLiteral("sboot 修订对照：（未通过预检，不做对照）"));
        return;
    }
    m_segmentView->setPlainText(segmentTableText(m_loadout));
    m_shaLabel->setText(QStringLiteral("sboot 修订对照：%1")
        .arg(sha1CompareText(m_loadout.sbootSha1, eub::sha1Hex(m_sboot).toLatin1())));
}

bool EubRecoveryDialog::prepare(const QByteArray &sboot, const QString &sourceDescription,
                                QString *error)
{
    // fail-closed：入口先清（早退路径不少，逐个清容易漏；同 eub_session.cpp:100 的 identify 惯例）
    m_prepared = false;
    m_extraFilesBlocked = false;        // 本次预检的闸位：不随上一次的载荷残留
    m_sboot = sboot;
    m_loadout = eub::EubLoadout{};

    // 失败路径带上"识别到底成没成"：设备行不能一律写"识别失败"—— 太短/不匹配是**载荷**的问题，
    // 设备其实识别成功了（用户据此知道该换固件还是该查设备）。
    auto fail = [&](const QString &deviceLine, const QString &msg) {
        refreshPreview(deviceLine, sourceDescription);
        refreshStartEnabled();
        if (error) *error = msg;
        log(msg, true);
        return false;
    };

    QString err;
    QString socOrigin = QStringLiteral("设备自报");
    if (!m_session.identify(m_loadout, &err)) {
        // facts §A4 的兜底：极老 SoC 不报 SoC 名（iProduct 是 "SEC S5PC210 Test B/D"），
        // 参照实现在**镜像内容**里正则 `EXYNOS[0-9]+` 反推（hubble.py:192-202）。
        // 识别失败有两条：设备没报名字 / 报了个表里没有的名字 —— 两条都试兜底（镜像才是
        // "我们要切哪个 sboot"的直接证据），兜底也失败才报错。
        const QString imageSoc = eub::detectSocFromImage(sboot);
        QString lookupErr;
        if (imageSoc.isEmpty() || !eub::eubLoadoutFor(imageSoc, m_loadout, &lookupErr)) {
            m_loadout = eub::EubLoadout{};      // eubLoadoutFor 自己会清，这里覆盖"镜像没名字"那条
            const QString fallback = imageSoc.isEmpty()
                ? QStringLiteral("兜底：设备未自报 SoC 名时可用镜像反推，但本镜像里没有 "
                                 "EXYNOS<型号> 字样（facts §A4）——无法从镜像识别 SoC。"
                                 "请确认选的是原厂 BL 包里的 sboot.bin（或 sboot.bin.lz4），"
                                 "而不是别的分区镜像")
                : QStringLiteral("兜底：从镜像反推得到「%1」，但该 SoC 也没有布局表：%2"
                                 "（facts §A4）").arg(imageSoc, lookupErr);
            return fail(QStringLiteral("设备：识别失败（原因见日志与下方弹窗）"),
                        QStringLiteral("%1\n%2").arg(err, fallback));
        }
        // 措辞**不断言成因**：本兜底对"任意 identify 失败"生效 —— 设备没报名字只是其中一种，
        // 设备根本没连上 / 自述读失败同样走到这里（那时写"设备未自报 SoC 名"就是假话）。
        // 真实原因由 %1 原样带出（identify 的 error），来源只声称"镜像内容"，并点明这条判定的
        // 前提（依赖镜像正确）与后续补丁（run 第 1 段仍会核对设备身份，见 eub_session.cpp）。
        socOrigin = QStringLiteral("镜像内容反推（facts §A4）");
        log(QStringLiteral("未能从设备读出可用的 SoC 名（%1）；已按镜像内容识别为 %2 —— "
                           "该判定依赖镜像正确，且发送前仍会核对设备身份")
                .arg(err, imageSoc));
    }

    // 切段**试算**：太短当场拒绝，不等到用户点了"开始"才报（也保证"开始"时不再有表/镜像不匹配
    // 这类可预见的失败）。切出的字节本层不留用 —— run() 会按同一张表重切（不重复实现）。
    QList<QPair<QString, QByteArray>> parts;
    QString splitErr;
    if (!eub::splitSboot(sboot, m_loadout, parts, &splitErr)) {
        const QString soc = m_loadout.soc;          // 清表前留名，供设备行说明"识别成功但载荷不匹配"
        m_loadout = eub::EubLoadout{};
        return fail(QStringLiteral("设备 SoC：%1（已识别；载荷预检未通过，未发送任何字节）").arg(soc),
                    splitErr);
    }

    m_prepared = true;
    // I1（终审）：该 SoC 的参照流程要在分段**之后另发** BL 包内文件（9830 = ldfw.img/tzsw.img，
    // hubble.py:329-341），而本仓本期没有该发送路径 —— run 会据此 fail-closed 拒绝执行。在这里
    // 就把情形讲明并关闸：让用户"点了开始才发现"等于把一次**已知做不成**的操作留给设备去承担。
    // 段表照常展示（偏移/证据仍可供人工与参照核对），只是「开始救援」不可用。
    QString extraNote;
    if (!m_loadout.extraFiles.isEmpty()) {
        m_extraFilesBlocked = true;
        extraNote = QStringLiteral(
            "\n本仓本期不发送额外文件（%1）：参照流程要求它们在分段之后另发，而本仓尚未实现该发送"
            "路径 —— 为避免把设备留在半完成状态（分段已发、文件未发），「开始救援」已禁用。")
                        .arg(m_loadout.extraFiles.join(QStringLiteral("、")));
    }
    refreshPreview(QStringLiteral("设备 SoC：%1（来源：%2）\n布局表：%3 段；证据：%4%5")
                       .arg(m_loadout.soc, socOrigin)
                       .arg(m_loadout.segments.size())
                       .arg(m_loadout.evidence, extraNote),
                   sourceDescription);
    refreshStartEnabled();
    const QString gateNote = m_extraFilesBlocked
        ? QStringLiteral("该 SoC 的参照流程需在段后另发 %1（本仓本期不发送），已禁用「开始救援」")
              .arg(m_loadout.extraFiles.join(QStringLiteral("、")))
        : (m_confirmBox->isChecked()
               ? QStringLiteral("已勾选确认，可以开始")
               : QStringLiteral("请阅读段表与 sha1 对照后勾选确认"));
    log(QStringLiteral("预检通过：%1，%2 段（证据：%3）。%4")
            .arg(m_loadout.soc)
            .arg(m_loadout.segments.size())
            .arg(m_loadout.evidence, gateNote));
    return true;
}

void EubRecoveryDialog::onPickSource()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 sboot 来源"), QString(),
        QStringLiteral("sboot 镜像 (*.bin *.lz4);;三星 BL 包 (*.tar.md5 *.tar);;所有文件 (*)"));
    if (path.isEmpty())
        return;     // 用户取消：不算错误，保持原状态

    QByteArray sboot;
    eub::SbootSource source;
    QString err;
    if (!eub::loadSbootBytes(path, sboot, &source, &err)) {
        log(err, true);
        QMessageBox::warning(this, QStringLiteral("载荷读取失败"),
                             QStringLiteral("%1\n\n请换用原厂 BL 包（BL_*.tar.md5）内的 sboot.bin(.lz4) "
                                            "或裸 sboot.bin（facts §C1）。").arg(err));
        return;
    }

    if (!prepare(sboot, source.description, &err)) {
        QMessageBox::warning(this, QStringLiteral("预检失败"),
                             QStringLiteral("%1\n\n本流程不会发送任何字节。").arg(err));
        return;
    }
    log(QStringLiteral("已载入载荷：%1").arg(source.description));
}

void EubRecoveryDialog::onStart()
{
    if (!m_startBtn->isEnabled())      // 双保险：门控以外的路径（如快捷键）不许绕过
        return;

    QString err;
    log(QStringLiteral("开始救援：向设备 RAM 逐段发送 %1 段（每段重开设备；失败即中止，"
                       "不支持从中间续传）…").arg(m_loadout.segments.size()));
    if (!m_session.run(m_loadout, m_sboot, &err)) {
        log(err, true);
        // 失败文案要可行动：告诉用户"设备现在处于什么状态"与"下一步做什么"（对话框是人机界面）
        QMessageBox::critical(this, QStringLiteral("EUB 救援失败"),
                              QStringLiteral("%1\n\n本次救援中止：设备未被引导进 Download 模式"
                                             "（引导链必须从第 1 段重来，不支持中途续传）。\n"
                                             "可重试：确认设备仍在 EUB 模式（必要时重新插拔/重新进 EUB）、"
                                             "换用与布局表匹配的原厂 BL，再点「开始救援」。")
                                  .arg(err));
        return;
    }
    log(QStringLiteral("救援完成：分段已全部发送。"));
    QMessageBox::information(this, QStringLiteral("EUB 救援"), rescueSuccessText());
}

QString EubRecoveryDialog::statusText() const
{
    if (!m_prepared)
        return QStringLiteral("未就绪：请先选择 sboot 来源并完成预检");
    return m_deviceLabel->text() + QLatin1Char('\n') + m_shaLabel->text();
}

QString EubRecoveryDialog::segmentTableText(const eub::EubLoadout &lo)
{
    QStringList lines;
    lines.reserve(lo.segments.size() + 6);
    for (const eub::EubSegment &s : lo.segments) {
        // 逐段一行：名字 + 该段**自己的**偏移与长度（段名各工具不一，以偏移为准，facts §C2）
        lines << QStringLiteral("%1  offset 0x%2  length 0x%3（%4 字节）")
                     .arg(s.name, QString::number(s.offset, 16), QString::number(s.length, 16))
                     .arg(s.length);
    }
    if (!lo.evidence.isEmpty())
        lines << QStringLiteral("证据：%1").arg(lo.evidence);
    if (!lo.sourceNote.isEmpty())
        lines << QStringLiteral("来源：%1").arg(lo.sourceNote);

    // 帧风格出处（facts §B6 硬要求：UI 必须说明"照抄自哪个实现"，且不得声称理解其语义）。
    // 判据按字节比对内置两种风格；都不是 → 自报"自定义"，**不冒用**某个实现的名义。
    QString origin;
    if (lo.style.header == eub::dnwStyle().header && lo.style.trailer == eub::dnwStyle().trailer)
        origin = QStringLiteral("照抄 hubble");
    else if (lo.style.header == eub::zeroStyle().header
             && lo.style.trailer == eub::zeroStyle().trailer)
        origin = QStringLiteral("照抄 exynos-usbdl");
    else
        origin = QStringLiteral("自定义（非内置风格，按给定的头/尾原样发送）");
    lines << QStringLiteral("帧风格：%1（头 `%2` / 尾 `%3`）；该两字段语义未定"
                            "（facts §B4/§B5：三个可用实现互相矛盾，本仓只照抄其中之一，不声称理解语义）")
                 .arg(origin, hexBytes(lo.style.header), hexBytes(lo.style.trailer));

    if (!lo.extraFiles.isEmpty()) {
        // 口径与实现一致（终审 I1）：参照流程要在段后另发这些文件，而本仓本期**没有**该发送路径
        // （run 会据此 fail-closed）—— 写"各段发完后另发"会让用户以为本工具会发。
        lines << QStringLiteral("额外文件：%1 —— 参照流程需在段后另发（包内自备）；"
                                "**本仓本期不发送**（发送路径未实现，run 会据此拒绝执行）")
                     .arg(lo.extraFiles.join(QStringLiteral("、")));
    }
    lines << (lo.responseSupport
        ? QStringLiteral("回显：该 SoC 会回显，每段后读一次、原文落日志（本期不解析结构，facts §C8/§D7）")
        : QStringLiteral("回显：无（该 SoC 不产生回显）"));
    return lines.join(QLatin1Char('\n'));
}

QString EubRecoveryDialog::sha1CompareText(const QByteArray &tableSha1, const QByteArray &fileSha1)
{
    const QString filePrefix = QString::fromLatin1(fileSha1.left(8));
    if (tableSha1.isEmpty()) {
        // 表未记录修订（hubble 系 JSON 无修订字段）：明说"不知道"，**不得**报成一致
        return QStringLiteral("该表未记录固件修订（无法对照）；你的文件 sha1 前 8 位 %1")
            .arg(filePrefix);
    }
    const QString tablePrefix = QString::fromLatin1(tableSha1.left(8));
    if (tableSha1 == fileSha1) {
        return QStringLiteral("sha1 一致（%1）：你的固件与该表所用修订相同").arg(filePrefix);
    }
    // 不硬拒绝（spec §D6：用户的固件几乎必然不同修订），但两边前 8 位都要给 —— 只给一边没法核对
    return QStringLiteral("sha1 不一致：表 %1 / 你的文件 %2（布局偏移可能不匹配）")
        .arg(tablePrefix, filePrefix);
}

QString EubRecoveryDialog::rescueSuccessText()
{
    // facts §F5/§D1：本流程只向设备 RAM 发了引导镜像，**没有任何写存储命令**；bootloader 仍处于
    // 被擦状态，必须随后正常刷写 —— 这句不能省（用户以为"救援完成 = 修好了"是最坏误解）。
    return QStringLiteral("分段已全部发送。设备应已进入 Download 模式，请继续用三星刷写"
                          "（BL/AP/CP/CSC）刷入固件。\n\n"
                          "注意：本流程只向设备 RAM 发送了引导镜像，未写任何存储，"
                          "设备仍需正常刷写。");
}

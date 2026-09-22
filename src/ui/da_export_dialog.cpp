// src/ui/da_export_dialog.cpp —— 见 .h 的入口形态与门控口径。
//
// 本文件把三层串起来（都是既有交付，本任务只做接线）：
//   芯片码 → 芯片表（`lookupChip` 取 **dacode**，不是 hw_code —— 89 条里 31 条两者不同）
//   → `parseDaFile` → `selectDaEntry`（region[1]/[2] 硬编码）→ `writeExtractedDAs`
// 组合顺序与判据都在被调方（T4/D1），本层**不重写任何一条**：这里只做"取输入 → 报结论"。
#include "ui/da_export_dialog.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/modes/mtk_chip_table.h"
#include "core/modes/mtk_da_file.h"
#include "ui/chip_code_text.h"       // parseHwCodeText（芯片码文本解析的唯一实现；与 BROM 区共用）

DaExportDialog::DaExportDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("导出 DA 片段（DA1/DA2）"));

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_daPathEdit = new QLineEdit(this);
    m_daPathEdit->setObjectName(QStringLiteral("daFilePathEdit"));
    m_daPathEdit->setPlaceholderText(QStringLiteral("AllInOne DA 文件（*.bin）"));
    auto *daBrowse = new QPushButton(QStringLiteral("浏览…"), this);
    auto *daRow = new QHBoxLayout;
    daRow->addWidget(m_daPathEdit, 1);
    daRow->addWidget(daBrowse);
    form->addRow(QStringLiteral("DA 文件"), daRow);

    m_hwCodeEdit = new QLineEdit(this);
    m_hwCodeEdit->setObjectName(QStringLiteral("daHwCodeEdit"));
    m_hwCodeEdit->setPlaceholderText(QStringLiteral("如 0x6735"));
    form->addRow(QStringLiteral("芯片码 hw_code"), m_hwCodeEdit);

    m_outDirEdit = new QLineEdit(this);
    m_outDirEdit->setObjectName(QStringLiteral("daOutDirEdit"));
    m_outDirEdit->setPlaceholderText(QStringLiteral("导出目录（已存在）"));
    auto *outBrowse = new QPushButton(QStringLiteral("浏览…"), this);
    auto *outRow = new QHBoxLayout;
    outRow->addWidget(m_outDirEdit, 1);
    outRow->addWidget(outBrowse);
    form->addRow(QStringLiteral("输出目录"), outRow);

    root->addLayout(form);

    auto *hint = new QLabel(QStringLiteral(
        "本地只读：只读这份 DA 文件、写 da1.bin 与 da2.bin 两个文件 —— 不连设备、不改设备任何状态。\n"
        "dacode 由芯片表按芯片码给出；设备信息面板里的 hw_code 就是这里的芯片码。\n"
        "DA1 恒取 region[1]、DA2 恒取 region[2]（按序号挑条目会把 region[0] 的 EMI/env 当 DA1，故不提供）。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    root->addWidget(hint);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("daStatusLabel"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_exportBtn = new QPushButton(QStringLiteral("导出"), this);
    m_exportBtn->setObjectName(QStringLiteral("daExportButton"));
    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_exportBtn);
    buttons->addWidget(closeBtn);
    root->addLayout(buttons);

    connect(daBrowse, &QPushButton::clicked, this, &DaExportDialog::onBrowseDaFile);
    connect(outBrowse, &QPushButton::clicked, this, &DaExportDialog::onBrowseOutDir);
    connect(m_exportBtn, &QPushButton::clicked, this, &DaExportDialog::onExportClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void DaExportDialog::setDaFilePath(const QString &path) { m_daPathEdit->setText(path); }
void DaExportDialog::setHwCodeText(const QString &text)  { m_hwCodeEdit->setText(text); }
void DaExportDialog::setOutputDir(const QString &dir)    { m_outDirEdit->setText(dir); }

void DaExportDialog::onBrowseDaFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 AllInOne DA 文件"), QString(),
        QStringLiteral("DA 文件 (*.bin);;所有文件 (*)"));
    if (!path.isEmpty())
        m_daPathEdit->setText(path);
}

void DaExportDialog::onBrowseOutDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择导出目录"), QString());
    if (!dir.isEmpty())
        m_outDirEdit->setText(dir);
}

void DaExportDialog::onExportClicked()
{
    exportNow(nullptr);   // 状态行已在 exportNow 里落字，按钮这条路径不另写一套文案
}

bool DaExportDialog::exportNow(QString *error)
{
    if (error) error->clear();

    const QString daPath = m_daPathEdit->text().trimmed();
    const QString outDir = m_outDirEdit->text().trimmed();
    if (daPath.isEmpty()) {
        setStatus(QStringLiteral("✘ 请先选择 DA 文件"), true);
        if (error) *error = QStringLiteral("未指定 DA 文件");
        return false;
    }
    if (outDir.isEmpty()) {
        setStatus(QStringLiteral("✘ 请先指定导出目录"), true);
        if (error) *error = QStringLiteral("未指定导出目录");
        return false;
    }

    // ① 芯片码 → hw_code（解析的唯一实现与 BROM 区共用；两条入口不接受不同形态的输入）。
    quint16 hwCode = 0;
    QString perr;
    if (!bromui::parseHwCodeText(m_hwCodeEdit->text(), &hwCode, &perr)) {
        setStatus(QStringLiteral("✘ 芯片码不可用：%1").arg(perr), true);
        if (error) *error = perr;
        return false;
    }

    // ② hw_code → **dacode**（查 DA 条目的键；89 条里 31 条 dacode != hw_code，不能拿 hw_code 顶替）。
    const mtkbrom::ChipInfo *chip = mtkbrom::lookupChip(hwCode);
    if (!chip) {
        const QString why = QStringLiteral("芯片 %1 不在芯片表内 —— 取不到 dacode（查 DA 条目的键），"
                                           "请核对设备信息面板里的 hw_code")
                                .arg(bromui::formatHwCodeHint(hwCode));
        setStatus(QStringLiteral("✘ %1").arg(why), true);
        if (error) *error = why;
        return false;
    }

    // ③ 读 DA 文件（本功能不依赖设备：读的是用户显式给的文件）。
    QFile f(daPath);
    if (!f.open(QIODevice::ReadOnly)) {
        const QString why = QStringLiteral("DA 文件打不开：%1（%2）").arg(daPath, f.errorString());
        setStatus(QStringLiteral("✘ %1").arg(why), true);
        if (error) *error = why;
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    mtkbrom::DaFile parsed;
    QString parseErr;
    if (!mtkbrom::parseDaFile(raw, parsed, &parseErr)) {
        const QString why = QStringLiteral("DA 解析失败：%1").arg(parseErr);
        setStatus(QStringLiteral("✘ %1").arg(why), true);
        if (error) *error = why;
        return false;
    }

    // ④ 选条目：版本两维传 0 = **该维旁路**（规则 2：设备值为 0 时旁路；本入口没有设备可问）。
    //    多条候选同 hw_code 时取文件顺序首个（上游 first-match），跳过的条目会进 warnings。
    QStringList warnings;
    mtkbrom::DaSelection sel;
    QString selErr;
    if (!mtkbrom::selectDaEntry(parsed, chip->dacode, /*deviceHwVer=*/0, /*deviceSwVer=*/0,
                                &warnings, sel, &selErr)) {
        // 这里打印的是 dacode（不是 chipHint 形态的 hw_code）—— 故显式格式化，不借 formatHwCodeHint。
        const QString why = QStringLiteral("按 dacode 0x%1 选条目失败：%2")
                                .arg(chip->dacode, 4, 16, QLatin1Char('0'))
                                .arg(selErr);
        setStatus(QStringLiteral("✘ %1").arg(why), true);
        if (error) *error = why;
        return false;
    }

    // ⑤ 落盘（逐字节照搬选中切片；失败路径不留半份产物 —— 判据与清理都在 writeExtractedDAs）。
    QString da1Path, da2Path;
    QString writeErr;
    if (!mtkbrom::writeExtractedDAs(sel, outDir, da1Path, da2Path, &writeErr)) {
        setStatus(QStringLiteral("✘ 导出失败：%1").arg(writeErr), true);
        if (error) *error = writeErr;
        return false;
    }

    QString status = QStringLiteral("✔ 已导出 %1（%2 字节）+ %3（%4 字节）→ %5")
                         .arg(QFileInfo(da1Path).fileName())
                         .arg(sel.da1Bytes.size())
                         .arg(QFileInfo(da2Path).fileName())
                         .arg(sel.da2Bytes.size())
                         .arg(outDir);
    // 世代提示：V6 的文件只能推向 XML（isXmlForced，与 P10 的负向判别式同源）——照实说给用户。
    if (sel.isXmlForced)
        status += QStringLiteral(" ｜ 注：该 DA 文件含 MTK_DA_v6 标记（只能用于 XML 路径）");
    if (!warnings.isEmpty())
        status += QStringLiteral(" ｜ 跳过：%1").arg(warnings.join(QStringLiteral("；")));
    setStatus(status, false);
    return true;
}

void DaExportDialog::setStatus(const QString &text, bool isError)
{
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(isError
        ? QStringLiteral("color: #e74c3c; font-size: 12px;")
        : QStringLiteral("color: #27ae60; font-size: 12px;"));
}

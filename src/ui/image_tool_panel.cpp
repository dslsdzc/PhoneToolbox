#include "image_tool_panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QDateTime>
#include <QScrollBar>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QAction>
#include <QPoint>

// D4 修补对话框依赖（root_patcher 各 patcher 的下载源/缓存 key 辅助）
#include <QDialog>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QHash>
#include "root_patcher/magisk_patcher.h"
#include "root_patcher/kernelsu_patcher.h"
#include "root_patcher/assets_downloader.h"

namespace {

// 格式枚举 → 展示名（与 registry.cpp detect() 的 detail 文案风格一致）
QString formatName(imgreg::Format f)
{
    switch (f) {
    case imgreg::Format::Payload:    return QStringLiteral("Payload OTA 包");
    case imgreg::Format::Zip:        return QStringLiteral("Zip 刷机包");
    case imgreg::Format::Tar:        return QStringLiteral("Tar 镜像");
    case imgreg::Format::TarMd5:     return QStringLiteral("Tar.md5 镜像");
    case imgreg::Format::Sparse:     return QStringLiteral("Sparse 稀疏镜像");
    case imgreg::Format::Super:      return QStringLiteral("Super 动态分区");
    case imgreg::Format::Boot:       return QStringLiteral("Boot 镜像");
    case imgreg::Format::VendorBoot: return QStringLiteral("Vendor_boot 镜像");
    case imgreg::Format::Vbmeta:     return QStringLiteral("Vbmeta 镜像");
    case imgreg::Format::Dtb:        return QStringLiteral("Dtb 镜像");
    case imgreg::Format::Br:         return QStringLiteral("Brotli 压缩流");
    case imgreg::Format::Lz4:        return QStringLiteral("LZ4 压缩流");
    case imgreg::Format::Xz:         return QStringLiteral("XZ 压缩流");
    case imgreg::Format::Gzip:       return QStringLiteral("Gzip 压缩流");
    case imgreg::Format::Zstd:       return QStringLiteral("Zstd 压缩流");
    case imgreg::Format::Brotli:     return QStringLiteral("Brotli 压缩流");
    case imgreg::Format::Dat:        return QStringLiteral("Dat 系统分区");
    case imgreg::Format::Pac:        return QStringLiteral("PAC 固件");
    case imgreg::Format::Kdz:        return QStringLiteral("KDZ 固件");
    case imgreg::Format::UpdateApp:  return QStringLiteral("Update.app 固件");
    case imgreg::Format::UpdateBin:  return QStringLiteral("Update.bin 固件");
    case imgreg::Format::Sin:        return QStringLiteral("SIN 镜像");
    case imgreg::Format::DiskGpt:    return QStringLiteral("GPT 磁盘镜像");
    case imgreg::Format::TwrpWin:    return QStringLiteral("TWRP 备份镜像");
    case imgreg::Format::Erofs:      return QStringLiteral("EROFS 分区镜像");
    case imgreg::Format::Ext4:       return QStringLiteral("Ext4 分区镜像");
    case imgreg::Format::RawImage:   return QStringLiteral("Raw 镜像");
    case imgreg::Format::Unknown:    return QStringLiteral("未知格式");
    }
    return QStringLiteral("未知格式");
}

// RootType → 展示名（与 root_patcher.cpp rootTypeName 文案一致）
QString rootTypeDisplayName(patcher::RootType t)
{
    switch (t) {
    case patcher::RootType::Magisk:        return QStringLiteral("Magisk");
    case patcher::RootType::MagiskAlpha:   return QStringLiteral("Magisk Alpha");
    case patcher::RootType::Kitsune:       return QStringLiteral("Kitsune Magisk");
    case patcher::RootType::KernelSU:      return QStringLiteral("KernelSU");
    case patcher::RootType::KernelSU_Next: return QStringLiteral("KernelSU Next");
    case patcher::RootType::SukiSU:        return QStringLiteral("SukiSU");
    case patcher::RootType::ReSukiSU:      return QStringLiteral("ReSukiSU");
    case patcher::RootType::APatch:        return QStringLiteral("APatch");
    case patcher::RootType::KernelPatch:   return QStringLiteral("KernelPatch");
    case patcher::RootType::RamdiskSu:     return QStringLiteral("RamdiskSu");
    case patcher::RootType::ModuleInstall: return QStringLiteral("ModuleInstall");
    }
    return QStringLiteral("未知方案");
}

} // namespace

namespace { // （与上方匿名命名空间同属一个，Dialog 私有类与 formatName 工具并存）

// ==================== Root 修补方案选择对话框（D4） ====================
//
// 后端接口核实（2026-08-05，以 src/root_patcher 实际实现为准，非计划文档）：
//   • RootType 11 项（root_patcher.h L8-20）：Magisk 系 3（Magisk/
//     MagiskAlpha/Kitsune）、KernelSU 系 4（KernelSU/KernelSU_Next/SukiSU/
//     ReSukiSU）、APatch/KernelPatch、RamdiskSu、ModuleInstall —— 分组与
//     C 计划一致（3+4+2+2）
//   • PatchConfig（root_patcher.h L31-42）：type/apkPath/koPath/kpatchPath/
//     suZipPath/moduleZipPath/deviceKmi/variant
//   • patchFile(bootPath,cfg,&outPath,&error)（root_patcher.cpp L78）：
//     读 boot → create() 派发 → 自动备份 <源>.orig.bak、写 <基名>_patched.img；
//     失败不落任何产物；已存在 .orig.bak 拒绝修补（防覆盖原厂备份）；
//     ModuleInstall 明确拒绝（输出为模块包，须直接使用 ModuleInstaller ——
//     UI 侧接线待办，见后端待办）
//   • create() 11 项全部非空（magisk_patcher.cpp 工厂 L165）；KernelSU 系空
//     variant 按 RootType 默认映射 official/next/suki/resuki（kernelsu_
//     patcher.cpp L104-120）；Magisk 系 variant 仅下载 key 用（patch() 不读）
//   • MagiskPatcher::downloadUrl() 返回 releases 页（asset 名随版本变化，
//     由调用方经 GitHub API 解析具体 asset）；Kitsune 返回空（官方源已
//     下线，2026-08 验证）→ 无下载按钮
//   • KernelSuPatcher：koUrl(variant,kmi)/ksuinitUrl(variant) 为直接资产
//     URL；supportedKmis(variant) 7 个 KMI；next/suki 的 ksuinit 回退官方
//     KernelSU 版本（fork 定制行为不保证）→ 对话框内标注
//   • APatchPatcher：apkPath 全宿主拒绝（APK 内 libkptools.so 为 Android
//     bionic PIE，PC 无法 exec，计划 C F-1）→ 仅 kpatchPath 手动目录
//     （kptools*/kpimg*，Windows 为 win/kptools.exe 解压形态）；下载 APK
//     无法使用 → 不提供下载
//   • RamdiskSu：suZipPath（SuperSU 刷入包 zip）；ModuleInstall：
//     moduleZipPath（模块 zip）
//   • AssetsDownloader：无逐块进度信号（仅 downloadFinished/Failed）→
//     对话框内以不定进度条表示"下载中"（UI 侧适配）；key↔URL 须一一对应
//     （不同 URL 必须不同 key，kernelsu 的 ko 与 ksuinit 分开 key）
//
// 注入物优先级：手动指定 > 缓存 > 下载（与 AssetsDownloader 语义一致；
// 手动浏览后点下载即提示"手动优先"跳过）。下载失败/取消均可改手动指定。
class RootPatchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RootPatchDialog(const QString &bootPath, QWidget *parent = nullptr);
    patcher::PatchConfig config() const { return m_cfg; }

private slots:
    void onTypeChanged();
    void onDownloadFinished(const QString &key, const QString &filePath);
    void onDownloadFailed(const QString &key, const QString &error);

private:
    void setupUI();
    void addTypeButton(patcher::RootType type, const QString &name, QBoxLayout *layout);
    void updateFields();
    void browseFor(const QString &field);
    void startFieldDownload(const QString &field);
    void startDownload(const QString &key, const QString &field, const QUrl &url);
    void startGithubResolve(const QString &repo, const QRegularExpression &assetPattern);
    void onOkClicked();
    void setBusy(bool busy);
    void setNote(const QString &text);
    QString defaultNote(patcher::RootType type) const;
    QString variantFor(patcher::RootType type) const;
    QString ksuDownloadKey(patcher::RootType type, const QString &suffix) const;
    static bool isMagiskFamily(patcher::RootType t);
    static bool isKsuFamily(patcher::RootType t);
    patcher::RootType currentType() const;

    patcher::PatchConfig m_cfg;

    // 方案单选组（11 项，与 C 计划分组一致）
    QButtonGroup *m_typeGroup = nullptr;
    QList<patcher::RootType> m_types;   // 与 m_typeButtons 顺序对应
    QList<QRadioButton *> m_typeButtons;

    // 注入物字段行（按方案显隐；apk 行在 Magisk 系指 APK、KernelSU 系指 ksuinit）
    QWidget *m_apkRow = nullptr;
    QLabel *m_apkLabel = nullptr;
    QLineEdit *m_apkEdit = nullptr;
    QPushButton *m_apkBrowseBtn = nullptr;
    QPushButton *m_apkDownloadBtn = nullptr;
    QWidget *m_koRow = nullptr;
    QLineEdit *m_koEdit = nullptr;
    QPushButton *m_koBrowseBtn = nullptr;
    QPushButton *m_koDownloadBtn = nullptr;
    QWidget *m_kmiRow = nullptr;
    QComboBox *m_kmiCombo = nullptr;
    QWidget *m_kpatchRow = nullptr;
    QLineEdit *m_kpatchEdit = nullptr;
    QPushButton *m_kpatchBrowseBtn = nullptr;
    QWidget *m_suZipRow = nullptr;
    QLineEdit *m_suZipEdit = nullptr;
    QPushButton *m_suZipBrowseBtn = nullptr;
    QWidget *m_moduleZipRow = nullptr;
    QLineEdit *m_moduleZipEdit = nullptr;
    QPushButton *m_moduleZipBrowseBtn = nullptr;

    QLabel *m_warnLabel = nullptr; // 顶部固定提示（unlock 警告 + 备份语义）
    QLabel *m_noteLabel = nullptr; // 动态提示（按方案/下载状态）
    QProgressBar *m_progress = nullptr;
    QPushButton *m_okBtn = nullptr; // 下载期间一并冻结

    patcher::AssetsDownloader *m_downloader = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
    QHash<QString, QString> m_keyTarget; // 下载 key → 字段（"apk"/"ko"）
    int m_activeDownloads = 0;
};

bool RootPatchDialog::isMagiskFamily(patcher::RootType t)
{
    return t == patcher::RootType::Magisk || t == patcher::RootType::MagiskAlpha ||
           t == patcher::RootType::Kitsune;
}

bool RootPatchDialog::isKsuFamily(patcher::RootType t)
{
    return t == patcher::RootType::KernelSU || t == patcher::RootType::KernelSU_Next ||
           t == patcher::RootType::SukiSU || t == patcher::RootType::ReSukiSU;
}

patcher::RootType RootPatchDialog::currentType() const
{
    for (int i = 0; i < m_typeButtons.size(); ++i) {
        if (m_typeButtons.at(i)->isChecked())
            return m_types.at(i);
    }
    return patcher::RootType::Magisk; // 防御：默认首个
}

// variant 归一映射（与 kernelsu_patcher.cpp L104-120 / magisk 各下载源一致；
// APatch/KernelPatch/RamdiskSu/ModuleInstall 不使用 variant）
QString RootPatchDialog::variantFor(patcher::RootType type) const
{
    switch (type) {
    case patcher::RootType::MagiskAlpha:   return QStringLiteral("alpha");
    case patcher::RootType::Kitsune:       return QStringLiteral("kitsune");
    case patcher::RootType::KernelSU_Next: return QStringLiteral("next");
    case patcher::RootType::SukiSU:        return QStringLiteral("suki");
    case patcher::RootType::ReSukiSU:      return QStringLiteral("resuki");
    case patcher::RootType::KernelSU:      return QStringLiteral("official");
    case patcher::RootType::Magisk:        return QStringLiteral("official");
    default:                               return QString();
    }
}

// KernelSU 系下载缓存 key：assetKey(variant) + 用途 + "-latest"（URL 为
// releases/latest/download，无版本号可辨 → "latest" 即诚实版本令牌；
// ko 与 ksuinit 不同 URL → 必须分开 key）
QString RootPatchDialog::ksuDownloadKey(patcher::RootType type, const QString &suffix) const
{
    return patcher::KernelSuPatcher::assetKey(variantFor(type))
        + QLatin1Char('-') + suffix + QStringLiteral("-latest");
}

RootPatchDialog::RootPatchDialog(const QString &bootPath, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Root 修补方案"));
    setupUI();

    m_downloader = new patcher::AssetsDownloader(this);
    m_nam = new QNetworkAccessManager(this);
    connect(m_downloader, &patcher::AssetsDownloader::downloadFinished,
            this, &RootPatchDialog::onDownloadFinished);
    connect(m_downloader, &patcher::AssetsDownloader::downloadFailed,
            this, &RootPatchDialog::onDownloadFailed);

    if (!bootPath.isEmpty())
        m_warnLabel->setText(QStringLiteral("目标镜像: %1\n%2")
                                 .arg(QFileInfo(bootPath).fileName(), m_warnLabel->text()));

    // 默认选中 Magisk（最常见方案）
    if (!m_typeButtons.isEmpty())
        m_typeButtons.first()->setChecked(true);
    updateFields();
}

void RootPatchDialog::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // 顶部固定提示：unlock 警告 + 备份语义（.orig.bak 守卫，见 root_patcher.cpp）
    QLabel *tipLabel = new QLabel(QStringLiteral("⚠ 仅适用于已解锁 Bootloader 的设备"), this);
    tipLabel->setStyleSheet("color: #e67e22; font-weight: bold;");
    mainLayout->addWidget(tipLabel);
    m_warnLabel = new QLabel(QStringLiteral(
        "修补将自动备份原镜像为 <文件名>.orig.bak，并输出 <基名>_patched.img；"
        "已存在备份时拒绝修补（须先还原为原厂镜像）"), this);
    m_warnLabel->setWordWrap(true);
    mainLayout->addWidget(m_warnLabel);

    // ---- 方案分组单选（4 组，与 C 计划分组一致）----
    m_typeGroup = new QButtonGroup(this);
    m_typeGroup->setExclusive(true);

    QGroupBox *magiskBox = new QGroupBox(QStringLiteral("Magisk 系"), this);
    QVBoxLayout *magiskLayout = new QVBoxLayout(magiskBox);
    addTypeButton(patcher::RootType::Magisk,
                  QStringLiteral("Magisk（官方 topjohnwu）"), magiskLayout);
    addTypeButton(patcher::RootType::MagiskAlpha,
                  QStringLiteral("Magisk Alpha（vvb2060）"), magiskLayout);
    addTypeButton(patcher::RootType::Kitsune,
                  QStringLiteral("Kitsune Magisk（原 Magisk Delta）"), magiskLayout);
    mainLayout->addWidget(magiskBox);

    QGroupBox *ksuBox = new QGroupBox(QStringLiteral("KernelSU 系"), this);
    QVBoxLayout *ksuLayout = new QVBoxLayout(ksuBox);
    addTypeButton(patcher::RootType::KernelSU,
                  QStringLiteral("KernelSU（官方 tiann）"), ksuLayout);
    addTypeButton(patcher::RootType::KernelSU_Next,
                  QStringLiteral("KernelSU Next"), ksuLayout);
    addTypeButton(patcher::RootType::SukiSU,
                  QStringLiteral("SukiSU（SukiSU-Ultra）"), ksuLayout);
    addTypeButton(patcher::RootType::ReSukiSU,
                  QStringLiteral("ReSukiSU"), ksuLayout);
    mainLayout->addWidget(ksuBox);

    QGroupBox *apatchBox = new QGroupBox(QStringLiteral("APatch 系"), this);
    QVBoxLayout *apatchLayout = new QVBoxLayout(apatchBox);
    addTypeButton(patcher::RootType::APatch, QStringLiteral("APatch"), apatchLayout);
    addTypeButton(patcher::RootType::KernelPatch, QStringLiteral("KernelPatch"), apatchLayout);
    mainLayout->addWidget(apatchBox);

    QGroupBox *otherBox = new QGroupBox(QStringLiteral("其他"), this);
    QVBoxLayout *otherLayout = new QVBoxLayout(otherBox);
    addTypeButton(patcher::RootType::RamdiskSu,
                  QStringLiteral("RamdiskSu（SuperSU 系，老设备）"), otherLayout);
    addTypeButton(patcher::RootType::ModuleInstall,
                  QStringLiteral("ModuleInstall（模块框架安装）"), otherLayout);
    mainLayout->addWidget(otherBox);

    connect(m_typeGroup, &QButtonGroup::buttonToggled, this,
            [this](QAbstractButton *, bool checked) {
                if (checked)
                    onTypeChanged();
            });

    // ---- 注入物区域 ----
    QGroupBox *injBox = new QGroupBox(QStringLiteral("注入物"), this);
    QVBoxLayout *injLayout = new QVBoxLayout(injBox);

    // apk 行：Magisk 系 = 注入 APK；KernelSU 系 = ksuinit init wrapper
    m_apkRow = new QWidget(injBox);
    QHBoxLayout *apkLayout = new QHBoxLayout(m_apkRow);
    apkLayout->setContentsMargins(0, 0, 0, 0);
    m_apkLabel = new QLabel(QStringLiteral("注入 APK:"), m_apkRow);
    m_apkEdit = new QLineEdit(m_apkRow);
    m_apkEdit->setReadOnly(true);
    m_apkEdit->setPlaceholderText(QStringLiteral("未指定（可下载或浏览）"));
    m_apkBrowseBtn = new QPushButton(QStringLiteral("浏览..."), m_apkRow);
    m_apkDownloadBtn = new QPushButton(QStringLiteral("下载"), m_apkRow);
    apkLayout->addWidget(m_apkLabel);
    apkLayout->addWidget(m_apkEdit, 1);
    apkLayout->addWidget(m_apkBrowseBtn);
    apkLayout->addWidget(m_apkDownloadBtn);
    injLayout->addWidget(m_apkRow);

    // ko 行：KernelSU 系内核模块（.ko 或 LKM zip）
    m_koRow = new QWidget(injBox);
    QHBoxLayout *koLayout = new QHBoxLayout(m_koRow);
    koLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *koLabel = new QLabel(QStringLiteral("内核模块 (.ko):"), m_koRow);
    m_koEdit = new QLineEdit(m_koRow);
    m_koEdit->setReadOnly(true);
    m_koEdit->setPlaceholderText(QStringLiteral("未指定（可下载或浏览）"));
    m_koBrowseBtn = new QPushButton(QStringLiteral("浏览..."), m_koRow);
    m_koDownloadBtn = new QPushButton(QStringLiteral("下载"), m_koRow);
    koLayout->addWidget(koLabel);
    koLayout->addWidget(m_koEdit, 1);
    koLayout->addWidget(m_koBrowseBtn);
    koLayout->addWidget(m_koDownloadBtn);
    injLayout->addWidget(m_koRow);

    // KMI 行：KernelSU 系设备 KMI（留空 → 修补时从 boot 镜像自动检测）
    m_kmiRow = new QWidget(injBox);
    QHBoxLayout *kmiLayout = new QHBoxLayout(m_kmiRow);
    kmiLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *kmiLabel = new QLabel(QStringLiteral("设备 KMI:"), m_kmiRow);
    m_kmiCombo = new QComboBox(m_kmiRow);
    m_kmiCombo->setEditable(true);
    m_kmiCombo->setPlaceholderText(QStringLiteral("自动检测（留空）"));
    kmiLayout->addWidget(kmiLabel);
    kmiLayout->addWidget(m_kmiCombo, 1);
    injLayout->addWidget(m_kmiRow);

    // kpatch 目录行：APatch/KernelPatch 系（含 kptools*/kpimg* 的解压目录）
    m_kpatchRow = new QWidget(injBox);
    QHBoxLayout *kpatchLayout = new QHBoxLayout(m_kpatchRow);
    kpatchLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *kpatchLabel = new QLabel(
        QStringLiteral("kptools/kpimg 目录:"), m_kpatchRow);
    m_kpatchEdit = new QLineEdit(m_kpatchRow);
    m_kpatchEdit->setReadOnly(true);
    m_kpatchEdit->setPlaceholderText(QStringLiteral("未指定"));
    m_kpatchBrowseBtn = new QPushButton(QStringLiteral("浏览..."), m_kpatchRow);
    kpatchLayout->addWidget(kpatchLabel);
    kpatchLayout->addWidget(m_kpatchEdit, 1);
    kpatchLayout->addWidget(m_kpatchBrowseBtn);
    injLayout->addWidget(m_kpatchRow);

    // su 包行：RamdiskSu（SuperSU 刷入包 zip）
    m_suZipRow = new QWidget(injBox);
    QHBoxLayout *suZipLayout = new QHBoxLayout(m_suZipRow);
    suZipLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *suZipLabel = new QLabel(QStringLiteral("SuperSU 刷入包 zip:"), m_suZipRow);
    m_suZipEdit = new QLineEdit(m_suZipRow);
    m_suZipEdit->setReadOnly(true);
    m_suZipEdit->setPlaceholderText(QStringLiteral("未指定"));
    m_suZipBrowseBtn = new QPushButton(QStringLiteral("浏览..."), m_suZipRow);
    suZipLayout->addWidget(suZipLabel);
    suZipLayout->addWidget(m_suZipEdit, 1);
    suZipLayout->addWidget(m_suZipBrowseBtn);
    injLayout->addWidget(m_suZipRow);

    // 模块 zip 行：ModuleInstall
    m_moduleZipRow = new QWidget(injBox);
    QHBoxLayout *moduleZipLayout = new QHBoxLayout(m_moduleZipRow);
    moduleZipLayout->setContentsMargins(0, 0, 0, 0);
    QLabel *moduleZipLabel = new QLabel(QStringLiteral("模块 zip:"), m_moduleZipRow);
    m_moduleZipEdit = new QLineEdit(m_moduleZipRow);
    m_moduleZipEdit->setReadOnly(true);
    m_moduleZipEdit->setPlaceholderText(QStringLiteral("未指定"));
    m_moduleZipBrowseBtn = new QPushButton(QStringLiteral("浏览..."), m_moduleZipRow);
    moduleZipLayout->addWidget(moduleZipLabel);
    moduleZipLayout->addWidget(m_moduleZipEdit, 1);
    moduleZipLayout->addWidget(m_moduleZipBrowseBtn);
    injLayout->addWidget(m_moduleZipRow);

    // 动态提示
    m_noteLabel = new QLabel(injBox);
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setStyleSheet("color: #7f8c8d;");
    injLayout->addWidget(m_noteLabel);

    mainLayout->addWidget(injBox);

    // 下载进度（AssetsDownloader 无逐块进度信号 → 不定进度条表示"下载中"）
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0); // 不定模式
    m_progress->setVisible(false);
    mainLayout->addWidget(m_progress);

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_okBtn = buttons->button(QDialogButtonBox::Ok);
    m_okBtn->setText(QStringLiteral("开始修补"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &RootPatchDialog::onOkClicked);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    // 信号接线
    connect(m_apkBrowseBtn, &QPushButton::clicked, this,
            [this] { browseFor(QStringLiteral("apk")); });
    connect(m_koBrowseBtn, &QPushButton::clicked, this,
            [this] { browseFor(QStringLiteral("ko")); });
    connect(m_kpatchBrowseBtn, &QPushButton::clicked, this,
            [this] { browseFor(QStringLiteral("kpatch")); });
    connect(m_suZipBrowseBtn, &QPushButton::clicked, this,
            [this] { browseFor(QStringLiteral("suzip")); });
    connect(m_moduleZipBrowseBtn, &QPushButton::clicked, this,
            [this] { browseFor(QStringLiteral("modulezip")); });
    connect(m_apkDownloadBtn, &QPushButton::clicked, this,
            [this] { startFieldDownload(QStringLiteral("apk")); });
    connect(m_koDownloadBtn, &QPushButton::clicked, this,
            [this] { startFieldDownload(QStringLiteral("ko")); });
}

void RootPatchDialog::addTypeButton(patcher::RootType type, const QString &name,
                                    QBoxLayout *layout)
{
    auto *btn = new QRadioButton(name, qobject_cast<QWidget *>(layout->parentWidget()));
    m_typeGroup->addButton(btn);
    m_types.append(type);
    m_typeButtons.append(btn);
    layout->addWidget(btn);
}

void RootPatchDialog::onTypeChanged()
{
    updateFields();
}

void RootPatchDialog::updateFields()
{
    const patcher::RootType type = currentType();
    const bool magisk = isMagiskFamily(type);
    const bool ksu = isKsuFamily(type);

    m_apkRow->setVisible(magisk || ksu);
    if (magisk) {
        m_apkLabel->setText(QStringLiteral("注入 APK:"));
        // Kitsune 无官方下载源（官方仓库已下线，2026-08 验证）→ 隐藏下载
        m_apkDownloadBtn->setVisible(type != patcher::RootType::Kitsune);
    } else if (ksu) {
        m_apkLabel->setText(QStringLiteral("ksuinit init wrapper:"));
        m_apkDownloadBtn->setVisible(true);
    }
    m_koRow->setVisible(ksu);
    m_kmiRow->setVisible(ksu);
    if (ksu) {
        // 4 个变体 KMI 列表一致（同 7 项，kernelsu_patcher.cpp L336）→
        // 切换变体时保留用户已选/已输入文本
        const QString kmiText = m_kmiCombo->currentText();
        m_kmiCombo->clear();
        m_kmiCombo->addItems(patcher::KernelSuPatcher::supportedKmis(variantFor(type)));
        if (!kmiText.isEmpty())
            m_kmiCombo->setCurrentText(kmiText);
    }
    m_kpatchRow->setVisible(type == patcher::RootType::APatch ||
                            type == patcher::RootType::KernelPatch);
    m_suZipRow->setVisible(type == patcher::RootType::RamdiskSu);
    m_moduleZipRow->setVisible(type == patcher::RootType::ModuleInstall);
    m_noteLabel->setText(defaultNote(type));
}

void RootPatchDialog::browseFor(const QString &field)
{
    QString start;
    if (field == QLatin1String("apk"))
        start = m_apkEdit->text().trimmed();
    else if (field == QLatin1String("ko"))
        start = m_koEdit->text().trimmed();
    else if (field == QLatin1String("kpatch"))
        start = m_kpatchEdit->text().trimmed();
    else if (field == QLatin1String("suzip"))
        start = m_suZipEdit->text().trimmed();
    else
        start = m_moduleZipEdit->text().trimmed();
    if (start.isEmpty())
        start = QDir::homePath();

    QString path;
    if (field == QLatin1String("kpatch")) {
        path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择含 kptools*/kpimg* 的目录（KernelPatch 预编译资产解压目录）"),
            start);
    } else {
        path = QFileDialog::getOpenFileName(this, QStringLiteral("选择注入物文件"), start);
    }
    if (path.isEmpty())
        return;
    path = QDir::toNativeSeparators(path);
    if (field == QLatin1String("apk"))
        m_apkEdit->setText(path);
    else if (field == QLatin1String("ko"))
        m_koEdit->setText(path);
    else if (field == QLatin1String("kpatch"))
        m_kpatchEdit->setText(path);
    else if (field == QLatin1String("suzip"))
        m_suZipEdit->setText(path);
    else
        m_moduleZipEdit->setText(path);
    setNote(QStringLiteral("已手动指定: %1（手动优先，下载将忽略）").arg(path));
}

void RootPatchDialog::startFieldDownload(const QString &field)
{
    const patcher::RootType type = currentType();
    const QString manual = (field == QLatin1String("apk")) ? m_apkEdit->text().trimmed()
                                                           : m_koEdit->text().trimmed();
    // 手动指定优先于下载
    if (!manual.isEmpty()) {
        setNote(QStringLiteral("已手动指定注入物，下载忽略（手动优先）"));
        return;
    }

    if (isMagiskFamily(type)) {
        if (type == patcher::RootType::Kitsune) {
            setNote(QStringLiteral("Kitsune 无官方下载源（官方仓库已下线），请手动指定 APK"));
            return;
        }
        // 官方/Alpha：downloadUrl() 仅提供 releases 页（asset 名随版本变化）→
        // UI 侧经 GitHub API 解析具体资产后再下载
        const QString repo = (type == patcher::RootType::MagiskAlpha)
            ? QStringLiteral("vvb2060/Magisk")
            : QStringLiteral("topjohnwu/Magisk");
        const QRegularExpression pattern = (type == patcher::RootType::MagiskAlpha)
            ? QRegularExpression(QStringLiteral("^app-release\\.apk$"))
            : QRegularExpression(QStringLiteral("^Magisk-v[0-9].*\\.apk$"));
        startGithubResolve(repo, pattern);
        return;
    }
    if (isKsuFamily(type)) {
        const QString variant = variantFor(type);
        if (field == QLatin1String("apk")) {
            // next/suki 的 ksuinit 回退官方 KernelSU 版本（标注见 defaultNote）
            startDownload(ksuDownloadKey(type, QStringLiteral("ksuinit")), field,
                          patcher::KernelSuPatcher::ksuinitUrl(variant));
        } else {
            // 下载内核模块必须指定 KMI（资产按 {kmi}_kernelsu.ko 命名）；
            // 未选则弹选择框（7 选 1，可输入自定义）
            QString kmi = m_kmiCombo->currentText().trimmed();
            if (kmi.isEmpty()) {
                bool ok = false;
                const QStringList kmis = patcher::KernelSuPatcher::supportedKmis(variant);
                kmi = QInputDialog::getItem(
                    this, QStringLiteral("选择 KMI"),
                    QStringLiteral("下载内核模块需要指定 KMI（设备内核版本，如 "
                                   "android14-6.1）：\n留空则修补时从 boot 镜像自动检测。"),
                    kmis, 0, true, &ok);
                if (!ok)
                    return;
                m_kmiCombo->setCurrentText(kmi); // 回填：修补时作 deviceKmi 兜底
            }
            startDownload(ksuDownloadKey(type, QStringLiteral("ko")), field,
                          patcher::KernelSuPatcher::koUrl(variant, kmi));
        }
        return;
    }
    // APatch/KernelPatch/RamdiskSu/ModuleInstall：无下载
    setNote(QStringLiteral("该方案无自动下载（见提示），请手动指定注入物"));
}

void RootPatchDialog::startDownload(const QString &key, const QString &field, const QUrl &url)
{
    m_keyTarget.insert(key, field);
    ++m_activeDownloads;
    setBusy(true);
    m_downloader->downloadAsync(url, key);
}

void RootPatchDialog::startGithubResolve(const QString &repo,
                                         const QRegularExpression &assetPattern)
{
    ++m_activeDownloads;
    setBusy(true);
    // key 基名（assetKey：magisk / magisk-alpha / magisk-kitsune）
    const QString baseKey = patcher::MagiskPatcher::assetKey(variantFor(currentType()));
    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repo));
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "PhoneToolbox");
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, assetPattern, baseKey]() {
        reply->deleteLater();
        if (m_activeDownloads > 0)
            --m_activeDownloads;
        if (m_activeDownloads == 0)
            setBusy(false);
        if (reply->error() != QNetworkReply::NoError) {
            setNote(QStringLiteral("解析最新 release 失败: %1（可手动指定注入物）")
                        .arg(reply->errorString()));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        const QString tag = root.value(QStringLiteral("tag_name")).toString();
        QString assetName;
        QString assetUrl;
        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue &v : assets) {
            const QJsonObject a = v.toObject();
            const QString name = a.value(QStringLiteral("name")).toString();
            if (assetPattern.match(name).hasMatch()) {
                assetName = name;
                assetUrl = a.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }
        if (assetUrl.isEmpty()) {
            setNote(QStringLiteral("release 中未找到匹配资产（%1），请手动指定注入物")
                        .arg(assetPattern.pattern()));
            return;
        }
        // 版本化缓存 key（如 "magisk-v30.7"，与后端示例一致）；key 禁止路径
        // 分隔符（AssetsDownloader validKey），tag 异常时替换兜底
        QString key = baseKey + QLatin1Char('-') + tag;
        key.replace(QLatin1Char('/'), QLatin1Char('-'));
        key.replace(QLatin1Char('\\'), QLatin1Char('-'));
        setNote(QStringLiteral("已找到 %1，开始下载...").arg(assetName));
        startDownload(key, QStringLiteral("apk"), QUrl(assetUrl));
    });
}

void RootPatchDialog::onDownloadFinished(const QString &key, const QString &filePath)
{
    const QString field = m_keyTarget.take(key);
    if (field.isEmpty())
        return; // 非本对话框发起的下载（防御）
    if (m_activeDownloads > 0)
        --m_activeDownloads;
    if (m_activeDownloads == 0)
        setBusy(false);
    if (field == QLatin1String("apk")) {
        m_apkEdit->setText(QDir::toNativeSeparators(filePath));
        setNote(QStringLiteral("注入物已就绪: %1").arg(filePath));
    } else if (field == QLatin1String("ko")) {
        m_koEdit->setText(QDir::toNativeSeparators(filePath));
        setNote(QStringLiteral("注入物已就绪: %1").arg(filePath));
    }
}

void RootPatchDialog::onDownloadFailed(const QString &key, const QString &error)
{
    const QString field = m_keyTarget.take(key);
    if (field.isEmpty())
        return;
    if (m_activeDownloads > 0)
        --m_activeDownloads;
    if (m_activeDownloads == 0)
        setBusy(false);
    Q_UNUSED(field);
    setNote(QStringLiteral("下载失败: %1（可手动指定注入物）").arg(error));
}

void RootPatchDialog::onOkClicked()
{
    const patcher::RootType type = currentType();
    QString missing;
    if (isMagiskFamily(type)) {
        if (m_apkEdit->text().trimmed().isEmpty())
            missing = QStringLiteral("Magisk 系需要注入 APK（可下载或手动指定）");
    } else if (isKsuFamily(type)) {
        if (m_apkEdit->text().trimmed().isEmpty())
            missing = QStringLiteral("KernelSU 系需要 ksuinit init wrapper（可下载或手动指定）");
        else if (m_koEdit->text().trimmed().isEmpty())
            missing = QStringLiteral("KernelSU 系需要内核模块 .ko（可下载或手动指定）");
    } else if (type == patcher::RootType::APatch || type == patcher::RootType::KernelPatch) {
        if (m_kpatchEdit->text().trimmed().isEmpty())
            missing = QStringLiteral("APatch/KernelPatch 需要指定含 kptools*/kpimg* 的解压目录");
    } else if (type == patcher::RootType::RamdiskSu) {
        if (m_suZipEdit->text().trimmed().isEmpty())
            missing = QStringLiteral("RamdiskSu 需要 SuperSU 刷入包 zip");
    } else if (type == patcher::RootType::ModuleInstall) {
        if (m_moduleZipEdit->text().trimmed().isEmpty())
            missing = QStringLiteral("ModuleInstall 需要模块 zip");
    }
    if (!missing.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("注入物缺失"), missing);
        return;
    }

    // 组装修补配置（各 patcher 按 type 取用相关字段，其余忽略）
    m_cfg = patcher::PatchConfig{};
    m_cfg.type = type;
    m_cfg.variant = variantFor(type);
    m_cfg.apkPath = m_apkEdit->text().trimmed();
    m_cfg.koPath = m_koEdit->text().trimmed();
    m_cfg.kpatchPath = m_kpatchEdit->text().trimmed();
    m_cfg.suZipPath = m_suZipEdit->text().trimmed();
    m_cfg.moduleZipPath = m_moduleZipEdit->text().trimmed();
    // 留空 → 修补时从 boot 镜像自动检测（kernelsu_patcher.cpp detectKmi）
    m_cfg.deviceKmi = m_kmiCombo->currentText().trimmed();
    QDialog::accept();
}

void RootPatchDialog::setBusy(bool busy)
{
    m_progress->setVisible(busy);
    // 下载期间冻结方案切换与输入（防 key↔字段错位）
    for (QRadioButton *btn : std::as_const(m_typeButtons))
        btn->setEnabled(!busy);
    m_apkBrowseBtn->setEnabled(!busy);
    m_apkDownloadBtn->setEnabled(!busy);
    m_koBrowseBtn->setEnabled(!busy);
    m_koDownloadBtn->setEnabled(!busy);
    m_kpatchBrowseBtn->setEnabled(!busy);
    m_suZipBrowseBtn->setEnabled(!busy);
    m_moduleZipBrowseBtn->setEnabled(!busy);
    m_kmiCombo->setEnabled(!busy);
    if (m_okBtn)
        m_okBtn->setEnabled(!busy); // 下载期间禁"开始修补"（字段未就绪）
}

void RootPatchDialog::setNote(const QString &text)
{
    m_noteLabel->setText(text);
}

QString RootPatchDialog::defaultNote(patcher::RootType type) const
{
    switch (type) {
    case patcher::RootType::Magisk:
        return QStringLiteral(
            "注入 APK 可下载（官方 release）或手动指定；修补后由 magiskinit 接管 "
            "init，完整 overlay.d/fstab 处理请用 Magisk App 完成。");
    case patcher::RootType::MagiskAlpha:
        return QStringLiteral("Magisk Alpha 分支（vvb2060）；下载资产为 app-release.apk。");
    case patcher::RootType::Kitsune:
        return QStringLiteral("Kitsune 官方下载源已下线（2026-08 验证），须手动指定 APK。");
    case patcher::RootType::KernelSU:
        return QStringLiteral(
            "需要内核模块 (.ko) 与 ksuinit wrapper；KMI 留空则修补时从 boot 镜像自动检测。");
    case patcher::RootType::KernelSU_Next:
        return QStringLiteral(
            "需要内核模块 (.ko) 与 ksuinit wrapper；Next 无独立 ksuinit 资产，下载将回退 "
            "官方 KernelSU 版本（fork 定制行为不保证）。");
    case patcher::RootType::SukiSU:
        return QStringLiteral(
            "需要内核模块 (.ko) 与 ksuinit wrapper；SukiSU 无独立 ksuinit 资产，下载将回退 "
            "官方 KernelSU 版本（fork 定制行为不保证）。");
    case patcher::RootType::ReSukiSU:
        return QStringLiteral(
            "ko 下载为 lkm-all.zip（含全部 7 个 KMI 模块，按 KMI 提取）；ksuinit 下载为 "
            "ksuinit.zip（解压提取）。");
    case patcher::RootType::APatch:
        return QStringLiteral(
            "APatch 管理器 APK 内 libkptools.so 为 Android bionic ELF，PC 无法执行（后端门禁）→ "
            "须手动指定含 kptools*/kpimg* 的解压目录（KernelPatch release 预编译资产）。");
    case patcher::RootType::KernelPatch:
        return QStringLiteral(
            "KernelPatch 直接内核补丁；同样须手动指定含 kptools*/kpimg* 的解压目录。");
    case patcher::RootType::RamdiskSu:
        return QStringLiteral(
            "SuperSU recovery 刷入包（zip，含 update-binary 与 su 二进制）；老设备专用"
            "（SAR/system-as-root 布局不适用）。");
    case patcher::RootType::ModuleInstall:
        return QStringLiteral(
            "模块 zip 经后端 ModuleInstaller 输出模块包而非 boot 镜像；patchFile 拒绝该类型"
            "（UI 接线 ModuleInstaller 为后端待办），当前点击修补将返回后端明确提示。");
    }
    return QString();
}

} // namespace

ImageToolPanel::ImageToolPanel(QWidget *parent)
    : QWidget(parent)
    , m_fileLabel(nullptr)
    , m_formatLabel(nullptr)
    , m_detailLabel(nullptr)
    , m_backBtn(nullptr)
    , m_unpackBtn(nullptr)
    , m_packBtn(nullptr)
    , m_convertBtn(nullptr)
    , m_patchBtn(nullptr)
    , m_browseBtn(nullptr)
    , m_progressBar(nullptr)
    , m_logOutput(nullptr)
{
    setupUI();
    setupConnections();
    appendLog(QStringLiteral("提示: 将镜像文件拖入本面板，或点击「浏览...」选择文件"));
}

ImageToolPanel::~ImageToolPanel() = default;

void ImageToolPanel::setupUI()
{
    setAcceptDrops(true);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // 顶部：标题 + 返回
    QHBoxLayout *headerLayout = new QHBoxLayout();
    QLabel *title = new QLabel(QStringLiteral("镜像工具"), this);
    title->setStyleSheet("font-size: 16px; font-weight: bold;");
    m_backBtn = new QPushButton(QStringLiteral("← 返回"), this);
    m_backBtn->setMaximumWidth(80);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    headerLayout->addWidget(m_backBtn);
    mainLayout->addLayout(headerLayout);

    // 镜像信息卡（detectFinished 回来更新）
    QGroupBox *infoGroup = new QGroupBox(QStringLiteral("镜像信息"), this);
    QFormLayout *infoLayout = new QFormLayout(infoGroup);
    m_fileLabel = new QLabel(QStringLiteral("（拖入镜像文件或点击浏览）"), this);
    m_fileLabel->setWordWrap(true);
    m_formatLabel = new QLabel(QStringLiteral("—"), this);
    m_detailLabel = new QLabel(QStringLiteral("—"), this);
    m_detailLabel->setWordWrap(true);
    infoLayout->addRow(QStringLiteral("文件:"), m_fileLabel);
    infoLayout->addRow(QStringLiteral("格式:"), m_formatLabel);
    infoLayout->addRow(QStringLiteral("详情:"), m_detailLabel);
    mainLayout->addWidget(infoGroup);

    // 动作按钮组（按识别结果动态 enable）
    QGroupBox *actionGroup = new QGroupBox(QStringLiteral("操作"), this);
    QHBoxLayout *actionLayout = new QHBoxLayout(actionGroup);
    m_unpackBtn = new QPushButton(QStringLiteral("解包"), this);
    m_packBtn = new QPushButton(QStringLiteral("打包"), this);
    m_convertBtn = new QPushButton(QStringLiteral("转换"), this);
    m_patchBtn = new QPushButton(QStringLiteral("修补"), this);
    m_browseBtn = new QPushButton(QStringLiteral("浏览..."), this);
    m_unpackBtn->setEnabled(false);
    m_packBtn->setEnabled(false);
    m_convertBtn->setEnabled(false);
    m_patchBtn->setEnabled(false);
    actionLayout->addWidget(m_unpackBtn);
    actionLayout->addWidget(m_packBtn);
    actionLayout->addWidget(m_convertBtn);
    actionLayout->addWidget(m_patchBtn);
    actionLayout->addStretch();
    actionLayout->addWidget(m_browseBtn);
    mainLayout->addWidget(actionGroup);

    // 进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    mainLayout->addWidget(m_progressBar);

    // 日志区（撑满）
    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setPlaceholderText(QStringLiteral("操作日志"));
    mainLayout->addWidget(m_logOutput, 1);
}

void ImageToolPanel::setupConnections()
{
    connect(m_backBtn, &QPushButton::clicked, this, &ImageToolPanel::switchToDeviceInfo);
    connect(m_browseBtn, &QPushButton::clicked, this, &ImageToolPanel::onBrowseClicked);
    connect(m_unpackBtn, &QPushButton::clicked, this, &ImageToolPanel::onUnpackClicked);
    connect(m_packBtn, &QPushButton::clicked, this, &ImageToolPanel::onPackClicked);
    connect(m_convertBtn, &QPushButton::clicked, this, &ImageToolPanel::onConvertClicked);
    connect(m_patchBtn, &QPushButton::clicked, this, &ImageToolPanel::onPatchClicked);

    // worker（工作线程）→ 面板（UI 线程），跨线程自动 QueuedConnection
    connect(&m_worker, &ImageWorker::detectFinished,
            this, &ImageToolPanel::onDetectFinished);
    connect(&m_worker, &ImageWorker::unpackFinished,
            this, &ImageToolPanel::onUnpackFinished);
    connect(&m_worker, &ImageWorker::packFinished,
            this, &ImageToolPanel::onPackFinished);
    connect(&m_worker, &ImageWorker::convertFinished,
            this, &ImageToolPanel::onConvertFinished);
    connect(&m_worker, &ImageWorker::patchFinished,
            this, &ImageToolPanel::onPatchFinished);
    connect(&m_worker, &ImageWorker::progress,
            this, &ImageToolPanel::onWorkerProgress);
}

void ImageToolPanel::dragEnterEvent(QDragEnterEvent *event)
{
    // 接受带本地文件 URL 的拖入（至少 1 个本地路径；isLocalFile 规范判定）
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void ImageToolPanel::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    // 取第一个本地文件路径
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (!path.isEmpty()) {
            event->acceptProposedAction();
            startDetect(path);
            return;
        }
    }
    event->ignore();
}

void ImageToolPanel::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"));
    if (!path.isEmpty())
        startDetect(path);
}

void ImageToolPanel::startDetect(const QString &path)
{
    if (path.isEmpty())
        return;

    m_currentFile = path;
    m_detected = imgreg::Detected{};
    m_fileLabel->setText(QFileInfo(path).fileName());
    m_formatLabel->setText(QStringLiteral("识别中..."));
    m_detailLabel->setText(QStringLiteral("正在读取文件头..."));
    m_unpackBtn->setEnabled(false);
    m_packBtn->setEnabled(false);
    m_convertBtn->setEnabled(false);
    m_patchBtn->setEnabled(false);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QString()); // 清理上次操作的 format 残留
    m_progressBar->setVisible(true);
    appendLog(QStringLiteral("开始识别: %1").arg(path));

    m_worker.runDetect(path); // 工作线程执行，UI 不阻塞
}

void ImageToolPanel::onDetectFinished(const QString &path, const imgreg::Detected &detected)
{
    m_detected = detected;
    m_formatLabel->setText(formatName(detected.format));
    m_detailLabel->setText(detected.detail.isEmpty() ? QStringLiteral("—") : detected.detail);
    updateButtonsFor(detected);

    if (detected.format == imgreg::Format::Unknown) {
        const QString reason = detected.detail.isEmpty()
            ? QStringLiteral("无法识别的文件格式")
            : detected.detail;
        appendLog(QStringLiteral("识别失败: %1").arg(reason), true);
        return;
    }
    appendLog(QStringLiteral("已识别: %1 → %2")
                  .arg(QFileInfo(path).fileName(), formatName(detected.format)));
}

void ImageToolPanel::updateButtonsFor(const imgreg::Detected &detected)
{
    const imgreg::Format f = detected.format;
    // 解包：容器/分区类镜像（纯压缩流不直接解包；Raw 本身即已解包格式）
    m_unpackBtn->setEnabled(f == imgreg::Format::Payload || f == imgreg::Format::Zip ||
                            f == imgreg::Format::Tar || f == imgreg::Format::TarMd5 ||
                            f == imgreg::Format::Sparse || f == imgreg::Format::Super ||
                            f == imgreg::Format::Boot || f == imgreg::Format::VendorBoot ||
                            f == imgreg::Format::Vbmeta || f == imgreg::Format::Dtb ||
                            f == imgreg::Format::Dat || f == imgreg::Format::Pac ||
                            f == imgreg::Format::Kdz || f == imgreg::Format::UpdateApp ||
                            f == imgreg::Format::UpdateBin || f == imgreg::Format::Sin ||
                            f == imgreg::Format::DiskGpt || f == imgreg::Format::TwrpWin ||
                            f == imgreg::Format::Erofs || f == imgreg::Format::Ext4);
    // 打包（D6 接线）：sparse→raw、镜像集→tar、payload 全量等
    m_packBtn->setEnabled(f == imgreg::Format::Sparse || f == imgreg::Format::RawImage ||
                          f == imgreg::Format::Tar || f == imgreg::Format::Payload);
    // 转换（D3 接线）：sparse↔raw
    m_convertBtn->setEnabled(f == imgreg::Format::Sparse || f == imgreg::Format::RawImage);
    // 修补（D4 接线）：boot 类镜像
    m_patchBtn->setEnabled(f == imgreg::Format::Boot || f == imgreg::Format::VendorBoot);
}

void ImageToolPanel::onWorkerProgress(int percent, const QString &stage)
{
    if (!m_progressBar->isVisible())
        m_progressBar->setVisible(true);
    m_progressBar->setValue(percent);
    m_progressBar->setFormat(QStringLiteral("%1 %2%").arg(stage).arg(percent));
    if (percent >= 100) {
        m_progressBar->setVisible(false);
        m_progressBar->setFormat(QString()); // 清理 format 残留，下次操作从默认格式开始
    }
}

void ImageToolPanel::onUnpackClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;
    // D3 接线：弹目录选择框选输出目录（默认源文件所在目录）
    const QString outDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择解包输出目录"),
        QFileInfo(m_currentFile).absolutePath());
    if (outDir.isEmpty()) {
        appendLog(QStringLiteral("已取消解包（未选择输出目录）"));
        return;
    }
    appendLog(QStringLiteral("开始解包: %1 → %2").arg(m_currentFile, outDir));
    m_worker.runUnpack(m_currentFile, outDir, m_detected);
}

void ImageToolPanel::onPackClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;
    appendLog(QStringLiteral("开始打包: %1").arg(m_currentFile));
    // D6 接线：输出路径/输入清单由后续任务提供；当前为占位参数
    m_worker.runPack(m_currentFile + QStringLiteral(".tar"),
                     QStringList{m_currentFile}, m_detected);
}

void ImageToolPanel::onConvertClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;

    // D3 接线：目标类型选择（当前仅 sparse↔raw 两向，菜单内单选）
    const bool toSparse = (m_detected.format == imgreg::Format::RawImage);
    QMenu menu(this);
    menu.addAction(toSparse ? QStringLiteral("转换为 Sparse 镜像 (img2simg)")
                            : QStringLiteral("转换为 Raw 镜像 (simg2img)"));
    QAction *chosen = menu.exec(m_convertBtn->mapToGlobal(QPoint(0, m_convertBtn->height() + 2)));
    if (!chosen) {
        appendLog(QStringLiteral("已取消转换（未选择目标类型）"));
        return;
    }

    const QString base = QFileInfo(m_currentFile).completeBaseName();
    const QString suggested = QFileInfo(m_currentFile).absolutePath() + QLatin1Char('/')
        + base + (toSparse ? QStringLiteral(".sparse") : QStringLiteral(".raw"));
    const QString outPath = QFileDialog::getSaveFileName(
        this, toSparse ? QStringLiteral("选择 Sparse 输出文件")
                       : QStringLiteral("选择 Raw 输出文件"),
        suggested);
    if (outPath.isEmpty()) {
        appendLog(QStringLiteral("已取消转换（未选择输出文件）"));
        return;
    }

    appendLog(QStringLiteral("开始转换: %1 → %2").arg(m_currentFile, outPath));
    m_worker.runConvert(m_currentFile, outPath, m_detected);
}

void ImageToolPanel::onPatchClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;

    // D4 接线：方案选择对话框（11 种 RootType 分组单选 + 注入物 + KMI）。
    // 用户取消则不投递；确认后 worker 在工作线程调 patcher::patchFile，
    // 结果经 patchFinished 回传（失败走 error 日志，绝不崩溃、UI 不阻塞）。
    RootPatchDialog dlg(m_currentFile, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const patcher::PatchConfig config = dlg.config();

    appendLog(QStringLiteral("开始修补: %1").arg(m_currentFile));
    appendLog(QStringLiteral("方案: %1").arg(rootTypeDisplayName(config.type)));
    if (!config.apkPath.isEmpty())
        appendLog(QStringLiteral("  apk/ksuinit: %1").arg(config.apkPath));
    if (!config.koPath.isEmpty())
        appendLog(QStringLiteral("  内核模块: %1").arg(config.koPath));
    if (!config.kpatchPath.isEmpty())
        appendLog(QStringLiteral("  kptools/kpimg 目录: %1").arg(config.kpatchPath));
    if (!config.suZipPath.isEmpty())
        appendLog(QStringLiteral("  su 包: %1").arg(config.suZipPath));
    if (!config.moduleZipPath.isEmpty())
        appendLog(QStringLiteral("  模块 zip: %1").arg(config.moduleZipPath));
    appendLog(config.deviceKmi.isEmpty()
                  ? QStringLiteral("  KMI: 自动检测（修补时从 boot 镜像读取）")
                  : QStringLiteral("  KMI: %1").arg(config.deviceKmi));
    m_worker.runPatch(m_currentFile, config);
}

void ImageToolPanel::onUnpackFinished(bool ok, const QStringList &outputs, const QString &error)
{
    // D2 吸收：收尾清理进度条（含失败路径，防卡 0%）
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (!ok) {
        appendLog(QStringLiteral("解包失败: %1").arg(error), true);
        return;
    }
    appendLog(QStringLiteral("解包完成: 共 %1 个产物").arg(outputs.size()));
    const int shown = qMin(outputs.size(), 50);
    for (int i = 0; i < shown; ++i)
        appendLog(QStringLiteral("  ✔ %1").arg(outputs.at(i)));
    if (outputs.size() > shown)
        appendLog(QStringLiteral("  ... 其余 %1 项未列出").arg(outputs.size() - shown));
}

void ImageToolPanel::onPackFinished(bool ok, const QString &output, const QString &error)
{
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (ok)
        appendLog(QStringLiteral("打包完成: %1").arg(output));
    else
        appendLog(QStringLiteral("打包失败: %1").arg(error), true);
}

void ImageToolPanel::onConvertFinished(bool ok, const QString &output, const QString &error)
{
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (ok)
        appendLog(QStringLiteral("转换完成: %1").arg(output));
    else
        appendLog(QStringLiteral("转换失败: %1").arg(error), true);
}

void ImageToolPanel::onPatchFinished(bool ok, const QString &output, const QString &error)
{
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (ok)
        appendLog(QStringLiteral("修补完成: %1").arg(output));
    else
        appendLog(QStringLiteral("修补失败: %1").arg(error), true);
}

void ImageToolPanel::appendLog(const QString &msg, bool isError)
{
    if (msg.isEmpty()) {
        m_logOutput->insertHtml(QStringLiteral("<br>"));
        m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    QString color = isError ? QStringLiteral("#e74c3c") : QStringLiteral("#2c3e50");
    if (msg.contains(QStringLiteral("✔")))
        color = QStringLiteral("#27ae60");
    else if (msg.contains(QStringLiteral("✘")))
        color = QStringLiteral("#e74c3c");
    else if (msg.contains(QStringLiteral("⚠")))
        color = QStringLiteral("#e67e22");

    m_logOutput->insertHtml(QStringLiteral("<span style='color: %1;'>%2%3</span><br>")
                                .arg(color, timestamp, msg.toHtmlEscaped()));
    m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
    emit outputMessage(msg, isError);
}

// RootPatchDialog（.cpp 内 Q_OBJECT 私有类）的 moc 元数据：
// AUTOMOC 要求 .cpp 内 Q_OBJECT 类以 #include "<basename>.moc" 收尾，
// 使 moc 代码与本文件同 TU 编译（匿名命名空间类链接可见性要求）。
#include "image_tool_panel.moc"

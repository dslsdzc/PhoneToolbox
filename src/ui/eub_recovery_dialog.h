// src/ui/eub_recovery_dialog.h
//
// EUB 救援对话框（设计 spec §D9/§D10）：识别设备 → 选 sboot 来源 → 段表 + sha1 对照预览 →
// 勾选"未验证"确认 → 开始 → 进度日志 → 交接提示（设备应已进入 Download 模式，请继续刷写）。
// 载荷一律由用户自备（facts §F8），本对话框不内置任何三星二进制。
// 表项带 extraFiles 的 SoC（9830：ldfw.img/tzsw.img，facts §C7）由**同一个 BL 包**供给 —— 预检时
// 就从用户选中的来源里取出（取不到即预检失败、指引改选 BL 包），段与额外文件的发送都由会话层按序完成。
//
// ⚠️ 真机路径未验证：本机无任何 Exynos 设备（facts §F1）。本对话框的**离线可测面** = 两个纯函数
// （段表/摘要）+ 注入 mock 传输后的识别与预检；真机时序（枚举/claim/回显）留持机人。
#pragma once
#include <QDialog>
#include <QString>
#include <functional>

#include "core/eub/eub_loadout.h"
#include "core/eub/eub_session.h"
#include "core/eub/eub_transport.h"

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class EubRecoveryDialog : public QDialog
{
    Q_OBJECT

public:
    // transport 由调用方持有（真机 = LibusbEubTransport；用例 = MockEubTransport）。
    // **无默认空参**：本对话框不 new 真机传输 —— UI 层只依赖抽象，真机实现由 FlashPanel 侧构造
    // 并注入（依赖契约由构建图钉住：CMakeLists 的 test_eub_recovery_dialog 分支不含任何 libusb 源）。
    explicit EubRecoveryDialog(eub::IEubTransport &transport, QWidget *parent = nullptr);

    // 主窗口日志汇（FlashPanel 接到 outputMessage）。会话的进度回调也走这条线（见 .cpp 构造）。
    void setLogSink(const std::function<void(const QString &, bool)> &sink) { m_logSink = sink; }

    // 识别 + 载荷预检 + 查表 + 切段试算（"选择文件之后"的同一段逻辑；用例直接调它）。
    // 失败时 *error 为可行动的中文文案；m_prepared 保持 false（门控不开）。
    // SoC 名为空时用 detectSocFromImage 兜底再查一次表（facts §A4，见 .cpp）。
    // sourcePath = 用户选中的**来源文件**（onPickSource 传 QFileDialog 的返回值；用例传合成包/裸文件）。
    // 表项带 extraFiles（9830）时本函数必须能从**同一个来源**里取出这些条目（不新增第二个文件选择器，
    // spec §D8 的既定取舍）：来源不是 tar（裸 sboot.bin/.lz4）→ 失败并指引改选 BL_*.tar.md5；
    // 是 tar 但缺条目 → 失败，*error 里含缺失名与包内条目（来自 eub::loadNamedEntriesFromTar）。
    // 取出成功 → m_extras 就绪（顺序与 m_loadout.extraFiles 一一对应），勾选确认后即可开始。
    bool prepare(const QByteArray &sboot, const QString &sourcePath,
                 const QString &sourceDescription, QString *error);

    const eub::EubLoadout &loadout() const { return m_loadout; }
    // 设备/表/对照的**只读摘要**（= 设备标签 + sha1 标签两行）。未预检时返回"未就绪"提示。
    QString statusText() const;
    // 仅用例用：返回**真实按钮控件**的 enabled（不是另算一份门控表达式 —— 否则"忘了刷新按钮"
    // 的实现也能过门控用例）。
    bool isStartEnabledForTest() const;

    // 纯函数（离线可测）
    // extras = 已从 BL 包取出的额外文件载荷（按 lo.extraFiles 顺序一一对应，契约同 EubSession::run）；
    // 传空表 = 未加载形态（只列名字、不给字节数）—— 字节数一律取自**实际取出的载荷**，不硬编。
    static QString segmentTableText(const eub::EubLoadout &lo,
                                    const QList<QByteArray> &extras = QList<QByteArray>());
    static QString sha1CompareText(const QByteArray &tableSha1, const QByteArray &fileSha1);
    // 救援成功后的交接文案（facts §D1/§F5：只发 RAM 镜像、**未写任何存储**，设备仍需正常刷写）。
    // 抽成纯函数是为了让这段**强制文案**能被离线钉住：成功路径只有模态弹窗这一个展示面，
    // 用例没法（也不该）去点模态框。
    // withExtraFiles（9830）：本次在分段之后还另发了额外文件 —— 成功文案不得**少报**已发生的动作
    //（与 I1"不得多报"是同一条原则的另一半）。
    static QString rescueSuccessText(bool withExtraFiles = false);

private slots:
    void onPickSource();
    void onStart();

private:
    void log(const QString &line, bool isError = false);
    void refreshStartEnabled();
    // 额外文件是否就绪：数量与表名单一致（内容非空由 eub::loadNamedEntriesFromTar 保证）。
    // 与 EubSession::run 的入口校验同一条判据的**提前版** —— 这里挡住能让用户看见"为什么不能开始"，
    // 而不是点了「开始救援」才吃一次必然失败的发送。
    bool extrasReady() const;
    // 段表/sha1 两处展示的刷新（prepare 的成功与早退路径都要走到，避免留上一次的旧值）
    void refreshPreview(const QString &deviceLine, const QString &sourceDescription);

    eub::IEubTransport &m_transport;
    eub::EubOptions      m_opt;
    eub::EubSession      m_session;
    std::function<void(const QString &, bool)> m_logSink;

    QLabel         *m_deviceLabel  = nullptr;
    QLabel         *m_sourceLabel  = nullptr;
    QLabel         *m_shaLabel     = nullptr;
    QPlainTextEdit *m_segmentView  = nullptr;
    QPlainTextEdit *m_logView      = nullptr;
    QCheckBox      *m_confirmBox   = nullptr;
    QPushButton    *m_startBtn     = nullptr;

    QByteArray       m_sboot;
    // 表项带 extraFiles（9830）时，prepare 从**同一个 BL 包**取出的额外文件载荷（顺序与
    // m_loadout.extraFiles 一一对应）；表项不带该字段时恒为空表。取值只做一次（预检时），
    // 「开始救援」直接用这份内存里的数据 —— 用户不必担心包在中途被移走。
    QList<QByteArray> m_extras;
    eub::EubLoadout  m_loadout;
    bool             m_prepared = false;
};

#include <QApplication>
#include <QPalette>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDebug>
#include "ui/main_window.h"
#include "ui/agreement_dialog.h"     // W2-T5：首屏使用须知（协议正文入口）
#include "legal/consent_store.h"     // W2-T5：留痕（本机 QSettings，不联网）

// TMPDIR 指向不可写分区时（常见：TMPDIR 指到快满的盘），Qt/GTK 运行中写
// 临时文件会失败。GTK 的图标加载甚至直接断言 abort —— 用户看到的就是
// "启动就崩"。这里在 QApplication 之前实测一次，不可写就回退。
static void ensureWritableTempDir()
{
    const QString current = QDir::tempPath();
    QTemporaryFile probe(current + QStringLiteral("/phonetoolbox-write-probe-XXXXXX"));
    if (probe.open()) {
        probe.close(); // QTemporaryFile 析构时自动删除
        return;
    }

    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    const QString fallback =
        (!runtime.isEmpty() && QFileInfo(runtime).isWritable())
            ? runtime
            : QStringLiteral("/tmp");

    qputenv("TMPDIR", fallback.toLocal8Bit());
    qWarning() << "TMPDIR is not writable:" << current
               << "- falling back to" << fallback;
}

static void applyDarkTheme(QApplication &app)
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(30, 30, 30));
    p.setColor(QPalette::WindowText, Qt::white);
    p.setColor(QPalette::Base, QColor(42, 42, 42));
    p.setColor(QPalette::AlternateBase, QColor(50, 50, 50));
    p.setColor(QPalette::ToolTipBase, QColor(40, 40, 40));
    p.setColor(QPalette::ToolTipText, Qt::white);
    p.setColor(QPalette::Text, Qt::white);
    p.setColor(QPalette::Button, QColor(45, 45, 45));
    p.setColor(QPalette::ButtonText, Qt::white);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, QColor(42, 130, 218));
    p.setColor(QPalette::Highlight, QColor(42, 130, 218));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
    app.setPalette(p);
}

int main(int argc, char *argv[])
{
    // 必须在 QApplication / GTK 主题插件初始化之前。
    ensureWritableTempDir();

    QApplication app(argc, argv);

    // 设置应用信息
    app.setApplicationName("Phone Toolbox");
    app.setApplicationVersion("0.0.1-beta03");
    app.setOrganizationName("PhoneToolbox");

    // 设置应用程序样式
    app.setStyle("Fusion");
    applyDarkTheme(app);

    // W2-T5：首屏「使用须知」—— 本版本未接受则**不进入主界面**（用户拒绝即退出）。
    // 位置在 MainWindow 构造之前：主窗口一构造就会开始轮询设备，门控必须先于任何设备动作。
    // 留痕落本机 QSettings（见 legal/consent_store.h）；acceptAgreement 只在用户点了同意后才写。
    legal::ConsentStore store;
    if (!store.hasAcceptedAgreement()) {
        if (!agreementui::ask(nullptr))
            return 0;
        store.acceptAgreement();
    }

    MainWindow window;
    window.show();

    return app.exec();
}

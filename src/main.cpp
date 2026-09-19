#include <QApplication>
#include <QPalette>
#include "ui/main_window.h"
#include "ui/agreement_dialog.h"     // W2-T5：首屏使用须知（协议正文入口）
#include "legal/consent_store.h"     // W2-T5：留痕（本机 QSettings，不联网）

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

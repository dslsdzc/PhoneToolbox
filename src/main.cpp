#include <QApplication>
#include <QPalette>
#include "ui/main_window.h"

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
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("PhoneToolbox");

    // 设置应用程序样式
    app.setStyle("Fusion");
    applyDarkTheme(app);

    MainWindow window;
    window.show();

    return app.exec();
}

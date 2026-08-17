#pragma once

#include <QList>
#include <QWidget>

class ProtocolPlugin;

// 插件面板：列出已加载插件及其能力，点击执行（参数为空）。
// 复用现有面板信号模式：outputMessage(QString, bool) 发到 OutputPanel。
class PluginToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit PluginToolPanel(QWidget *parent = nullptr);
    void setPlugins(const QList<ProtocolPlugin *> &plugins);

signals:
    void outputMessage(const QString &text, bool isError);

private slots:
    void onExecuteClicked();

private:
    class QListWidget *m_list;
    class QPushButton *m_executeBtn;
    QList<ProtocolPlugin *> m_plugins;
};

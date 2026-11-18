#pragma once

#include <QList>
#include <QWidget>

#include "src/plugins/plugin_manager.h"

class ProtocolPlugin;

// 插件面板：展示**每个插件文件的扫描结果**（已加载 / 加载失败 + 原因）。
//
// 有意不提供"执行"按钮：插件能力（如华为刷写）需要业务参数，本来就由对应
// 流程自动调用；在一个通用列表里点执行只会得到"缺少参数"的失败，误导用户。
// 这里回答的是"插件有没有启动成功、为什么失败"。
class PluginToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit PluginToolPanel(QWidget *parent = nullptr);
    void setRecords(const QList<PluginRecord> &records);

private:
    class QTableWidget *m_table;
    class QLabel *m_summary;
    class QLabel *m_hint;
};

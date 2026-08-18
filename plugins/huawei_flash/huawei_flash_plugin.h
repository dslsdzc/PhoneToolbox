#pragma once

#include <QObject>

#include "src/plugins/plugin_interface.h"

// 华为刷写插件入口（协议层 F2-1/2/3 已填充；本文件仅插件接口适配）
class HuaweiFlashPlugin : public ProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ProtocolPlugin_iid)
    Q_INTERFACES(ProtocolPlugin)
public:
    explicit HuaweiFlashPlugin(QObject *parent = nullptr) : ProtocolPlugin(parent) {}

    QString name() const override { return QStringLiteral("华为刷写"); }
    QString description() const override
    {
        // 机型范围诚实标注：支持 Kirin 系芯片 USB Update 模式（实测前不承诺具体机型）
        return QStringLiteral("华为 Kirin USB Update 刷写通道（update.app 集成）；"
                              "支持 Kirin 系芯片 USB Update 模式（实测前不承诺具体机型）");
    }
    QStringList capabilities() const override
    {
        return { QStringLiteral("huawei-usb-update.flash") };
    }
    bool execute(const QString &capability, const QVariantMap &params, QString *error) override;
};

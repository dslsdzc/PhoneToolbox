#include "huawei_flash_plugin.h"

#include "hisi_flash.h"
#include "update_app.h"

bool HuaweiFlashPlugin::execute(const QString &capability, const QVariantMap &params,
                                QString *error)
{
    if (capability != QStringLiteral("huawei-usb-update.flash")) {
        if (error) *error = QStringLiteral("未知能力: %1").arg(capability);
        return false;
    }
    const QString updateApp = params.value(QStringLiteral("updateApp")).toString();
    if (updateApp.isEmpty()) {
        if (error) *error = QStringLiteral("缺少 updateApp 参数（update.app 路径）");
        return false;
    }
    return hisi::runHisiFlash(updateApp, nullptr, error);
}

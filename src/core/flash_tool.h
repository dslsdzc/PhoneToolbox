#ifndef FLASH_TOOL_H
#define FLASH_TOOL_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QVariantMap>

#include "modes/edl_handler.h"
#include "modes/mtk_handler.h"
#include "device_detector.h"

class FlashTool : public QObject
{
    Q_OBJECT

public:
    explicit FlashTool(QObject *parent = nullptr);

    // 列出所有分区 (解析 fastboot getvar all)
    QStringList listPartitions(const QString &deviceId);

    // 获取分区信息
    struct PartitionInfo {
        QString name;
        QString type;
        QString size;
    };
    QList<PartitionInfo> getPartitionDetails(const QString &deviceId);

    // 通过 ADB 列出分区 (adb shell ls /dev/block/by-name/)
    QStringList listPartitionsAdb(const QString &deviceId);
    QList<PartitionInfo> getPartitionDetailsAdb(const QString &deviceId);

    // 写入分区 (fastboot flash)
    bool flashPartition(const QString &deviceId, const QString &partition,
                        const QString &imagePath);

    // ADB root 刷入分区 (adb push + dd)
    bool adbFlashPartition(const QString &deviceId, const QString &partition,
                           const QString &imagePath);

    // 擦除分区
    bool erasePartition(const QString &deviceId, const QString &partition);

    // 格式化分区 (fastboot format <partition>)
    bool formatPartition(const QString &deviceId, const QString &partition);

    // 更新设备 (fastboot update zip)
    bool flashUpdate(const QString &deviceId, const QString &zipPath);

    // ADB Sideload (需要在 Recovery 模式下)
    bool adbSideload(const QString &deviceId, const QString &zipPath);

    // 运行刷机脚本 (自动识别平台)
    bool runFlashScript(const QString &scriptPath, const QString &deviceId = "");

    // 读取分区内容 (ADB dd + pull, 根据模式选择不同策略)
    // deviceMode: DeviceDetector 模式常量
    bool dumpPartition(const QString &deviceId, int deviceMode,
                       const QString &partition, const QString &outputPath);

    // 快速启动临时镜像 (fastboot boot)
    bool fastbootBoot(const QString &deviceId, const QString &imagePath);

    // Fastboot 内存转储 (fastboot oem dump-mem <addr> <size>)
    bool fastbootDumpMem(const QString &deviceId, const QString &address,
                         const QString &size, const QString &outputPath);

    // 检测 Bootloader 解锁状态 (返回: 1=已解锁, 0=已锁定, -1=未知)
    int checkBootloaderStatus(const QString &deviceId);

    // 解锁 Bootloader (fastboot oem unlock / flashing unlock)
    bool unlockBootloader(const QString &deviceId);
    // 回锁 Bootloader (fastboot oem lock / flashing lock)
    bool lockBootloader(const QString &deviceId);

    // 清除 FRP (跨模式: Fastboot erase / EDL write zero / MTK write zero)
    bool eraseFRP(const QString &deviceId, int deviceMode);

    // EDL 模式操作
    bool edlConnect(const QString &programmerPath);
    bool edlDisconnect();
    bool edlIsConnected() const;
    QList<EDLPartition> edlListPartitions();
    bool edlReadPartition(const EDLPartition &part, const QString &outputPath);
    bool edlWritePartition(const EDLPartition &part, const QString &imagePath);

    // MTK 模式操作
    bool mtkInitialize();
    bool mtkConnect();
    bool mtkDisconnect();
    bool mtkIsConnected() const;
    bool mtkSeccfgUnlock();
    bool mtkSeccfgLock();
    QList<MtkPartition> mtkListPartitions();
    bool mtkReadPartition(const QString &partition, const QString &outputPath);
    bool mtkWritePartition(const QString &partition, const QString &imagePath);
    bool mtkReadAllPartitions(const QString &directory);

    // F5: 整包刷写入口——按设备模式分派到协议通道（MTK BROM/华为插件/展锐）。
    // params 承载通道所需文件参数（键名见各通道实现）：
    //   mtk-brom:            daPath(DA 二进制路径)(分区列表待接线)
    //   huawei-usb-update:   updateApp(update.app 路径)
    //   spd:                 pacPath + fdl1Path + fdl2Path
    // 失败返回 false 并填 error；插件缺失时明确提示。
    bool flashFullPackage(const QString &deviceId, DeviceDetector::DeviceMode mode,
                          const QVariantMap &params, QString *error);

    // 纯函数（单测）：设备模式 → 协议通道名；非协议模式返回空串
    static QString flashChannelForMode(DeviceDetector::DeviceMode mode);

signals:
    void outputMessage(const QString &msg, bool isError);
    void flashProgress(int percent);

private:
    QString executeFastboot(const QStringList &args, int timeoutMs = 30000);
    QString executeFastbootWithDevice(const QString &deviceId,
                                      const QStringList &extraArgs,
                                      int timeoutMs = 30000);
    QString executeAdb(const QString &deviceId, const QStringList &args,
                       int timeoutMs = 30000);

    EDLHandler m_edlHandler;
    MtkHandler *m_mtkHandler;
};

#endif // FLASH_TOOL_H

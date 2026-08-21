#include <QtTest>

#include "core/device_detector.h"
#include "core/flash_tool.h"

class TestPipeline : public QObject {
    Q_OBJECT
private slots:
    void channelMapping();
    void channelMappingUnknown();
};

void TestPipeline::channelMapping()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_MTK_BROM),
             QStringLiteral("mtk-brom"));
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_HUAWEI_USB_UPDATE),
             QStringLiteral("huawei-usb-update"));
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_SPD),
             QStringLiteral("spd"));
}

void TestPipeline::channelMappingUnknown()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_ADB), QString());
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_FASTBOOT), QString());
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_UNKNOWN), QString());
}

QTEST_APPLESS_MAIN(TestPipeline)
#include "test_pipeline.moc"

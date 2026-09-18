// src/ui/flash_button_labels.cpp
//
// 「刷入」按钮的 (文案, tooltip) 逐模式映射 —— 唯一来源。文案逐字取自 FlashPanel 收口前的现文
// （src/ui/flash_panel.cpp 的 setDeviceInfo），期望值另有一份独立副本钉在
// tests/test_flash_button_labels.cpp 里：实现改字、用例没跟着改 → 红。
#include "ui/flash_button_labels.h"

namespace flashui {

ButtonLabels flashButtonLabelsFor(DeviceDetector::DeviceMode mode)
{
    // 无 default 分支是**故意的**：新增枚举值时 -Wswitch 会点名未处理的模式，且它会落到函数末尾的
    // 复位态（安全默认：宁可什么都不承诺，也不给错承诺）。新增协议模式必须同时改这里与用例的期望表。
    switch (mode) {
    case DeviceDetector::MODE_SAMSUNG_EUB:
        // 救援入口：文案与 tooltip 成对，缺一不可（I3 就是只复位 text 留下的"救援提示 + 写入动作"组合）。
        return {QStringLiteral("EUB 救援…"),
                QStringLiteral("EUB 救援：用你自备的原厂 BL（sboot.bin / BL_*.tar.md5）按 SoC 布局表分段注入设备 RAM，"
                               "把设备引导进 Download 模式；本流程只发 RAM 镜像、不写存储，完成后请继续用三星刷写")};
    case DeviceDetector::MODE_MTK_BROM:
        return {QStringLiteral("刷入"),
                QStringLiteral("协议通道按计划刷写：DA + 镜像 → 计划预览 → 按设备代际自动选择 "
                               "LEGACY / XFLASH / XML 链（三代均已实现，实际链路见日志「代际判定」）")};
    case DeviceDetector::MODE_HUAWEI_USB_UPDATE:
    case DeviceDetector::MODE_SPD:
    case DeviceDetector::MODE_SAMSUNG_ODIN:
        return {QStringLiteral("刷入"),
                QStringLiteral("协议通道整包/按计划刷写（按模式选择 update.app / pac+FDL / DA+镜像 / 三星 tar.md5）")};

    // 非协议模式：文案与 tooltip **一起**回复位态（tooltip 空 = 无提示）。
    // I3：只复位其一会让按钮顶着别的模式的承诺执行当前模式的动作 —— 例如 fastboot 下执行
    // **不可逆的分区写入**、提示却还写着"只发 RAM 镜像、不写存储"。
    case DeviceDetector::MODE_UNKNOWN:
    case DeviceDetector::MODE_ADB:
    case DeviceDetector::MODE_FASTBOOT:
    case DeviceDetector::MODE_FASTBOOTD:
    case DeviceDetector::MODE_EDL_9008:
    case DeviceDetector::MODE_MTK_DA:
    case DeviceDetector::MODE_RECOVERY:
        break;
    }

    // 复位态 / 未登记的枚举值。
    return {QStringLiteral("刷入"), QString()};
}

} // namespace flashui

// src/ui/flash_button_labels.h
//
// FlashPanel「刷入」按钮的 (文案, tooltip) 纯函数 —— EUB backlog Task 3。
//
// 为什么单独抽出来：flash_panel.cpp 的依赖闭包过大（FlashTool/AdbEmbedded/各协议通道/libusb），
// 仓内没有任何 FlashPanel 用例，于是"模式 → 按钮文案/tooltip"这类组合缺陷只能靠读码发现 ——
// I3 就是这么漏过去的：按钮只复位了 text、没复位 tooltip，EUB 设备离开后普通 fastboot 设备顶着
// 「EUB 救援…」+「只发 RAM 镜像、不写存储」的提示执行**不可逆的分区写入**
// （.superpowers/sdd/eub-final-fix-report.md §I3）。本函数只依赖 DeviceDetector::DeviceMode，
// 不碰 Widgets/libusb —— 于是组合可以被普通用例逐枚举钉住（tests/test_flash_button_labels.cpp）。
#pragma once

#include <QString>

#include "core/device_detector.h"

namespace flashui {

struct ButtonLabels {
    QString text;
    QString tooltip;   // 空 = 无提示（复位态）
};

// 模式 → 「刷入」按钮的 (文案, tooltip)。
// **唯一来源**：flash_panel.cpp 的 setDeviceInfo 只经这一处设置两者（一次无条件赋值，各模式分支
// 不再自行设置）。文案与 tooltip 必须**一起设置、一起复位** —— 只动其一会留下"文案/提示与动作
// 不符"的组合（I3 的教训）。
// 未登记的模式一律返回复位态 {"刷入", ""}（宁可什么都不承诺，也不给错承诺）；新增协议模式时
// 必须同时在本文件与 tests/test_flash_button_labels.cpp 的期望表里登记。
ButtonLabels flashButtonLabelsFor(DeviceDetector::DeviceMode mode);

} // namespace flashui

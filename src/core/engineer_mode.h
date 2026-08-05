#ifndef ENGINEER_MODE_H
#define ENGINEER_MODE_H

#include <QString>

// 工程模式入口映射模块（任务 E1）
//
// 每个入口均经联网搜索验证（来源见 engineer_mode.cpp 条目注释 + task-E1-report.md）。
// 原则：无法验证的入口不写入映射表；失败返回空 Entry / 合理默认，绝不崩溃。
namespace engmode {

struct Entry
{
    QString name;      // 工程模式名称（如 "MTK 工程模式"）
    QString dialCode;  // 拨号码（如 *#*#6484#*#*）
    QString activity;  // am start -n 目标（如 com.mediatek.engineermode/.EngineerMode）
    QString note;      // 来源/可靠性标注
};

// 品牌归一：ro.product.brand → 规范品牌名（xiaomi/Redmi/POCO → Xiaomi）。
// 空输入或未收录品牌返回空串（调用方据此走芯片回退/降级路径）。
QString detectBrand(const QString &roProductBrand);

// 品牌 → 入口；品牌未收录时按硬件芯片回退（MTK/高通 → 对应工程模式）；
// 全部未知返回空 Entry（note 说明"无已知入口"）。
Entry lookup(const QString &brand, const QString &hardware, const QString &model);

// 入口是否可执行：拨号码以 *# 或 ## 开头，或 Activity 为"包名/类名"组件形式。
// （Lenovo 的已验证拨号代码 ####1111# 为 ## 前缀，故放宽；详见报告。）
bool isValid(const Entry &e);

} // namespace engmode

#endif // ENGINEER_MODE_H

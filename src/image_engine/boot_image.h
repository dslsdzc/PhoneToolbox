#pragma once
#include <QByteArray>

namespace imgboot {

struct BootInfo {
    quint32 headerVersion = 0;
    quint32 pageSize = 4096;
    quint32 kernelSize = 0;
    quint32 ramdiskSize = 0;
    quint32 dtbSize = 0;
    QByteArray cmdline;
    QByteArray kernel;
    QByteArray ramdisk;
    QByteArray dtb;
    QByteArray raw; // 完整原镜像
};

bool isBootImage(const QByteArray &header);              // "ANDROID!"
bool parseBootImage(const QByteArray &raw, BootInfo &out);
QByteArray repackBootImage(const BootInfo &info);        // Task 9

} // namespace imgboot

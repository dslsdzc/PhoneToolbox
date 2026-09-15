#pragma once

// 字节数人性化（Phase C 的 planBytesText 从 UI 头抽到 core：UI 与计划层共用一份实现）
#include <QString>
#include <QtGlobal>

inline QString humanBytes(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    if (bytes >= 1024ull * 1024)
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    if (bytes >= 1024ull)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

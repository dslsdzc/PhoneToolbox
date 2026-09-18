#pragma once

// MTK 模块共用的错误出参写法（`error` 可空 —— 调用方常传 nullptr）。
//
// 为什么有本文件：这段"if (error) *error = msg"此前在 mtk_da_file.cpp / mtk_preloader_emi.cpp
// 的匿名命名空间里各写一份。它虽小，却是**失败文案的唯一出口** —— 多份副本改动时会漂移
// （其中一份忘了判空就是空指针解引用）。故收敛到本头。

#include <QString>

namespace mtkbrom {

inline void setErr(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
}

} // namespace mtkbrom

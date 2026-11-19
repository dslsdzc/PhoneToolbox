#ifndef PROCESS_RESULT_H
#define PROCESS_RESULT_H

#include <QString>
#include <QStringList>

// 外部命令（adb / fastboot / 脚本）的统一结果。
//
// 引入原因：此前 executeFastboot() 只把 stdout/stderr 拼成字符串返回，调用方
// 靠 `contains("FAILED")` 猜成败 —— 进程没启动、命令超时、退出码非 0 但输出
// 里没有 "FAILED" 时都会被当成成功。刷机场景下这是最危险的一类误判。
//
// 判据只有一条：只有「进程确实启动 + 未超时 + 退出码为 0」才算成功。
// 输出内容只用于展示与附加诊断，不参与成败判定（个别协议需要看文本时，
// 由调用方在 success() 之上再做显式检查，并在注释里写明理由）。
struct ProcessResult
{
    bool started = false;   // QProcess 是否成功启动（false = 可执行文件缺失/无权限）
    bool timedOut = false;  // 是否在超时时间内未结束（超时后进程已被 kill）
    int exitCode = -1;      // 正常结束时的退出码；未启动/超时为 -1

    QString stdoutText;
    QString stderrText;

    bool success() const
    {
        return started && !timedOut && exitCode == 0;
    }

    QString combinedOutput() const
    {
        return stdoutText + stderrText;
    }

    // 面向用户的一行失败原因。成功时返回空串。
    QString errorSummary() const
    {
        if (!started) {
            // 调用方可以在 stderrText 里预置更具体的原因（路径为空 / 初始化失败）。
            const QString why = stderrText.trimmed();
            return why.isEmpty()
                       ? QStringLiteral("进程未能启动（可执行文件缺失或不可执行）")
                       : why;
        }
        if (timedOut)
            return QStringLiteral("命令超时");
        if (exitCode != 0) {
            const QString err = stderrText.trimmed();
            return err.isEmpty()
                       ? QStringLiteral("命令失败（退出码 %1）").arg(exitCode)
                       : QStringLiteral("命令失败（退出码 %1）：%2").arg(exitCode).arg(err);
        }
        return QString();
    }
};

#endif // PROCESS_RESULT_H

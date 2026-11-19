#ifndef PROCESS_RUNNER_H
#define PROCESS_RUNNER_H

#include "process_result.h"

#include <QProcess>

// 同步执行一个外部命令，并把「启动失败 / 超时 / 退出码」分开记录。
// 成功判据只有 ProcessResult::success() 一条。
//
// 仅用于短生命周期的命令行工具（adb / fastboot / 刷机脚本）。放在头文件里
// 是为了让 FlashTool 与 ExploitEngine 共用同一份判定，避免两处各自实现后漂移。
inline ProcessResult runExternalProcess(const QString &program,
                                        const QStringList &args,
                                        int timeoutMs)
{
    ProcessResult result;
    if (program.isEmpty()) {
        result.stderrText = QStringLiteral("可执行文件路径为空");
        return result;
    }

    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.start();

    // 先区分「根本启动不了」和「跑了但超时」。旧实现把两者都写成
    // "Error: Command timeout"，调用方又只找 FAILED，于是失败被当成成功。
    if (!process.waitForStarted(5000)) {
        result.stderrText = QStringLiteral("无法启动 %1：%2")
                                .arg(program, process.errorString());
        return result;
    }
    result.started = true;

    if (!process.waitForFinished(timeoutMs)) {
        result.timedOut = true;
        process.kill();
        process.waitForFinished(2000);
        result.stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput());
        result.stderrText = QString::fromLocal8Bit(process.readAllStandardError());
        return result;
    }

    result.stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput());
    result.stderrText = QString::fromLocal8Bit(process.readAllStandardError());
    result.exitCode = process.exitCode();
    if (process.exitStatus() != QProcess::NormalExit) {
        result.exitCode = -1;
        result.stderrText += QStringLiteral("\n进程异常退出");
    }
    return result;
}

#endif // PROCESS_RUNNER_H

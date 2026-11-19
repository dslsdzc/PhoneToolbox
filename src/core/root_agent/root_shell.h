#ifndef ROOT_SHELL_H
#define ROOT_SHELL_H

#include <QMap>
#include <QString>

#include <functional>

struct RootShellResult {
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
    bool timedOut = false;
    bool usedAgent = false;

    // stdout + stderr，供旧代码按“一个字符串”消费。
    QString combinedText() const;
};

// 全局 root 执行入口：
//   1. 该设备注册了执行器（已连接的 Root 代理）→ 走代理；
//   2. 没有可用代理 → 回退 adb shell su -c（保持旧行为）。
// 代理通道未授权 dangerous 时执行器会返回错误，不会偷偷回退 adb su。
class RootShell
{
public:
    // 返回 usedAgent=false 表示当前没有可用代理，RootShell 会回退 adb。
    using Executor =
        std::function<RootShellResult(const QString &command, int timeoutMs)>;

    static RootShell &instance();

    void registerExecutor(const QString &serial, Executor executor);
    void unregisterExecutor(const QString &serial);
    bool hasExecutor(const QString &serial) const;

    RootShellResult run(const QString &serial, const QString &command,
                        int timeoutMs = 30000);

private:
    RootShell() = default;
    QMap<QString, Executor> m_executors;
};

#endif // ROOT_SHELL_H

#ifndef ROOT_AGENT_EXECUTOR_H
#define ROOT_AGENT_EXECUTOR_H

#include "core/root_agent/root_shell.h"

class RootAgentClient;

// 在 RootAgentClient 上同步执行一条 shell 命令。
// 返回 usedAgent=false 表示连接不可用，调用方应回退 adb。
RootShellResult runRootAgentCommand(RootAgentClient *client,
                                    const QString &command, int timeoutMs);

#endif // ROOT_AGENT_EXECUTOR_H

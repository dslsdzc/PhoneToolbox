package com.local.service;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * 桌面端通过 `am broadcast -n <pkg>/com.local.service.AgentBootReceiver` 触发。
 * 只启动 agent，不创建 Activity，因此不会把 APK 界面带到前台。
 */
public class AgentBootReceiver extends BroadcastReceiver {

    static final String ACTION_START_AGENT = "com.local.service.START_AGENT";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION_START_AGENT.equals(intent.getAction()))
            return;

        // AgentLauncher.start 自己维护读取线程；goAsync 必须覆盖实际启动，
        // 否则广播一返回系统就可能回收进程，su/agent 还没起来。
        Log.i("AgentBoot", "START_AGENT broadcast received");
        final PendingResult pending = goAsync();
        final CountDownLatch started = new CountDownLatch(1);
        Thread launcher = AgentLauncher.startIfNotRunning(context.getApplicationContext(),
                new AgentLauncher.Listener() {
                    @Override
                    public void onStarted(int port, int pid) {
                        started.countDown();
                    }

                    @Override
                    public void onExited(int exitCode, String lastLine) {
                        started.countDown();
                    }

                    @Override
                    public void onFailed(String error) {
                        started.countDown();
                    }
                });
        if (launcher == null) {
            pending.finish();
            return;
        }
        new Thread(() -> {
            try {
                started.await(8, TimeUnit.SECONDS);
            } catch (InterruptedException ignored) {
            } finally {
                pending.finish();
            }
        }, "agent-boot-wait").start();
    }
}

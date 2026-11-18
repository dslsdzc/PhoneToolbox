package com.local.service;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

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

        // AgentLauncher.start 自己维护读取线程，这里只需触发。
        Log.i("AgentBoot", "START_AGENT broadcast received");
        final PendingResult pending = goAsync();
        try {
            AgentLauncher.start(context.getApplicationContext(), null);
        } finally {
            pending.finish();
        }
    }
}

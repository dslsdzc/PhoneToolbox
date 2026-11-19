package com.phonetoolbox.agent;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Minimal Android 1.0-4.4 UI. The pair code is generated and shown here;
 * the desktop must not read it over adb or inject it through a broadcast.
 */
public final class LegacyActivity extends Activity {
    private TextView mCode;
    private TextView mCodeHint;
    private TextView mStatus;
    private TextView mPeers;
    private final Handler mHandler = new Handler();
    private volatile boolean mRefreshing;
    private volatile boolean mActive;

    private final Runnable mRefreshTask = new Runnable() {
        @Override
        public void run() {
            refreshAsync();
            if (mActive)
                mHandler.postDelayed(this, 3000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // FLAG_SECURE exists since API 1: block screenshots/recents/previews.
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_SECURE,
                WindowManager.LayoutParams.FLAG_SECURE);
        setTitle("PhoneToolbox Agent");

        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(16), dp(16), dp(16), dp(16));

        TextView title = text("PhoneToolbox Agent", 22, true);
        root.addView(title);

        TextView warning = text(
                "注意：配对即授予完整 Root 权限。\n"
                        + "配对成功后，这台电脑可以以 root 身份执行任意命令、读写任意文件，"
                        + "没有按应用或按操作的权限隔离。只与你信任的电脑配对。",
                13, false);
        warning.setTextColor(Color.rgb(122, 62, 0));
        warning.setBackgroundColor(Color.rgb(255, 243, 205));
        warning.setPadding(dp(10), dp(10), dp(10), dp(10));
        root.addView(warning, topMargin(dp(10)));

        root.addView(text("配对码（仅本机屏幕显示）", 14, true), topMargin(dp(16)));

        mCode = text(LegacyBootstrap.getOrCreatePairCode(this), 34, true);
        mCode.setGravity(Gravity.CENTER_HORIZONTAL);
        mCode.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        hardenPairCodeView();
        root.addView(mCode, topMargin(dp(4)));

        mCodeHint = text("桌面端必须手动输入此码，不会通过 adb 自动读取。", 12, false);
        mCodeHint.setTextColor(Color.DKGRAY);
        root.addView(mCodeHint, topMargin(dp(2)));

        LinearLayout firstRow = new LinearLayout(this);
        firstRow.setOrientation(LinearLayout.HORIZONTAL);
        Button regenerate = button("重新生成配对码");
        regenerate.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                regenerateAsync();
            }
        });
        Button restart = button("启动 / 重启服务");
        restart.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                restartAsync();
            }
        });
        firstRow.addView(regenerate, weight());
        firstRow.addView(restart, weight());
        root.addView(firstRow, topMargin(dp(12)));

        LinearLayout secondRow = new LinearLayout(this);
        secondRow.setOrientation(LinearLayout.HORIZONTAL);
        Button stop = button("停止服务");
        stop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopAsync();
            }
        });
        Button refresh = button("刷新");
        refresh.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                refreshAsync();
            }
        });
        secondRow.addView(stop, weight());
        secondRow.addView(refresh, weight());
        root.addView(secondRow, topMargin(dp(6)));

        mStatus = text("正在读取服务状态…", 14, false);
        root.addView(mStatus, topMargin(dp(16)));

        mPeers = text("正在读取已配对电脑…", 13, false);
        root.addView(mPeers, topMargin(dp(10)));

        TextView footer = text(
                "服务只监听 127.0.0.1，桌面端必须通过 adb forward 连接。\n"
                        + "配对码不会写入日志；删除已配对电脑会立即吊销其会话。",
                11, false);
        footer.setTextColor(Color.DKGRAY);
        root.addView(footer, topMargin(dp(18)));

        scroll.addView(root);
        setContentView(scroll);

        // Opening the app starts the agent if it is not already running.
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    if (!LegacyBootstrap.isAgentRunning(LegacyActivity.this))
                        LegacyBootstrap.start(LegacyActivity.this);
                } catch (Throwable ignored) {
                }
                mHandler.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        refreshAsync();
                    }
                }, 800);
            }
        }, "legacy-agent-start").start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        mActive = true;
        mHandler.post(mRefreshTask);
    }

    @Override
    protected void onPause() {
        mActive = false;
        mHandler.removeCallbacks(mRefreshTask);
        super.onPause();
    }

    private void regenerateAsync() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                LegacyBootstrap.regeneratePairCode(LegacyActivity.this);
                final boolean started = LegacyBootstrap.start(LegacyActivity.this);
                toast(started ? "已生成新的配对码"
                              : "新码已生成，但旧服务未退出");
                refreshAsync();
            }
        }, "legacy-agent-regenerate").start();
    }

    private void restartAsync() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean started = LegacyBootstrap.start(LegacyActivity.this);
                toast(started ? "服务已启动" : "启动失败：旧服务未退出");
                refreshAsync();
            }
        }, "legacy-agent-restart").start();
    }

    private void stopAsync() {
        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean stopped = LegacyBootstrap.stopAgent(LegacyActivity.this);
                toast(stopped ? "服务已停止" : "停止失败：旧进程未退出");
                refreshAsync();
            }
        }, "legacy-agent-stop").start();
    }

    private void refreshAsync() {
        if (mRefreshing || !mActive)
            return;
        mRefreshing = true;
        new Thread(new Runnable() {
            @Override
            public void run() {
                final String code = LegacyBootstrap.getOrCreatePairCode(
                        LegacyActivity.this);
                final boolean used = LegacyBootstrap.isPairCodeUsed(
                        LegacyActivity.this);
                final boolean running = LegacyBootstrap.isAgentRunning(
                        LegacyActivity.this);
                final int port = LegacyBootstrap.readAgentPort(
                        LegacyActivity.this);
                final String peers = LegacyBootstrap.readPeers(
                        LegacyActivity.this);
                mHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        mRefreshing = false;
                        mCode.setText(code);
                        mCodeHint.setText(used
                                ? "该配对码已使用；请点「重新生成配对码」生成新码。"
                                : "桌面端必须手动输入此码，不会通过 adb 自动读取。");
                        mCodeHint.setTextColor(used ? Color.rgb(160, 60, 0)
                                                    : Color.DKGRAY);
                        mStatus.setText(running
                                ? "服务运行中" + (port > 0 ? "，端口 " + port : "")
                                : "服务未运行");
                        mStatus.setTextColor(running ? Color.rgb(20, 110, 40)
                                                     : Color.rgb(160, 60, 0));
                        mPeers.setText(formatPeers(peers));
                    }
                });
            }
        }, "legacy-agent-refresh").start();
    }

    private String formatPeers(String raw) {
        String text = raw == null ? "" : raw.trim();
        if (text.length() == 0)
            return "已配对电脑：暂无";
        String[] lines = text.split("\n");
        StringBuilder builder = new StringBuilder();
        int count = 0;
        for (int i = 0; i < lines.length; ++i) {
            String line = lines[i].trim();
            if (line.length() == 0)
                continue;
            String[] parts = line.split(" ");
            String name = parts.length > 1 ? parts[1] : "(未命名)";
            if (count > 0)
                builder.append('\n');
            builder.append("- ").append(name);
            ++count;
            if (count >= 8)
                break;
        }
        if (count == 0)
            return "已配对电脑：暂无";
        return "已配对电脑（" + count + (count >= 8 ? "+" : "") + "）：\n"
                + builder.toString();
    }

    private void toast(final String message) {
        mHandler.post(new Runnable() {
            @Override
            public void run() {
                Toast.makeText(LegacyActivity.this, message,
                        Toast.LENGTH_SHORT).show();
            }
        });
    }

    // API 4+ can replace the accessibility text; API 16+ can remove the node.
    // Reflection keeps the same APK running on Android 1.0 (API 1).
    private void hardenPairCodeView() {
        try {
            View.class.getMethod("setContentDescription", CharSequence.class)
                    .invoke(mCode, "配对码，请查看屏幕");
        } catch (Throwable ignored) {
        }
        try {
            final java.lang.reflect.Field field =
                    View.class.getField("IMPORTANT_FOR_ACCESSIBILITY_NO");
            final int no = field.getInt(null);
            View.class.getMethod("setImportantForAccessibility", int.class)
                    .invoke(mCode, Integer.valueOf(no));
        } catch (Throwable ignored) {
        }
    }

    private TextView text(String value, int sizeSp, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        if (bold)
            view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    private Button button(String label) {
        Button button = new Button(this);
        button.setText(label);
        return button;
    }

    private LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        params.setMargins(dp(3), 0, dp(3), 0);
        return params;
    }

    private LinearLayout.LayoutParams topMargin(int margin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, margin, 0, 0);
        return params;
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                value, getResources().getDisplayMetrics());
    }
}

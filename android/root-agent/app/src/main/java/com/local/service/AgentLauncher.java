package com.local.service;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 启动/守护 native root agent。
 * MainActivity 和 AgentBootReceiver 共用：广播拉起时不会把界面带到前台。
 */
final class AgentLauncher {

    static final String PREFS = "prefs";
    static final String KEY_PAIR_CODE = "pair_code";
    static final String KEY_PORT = "agent_port";
    static final String KEY_PAIRED = "paired";
    static final String KEY_PID = "agent_pid";

    // One agent per app process. The flag stays set while the launcher thread
    // is alive (i.e. until the agent exits), so concurrent broadcasts cannot
    // start a second instance.
    private static final AtomicBoolean STARTING = new AtomicBoolean(false);

    interface Listener {
        void onStarted(int port, int pid);
        void onExited(int exitCode, String lastLine);
        void onFailed(String error);
    }

    private AgentLauncher() {
    }

    static String getOrCreatePairCode(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String code = prefs.getString(KEY_PAIR_CODE, null);
        if (code == null) {
            code = generatePairCode();
            prefs.edit().putString(KEY_PAIR_CODE, code).apply();
        }
        return code;
    }

    static String generatePairCode() {
        final int value = new SecureRandom().nextInt(1000000);
        return String.format(Locale.US, "%06d", value);
    }

    // 返回启动线程；listener 可选，回调发生在后台线程。
    static Thread start(final Context context, final Listener listener) {
        if (!STARTING.compareAndSet(false, true))
            return null;
        final Context app = context.getApplicationContext();
        Thread thread = new Thread(() -> {
            try {
                Log.i("AgentLauncher", "starting native agent");
                final SharedPreferences prefs =
                        app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
                final int oldPid = prefs.getInt(KEY_PID, -1);
                if (oldPid > 0) {
                    // Verify identity before killing: PID reuse otherwise turns
                    // a stale preference into a kill of an unrelated process.
                    if (isOurAgent(oldPid)) {
                        try {
                            Runtime.getRuntime().exec(
                                    new String[]{"su", "-c", "kill " + oldPid}).waitFor();
                        } catch (Exception ignored) {
                        }
                    }
                    prefs.edit().remove(KEY_PID).apply();
                }

                final String pairCode = getOrCreatePairCode(app);
                final String agentPath =
                        app.getApplicationInfo().nativeLibraryDir + "/libservice.so";
                final String stateDir = app.getFilesDir().getAbsolutePath();
                final String auditDir = app.getFilesDir().getAbsolutePath();
                final String auditPath = auditDir + "/audit.log";
                final String commandLine = agentPath
                        + " --port 0"
                        + " --pair-code " + pairCode
                        + " --state-dir " + stateDir
                        + " --audit " + auditPath;

                ProcessBuilder builder = new ProcessBuilder(
                        Arrays.asList("su", "-c", commandLine));
                builder.redirectErrorStream(true);
                Process process = builder.start();
                BufferedReader reader = new BufferedReader(
                        new InputStreamReader(process.getInputStream()));

                String line;
                String lastLine = null;
                int port = -1;
                while ((line = reader.readLine()) != null) {
                    lastLine = line;
                    if (line.startsWith("PORT=")) {
                        port = Integer.parseInt(line.substring(5));
                        prefs.edit().putInt(KEY_PORT, port).apply();
                    } else if (line.startsWith("PAIRED=")) {
                        final int paired =
                                line.substring(7).trim().equals("1") ? 1 : 0;
                        prefs.edit().putInt(KEY_PAIRED, paired).apply();
                    } else if (line.startsWith("PID=")) {
                        final int pid = Integer.parseInt(line.substring(4));
                        prefs.edit().putInt(KEY_PID, pid).apply();
                        Log.i("AgentLauncher", "agent started port=" + port
                                + " pid=" + pid);
                        if (listener != null)
                            listener.onStarted(port, pid);
                    }
                }

                final int exit = process.waitFor();
                if (listener != null)
                    listener.onExited(exit, lastLine);
            } catch (Exception exception) {
                Log.e("AgentLauncher", "start failed", exception);
                if (listener != null)
                    listener.onFailed(exception.getMessage());
            } finally {
                STARTING.set(false);
            }
        }, "agent-launcher");
        thread.start();
        return thread;
    }

    private static boolean isOurAgent(int pid) {
        Process process = null;
        try {
            process = Runtime.getRuntime().exec(
                    new String[]{"su", "-c", "cat /proc/" + pid + "/cmdline"});
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            char[] buffer = new char[512];
            int got = reader.read(buffer);
            if (got <= 0)
                return false;
            final String command = new String(buffer, 0, got);
            return command.contains("libservice.so");
        } catch (Exception ignored) {
            return false;
        } finally {
            if (process != null)
                process.destroy();
        }
    }
}

package com.local.service;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
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
    static final String KEY_PID_START = "agent_pid_start";

    // One agent per app process. The flag stays set while the launcher thread
    // is alive (i.e. until the agent exits), so concurrent broadcasts cannot
    // start a second instance.
    private static final AtomicBoolean STARTING = new AtomicBoolean(false);

    // Process identity is tri-state on purpose: "cannot verify" must never be
    // treated as "process is gone", otherwise a permission/proc failure turns
    // into killing an unrelated PID or starting a duplicate agent.
    private static final int PROC_GONE = 0;
    private static final int PROC_OURS = 1;
    private static final int PROC_NOT_OURS = 2;
    private static final int PROC_UNKNOWN = 3;

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
    static void clearPairCodeUsed(final Context context) {
        final Context app = context.getApplicationContext();
        final File used = new File(app.getFilesDir(), "pair_code_used");
        used.delete();
        try {
            Runtime.getRuntime().exec(new String[]{"su", "-c",
                    "rm -f " + used.getAbsolutePath()}).waitFor();
        } catch (Exception ignored) {
        }
    }

    // Start only when no agent is running; safe for boot/receiver callers.
    static Thread startIfNotRunning(final Context context, final Listener listener) {
        if (!STARTING.compareAndSet(false, true))
            return null;
        final Context app = context.getApplicationContext();
        Thread thread = new Thread(() -> {
            final boolean[] restartAfterExit = {false};
            try {
                Log.i("AgentLauncher", "starting native agent");
                final SharedPreferences prefs =
                        app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
                // A used code means the previous agent paired successfully.
                // Rotate before restarting so the old code cannot be replayed.
                final File usedFlag = new File(app.getFilesDir(), "pair_code_used");
                if (usedFlag.exists()) {
                    prefs.edit().putString(KEY_PAIR_CODE, generatePairCode()).apply();
                    clearPairCodeUsed(app);
                }
                final int oldPid = prefs.getInt(KEY_PID, -1);
                final long oldStart = prefs.getLong(KEY_PID_START, -1);
                if (oldPid > 0) {
                    // Verify identity before killing: PID reuse otherwise turns
                    // a stale preference into a kill of an unrelated process.
                    int state = probeAgent(oldPid, oldStart);
                    if (state == PROC_UNKNOWN) {
                        if (listener != null)
                            listener.onFailed("无法验证旧服务身份，拒绝启动");
                        return;
                    }
                    if (state == PROC_OURS) {
                        killAgent(oldPid);
                        state = waitForAgentExit(oldPid, oldStart, 3000);
                        if (state == PROC_OURS || state == PROC_UNKNOWN) {
                            if (listener != null)
                                listener.onFailed("旧服务未退出，拒绝重复启动");
                            return;
                        }
                    }
                    prefs.edit().remove(KEY_PID).remove(KEY_PID_START)
                            .remove(KEY_PORT).apply();
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
                    } else if (line.startsWith("PAIRING_OK=")) {
                        // One-time code: persist a fresh code and restart so
                        // the next pairing round uses it immediately. The used
                        // flag stays set until start() rotates it.
                        prefs.edit().putString(KEY_PAIR_CODE,
                                generatePairCode()).apply();
                        Log.i("AgentLauncher", "pairing complete; rotating code");
                        restartAfterExit[0] = true;
                        process.destroy();
                    } else if (line.startsWith("PID=")) {
                        final int pid = Integer.parseInt(line.substring(4));
                        final long pidStart = readProcessStartStable(pid);
                        prefs.edit().putInt(KEY_PID, pid)
                                .putLong(KEY_PID_START, pidStart).apply();
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
                if (restartAfterExit[0]) {
                    new Thread(() -> startIfNotRunning(app, null),
                               "agent-restart").start();
                }
            }
        }, "agent-launcher");
        thread.start();
        return thread;
    }

    // Stop the recorded agent, wait for it to disappear, and only then clear
    // runtime state. Returns false when the old process could not be confirmed
    // gone; callers must not start a duplicate in that case.
    static boolean stop(final Context context) {
        final Context app = context.getApplicationContext();
        final SharedPreferences prefs =
                app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int pid = prefs.getInt(KEY_PID, -1);
        long start = prefs.getLong(KEY_PID_START, -1);

        // A just-started launcher may not have written PID yet. Wait for it,
        // so restart cannot miss the process that is still starting.
        for (int i = 0; i < 100 && pid <= 0 && STARTING.get(); ++i) {
            try {
                Thread.sleep(50);
            } catch (InterruptedException ignored) {
            }
            pid = prefs.getInt(KEY_PID, -1);
            start = prefs.getLong(KEY_PID_START, -1);
        }

        if (pid <= 0) {
            // If the launcher is still starting without publishing a PID we
            // cannot prove that no agent exists. Refuse to report success.
            return !STARTING.get();
        }

        int state = probeAgent(pid, start);
        if (state == PROC_UNKNOWN) {
            // Do not clear the record: the process may still be alive and the
            // next start would create a duplicate we cannot manage.
            return false;
        }
        if (state == PROC_OURS) {
            killAgent(pid);
            state = waitForAgentExit(pid, start, 4000);
        }

        final boolean gone = state == PROC_GONE || state == PROC_NOT_OURS;
        if (!gone)
            return false;

        // The launcher thread may still be draining EOF. A subsequent
        // startIfNotRunning would be rejected while STARTING is set, so wait.
        for (int i = 0; i < 40 && STARTING.get(); ++i) {
            try {
                Thread.sleep(50);
            } catch (InterruptedException ignored) {
            }
        }
        if (STARTING.get())
            return false;

        prefs.edit().remove(KEY_PID).remove(KEY_PID_START)
                .remove(KEY_PORT).apply();
        return true;
    }

    // Explicit restart: stop and confirm the old process is gone before
    // starting exactly once with the current SharedPreferences (including a
    // regenerated pair code).
    static void restart(final Context context, final Listener listener) {
        final Context app = context.getApplicationContext();
        new Thread(() -> {
            if (!stop(app)) {
                if (listener != null)
                    listener.onFailed("旧服务未退出，重启失败");
                return;
            }
            startIfNotRunning(app, listener);
        }, "agent-restart").start();
    }

    private static String runShell(String command) {
        Process process = null;
        try {
            process = Runtime.getRuntime().exec(new String[]{
                    "/system/bin/sh", "-c", command});
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            StringBuilder builder = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null)
                builder.append(line).append('\n');
            reader.close();
            process.waitFor();
            return builder.toString();
        } catch (Exception ignored) {
            return "";
        } finally {
            if (process != null)
                process.destroy();
        }
    }

    // /proc/<pid>/stat field 22 (1-based), after the command name in parens.
    private static long parseProcessStart(String stat) {
        final int close = stat.lastIndexOf(')');
        if (close < 0)
            return -1;
        final String[] fields = stat.substring(close + 1).trim().split("\\s+");
        // fields[0] = state (field 3); starttime = field 22 -> index 19.
        if (fields.length < 20)
            return -1;
        try {
            return Long.parseLong(fields[19]);
        } catch (NumberFormatException ignored) {
            return -1;
        }
    }

    private static long readProcessStart(int pid) {
        return parseProcessStart(runShell("su -c "
                + "cat /proc/" + pid + "/stat"));
    }

    // PID= is printed immediately after fork/exec; /proc may not be fully
    // populated for a few milliseconds. Retry instead of persisting -1.
    private static long readProcessStartStable(int pid) {
        for (int i = 0; i < 5; ++i) {
            final long start = readProcessStart(pid);
            if (start > 0)
                return start;
            try {
                Thread.sleep(50);
            } catch (InterruptedException ignored) {
            }
        }
        return -1;
    }

    private static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    // One su query returns cmdline + stat so the two cannot be read from
    // different processes if the PID is reused mid-check.
    private static int probeAgent(int pid, long expectedStart) {
        if (pid <= 0)
            return PROC_GONE;

        final String script = "if [ -d /proc/" + pid + " ]; then "
                + "if [ -r /proc/" + pid + "/cmdline ] && "
                + "[ -r /proc/" + pid + "/stat ]; then "
                + "echo __PTB_PRESENT__; "
                + "cat /proc/" + pid + "/cmdline; echo; "
                + "cat /proc/" + pid + "/stat; "
                + "else echo __PTB_DENIED__; fi; "
                + "else echo __PTB_GONE__; fi";
        final String output = runShell("su -c " + shellQuote(script));

        if (output.contains("__PTB_GONE__"))
            return PROC_GONE;
        if (output.contains("__PTB_DENIED__"))
            return PROC_UNKNOWN;

        final int marker = output.indexOf("__PTB_PRESENT__");
        if (marker < 0)
            return PROC_UNKNOWN; // su missing/denied: cannot verify.
        final String rest = output.substring(marker + "__PTB_PRESENT__".length());
        final int split = rest.indexOf('\n');
        if (split < 0)
            return PROC_UNKNOWN;

        final String command = rest.substring(0, split).trim();
        if (!command.contains("libservice.so"))
            return PROC_NOT_OURS;
        if (expectedStart <= 0)
            return PROC_UNKNOWN; // No start time recorded: fail closed.
        final long actualStart = parseProcessStart(rest.substring(split + 1));
        if (actualStart <= 0)
            return PROC_UNKNOWN;
        return actualStart == expectedStart ? PROC_OURS : PROC_NOT_OURS;
    }

    private static int waitForAgentExit(int pid, long start, int timeoutMs) {
        final long deadline = System.currentTimeMillis() + timeoutMs;
        int state = probeAgent(pid, start);
        while (state == PROC_OURS && System.currentTimeMillis() < deadline) {
            try {
                Thread.sleep(100);
            } catch (InterruptedException ignored) {
            }
            state = probeAgent(pid, start);
        }
        return state;
    }

    private static void killAgent(int pid) {
        try {
            Runtime.getRuntime().exec(new String[]{"kill", String.valueOf(pid)});
        } catch (Exception ignored) {
        }
        try {
            Runtime.getRuntime().exec(
                    new String[]{"su", "-c", "kill " + pid}).waitFor();
        } catch (Exception ignored) {
        }
    }
}

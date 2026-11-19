package com.phonetoolbox.agent;

import android.content.Context;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.security.SecureRandom;
import java.util.Locale;

/**
 * Android 1.0-4.4 bootstrap for the static ARM phone_agent executable.
 *
 * The pair code is generated and displayed by LegacyActivity on the phone.
 * It is never accepted from an adb broadcast and never read back by the
 * desktop: an adb-only attacker must not be able to pair without seeing the
 * device screen. The receiver only starts/stops the local agent.
 */
public final class LegacyBootstrap {
    private static final String ASSET_NAME = "phone_agent";
    private static final String BINARY_NAME = "phone_agent";
    private static final String SCRIPT_NAME = "start-agent.sh";
    private static final String LOG_NAME = "agent.out";
    private static final String PAIR_CODE_NAME = "pair_code";
    private static final String PAIR_USED_NAME = "pair_code_used";
    // Fixed device-side port: the desktop can adb-forward to it without
    // reading the app-private agent.out file (unavailable before run-as).
    static final int AGENT_PORT = 32145;

    // "Cannot verify" is distinct from "same PID belongs to another process":
    // only the latter proves the recorded agent is gone.
    private static final int PROC_GONE = 0;
    private static final int PROC_OURS = 1;
    private static final int PROC_NOT_OURS = 2;
    private static final int PROC_UNKNOWN = 3;

    static final class PairCodeChoice {
        final String code;
        final boolean resetUsed;

        PairCodeChoice(String code, boolean resetUsed) {
            this.code = code;
            this.resetUsed = resetUsed;
        }
    }

    public static synchronized boolean start(Context context) {
        try {
            File base = context.getFilesDir();
            File binary = new File(base, BINARY_NAME);
            File state = new File(base, "state");
            File script = new File(base, SCRIPT_NAME);
            File log = new File(base, LOG_NAME);
            if (!state.isDirectory()) {
                state.mkdirs();
            }
            try {
                extract(context, binary);
            } catch (Throwable error) {
                logError(context, error);
            }
            if (!stopPreviousAgent(context)) {
                logError(context, new IOException("previous agent did not exit"));
                return false;
            }
            final PairCodeChoice choice = resolvePairCode(context);
            writeScript(script, binary, state, log, choice);
            startScript(script);
            return true;
        } catch (Throwable ignored) {
            logError(context, ignored);
            return false;
        }
    }

    static synchronized boolean stopAgent(Context context) {
        return stopPreviousAgent(context);
    }

    static boolean isAgentRunning(Context context) {
        int pid = readAgentPid(context);
        return pid > 0 && probeLegacyAgent(pid) == PROC_OURS;
    }

    static int readAgentPort(Context context) {
        String text = readText(new File(context.getFilesDir(), LOG_NAME));
        if (text.length() == 0)
            text = runShell("su -c "
                    + shellQuote("cat " + shellQuote(
                            new File(context.getFilesDir(), LOG_NAME).getAbsolutePath())));
        String[] lines = text.split("\n");
        for (int i = 0; i < lines.length; ++i) {
            String line = lines[i].trim();
            if (line.startsWith("PORT=")) {
                try {
                    return Integer.parseInt(line.substring(5).trim());
                } catch (Throwable ignored) {
                    return -1;
                }
            }
        }
        return -1;
    }

    static String getOrCreatePairCode(Context context) {
        File file = new File(context.getFilesDir(), PAIR_CODE_NAME);
        String existing = readText(file);
        if (isSixDigits(existing))
            return existing;
        String code = generatePairCode();
        writeText(file, code);
        return code;
    }

    static String regeneratePairCode(Context context) {
        String code = generatePairCode();
        writeText(new File(context.getFilesDir(), PAIR_CODE_NAME), code);
        deleteUsedFlag(context);
        return code;
    }

    static boolean isPairCodeUsed(Context context) {
        File used = usedFlag(context);
        if (used.exists())
            return true;
        String output = runShell("su -c "
                + shellQuote("ls " + shellQuote(used.getAbsolutePath())));
        return output.indexOf(used.getAbsolutePath()) >= 0;
    }

    static String readPeers(Context context) {
        File peers = new File(new File(context.getFilesDir(), "state"), "peers.txt");
        String text = readText(peers);
        if (text.length() > 0)
            return text;
        return runShell("su -c "
                + shellQuote("cat " + shellQuote(peers.getAbsolutePath())));
    }

    private static File usedFlag(Context context) {
        return new File(new File(context.getFilesDir(), "state"), PAIR_USED_NAME);
    }

    private static void deleteUsedFlag(Context context) {
        File used = usedFlag(context);
        used.delete();
        runShell("su -c " + shellQuote("rm -f " + shellQuote(used.getAbsolutePath())));
    }

    private static PairCodeChoice resolvePairCode(Context context) {
        if (isPairCodeUsed(context)) {
            // One-time code: rotate before starting a fresh agent instance.
            deleteUsedFlag(context);
            String code = generatePairCode();
            writeText(new File(context.getFilesDir(), PAIR_CODE_NAME), code);
            return new PairCodeChoice(code, true);
        }
        return new PairCodeChoice(getOrCreatePairCode(context), false);
    }

    private static String generatePairCode() {
        final int value = new SecureRandom().nextInt(1000000);
        return String.format(Locale.US, "%06d", Integer.valueOf(value));
    }

    private static boolean isSixDigits(String text) {
        if (text == null)
            return false;
        String value = text.trim();
        if (value.length() != 6)
            return false;
        for (int i = 0; i < value.length(); ++i) {
            if (value.charAt(i) < '0' || value.charAt(i) > '9')
                return false;
        }
        return true;
    }

    private static boolean stopPreviousAgent(Context context) {
        try {
            int pid = readAgentPid(context);
            if (pid <= 0)
                return true;

            int state = probeLegacyAgent(pid);
            if (state == PROC_UNKNOWN) {
                logError(context, new IOException("cannot verify previous agent"));
                return false;
            }
            if (state == PROC_GONE || state == PROC_NOT_OURS)
                return true;

            Runtime.getRuntime().exec(new String[] {
                    "kill", String.valueOf(pid) });
            // A root-owned agent needs the su path; harmless when su is
            // missing (legacy devices without root).
            try {
                Runtime.getRuntime().exec(new String[] {
                        "su", "-c", "kill " + pid }).waitFor();
            } catch (Throwable ignored) {
                // No su.
            }
            // Wait until the verified process is actually gone.
            for (int i = 0; i < 40 && state == PROC_OURS; ++i) {
                Thread.sleep(50);
                state = probeLegacyAgent(pid);
            }
            return state == PROC_GONE || state == PROC_NOT_OURS;
        } catch (Throwable ignored) {
            logError(context, ignored);
            return false;
        }
    }

    // Direct /proc read first; fall back to one su query when the agent runs
    // as another uid. Missing markers mean the kernel/su path could not tell
    // us anything, so the caller must not treat the process as gone.
    private static int probeLegacyAgent(int pid) {
        if (pid <= 0)
            return PROC_GONE;

        File cmdline = new File("/proc/" + pid + "/cmdline");
        String command = readText(cmdline);
        if (command.length() > 0)
            return command.indexOf("phone_agent") >= 0
                    ? PROC_OURS : PROC_NOT_OURS;

        String script = "if [ -d /proc/" + pid + " ]; then "
                + "if [ -r /proc/" + pid + "/cmdline ]; then "
                + "echo __PTB_PRESENT__; cat /proc/" + pid + "/cmdline; "
                + "else echo __PTB_DENIED__; fi; "
                + "else echo __PTB_GONE__; fi";
        String output = runShell("su -c " + shellQuote(script));

        if (output.indexOf("__PTB_GONE__") >= 0)
            return PROC_GONE;
        if (output.indexOf("__PTB_DENIED__") >= 0)
            return PROC_UNKNOWN;
        int marker = output.indexOf("__PTB_PRESENT__");
        if (marker < 0)
            return PROC_UNKNOWN;
        command = output.substring(marker + "__PTB_PRESENT__".length());
        return command.indexOf("phone_agent") >= 0
                ? PROC_OURS : PROC_NOT_OURS;
    }

    private static int readAgentPid(Context context) {
        String text = readText(new File(context.getFilesDir(), LOG_NAME));
        if (text.length() == 0)
            text = runShell("su -c "
                    + shellQuote("cat " + shellQuote(
                            new File(context.getFilesDir(), LOG_NAME).getAbsolutePath())));
        String[] lines = text.split("\n");
        for (int i = 0; i < lines.length; ++i) {
            String line = lines[i].trim();
            if (line.startsWith("PID=")) {
                try {
                    return Integer.parseInt(line.substring(4).trim());
                } catch (Throwable ignored) {
                    return -1;
                }
            }
        }
        return -1;
    }

    private static String readText(File file) {
        try {
            InputStream in = new FileInputStream(file);
            try {
                byte[] buffer = new byte[65536];
                int read = in.read(buffer);
                if (read <= 0)
                    return "";
                return new String(buffer, 0, read, "UTF-8").trim();
            } finally {
                in.close();
            }
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static void writeText(File file, String text) {
        try {
            OutputStream out = new FileOutputStream(file);
            try {
                out.write(text.getBytes("UTF-8"));
            } finally {
                out.close();
            }
        } catch (Throwable ignored) {
            // The UI still shows the in-memory code; retry next launch.
        }
    }

    private static String runShell(String command) {
        Process process = null;
        try {
            process = Runtime.getRuntime().exec(new String[] {
                    "/system/bin/sh", "-c", command });
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            StringBuilder builder = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                builder.append(line).append('\n');
            }
            reader.close();
            process.waitFor();
            return builder.toString();
        } catch (Throwable ignored) {
            return "";
        } finally {
            if (process != null)
                process.destroy();
        }
    }

    private static void logError(Context context, Throwable error) {
        try {
            OutputStream out = new FileOutputStream(
                    new File(context.getFilesDir(), "bootstrap.err"));
            try {
                out.write((error.toString() + "\n").getBytes("UTF-8"));
                StackTraceElement[] trace = error.getStackTrace();
                for (int i = 0; i < trace.length; i++) {
                    out.write(("  at " + trace[i] + "\n").getBytes("UTF-8"));
                }
            } finally {
                out.close();
            }
        } catch (Throwable ignored) {
            // Nothing else can be reported.
        }
    }

    private static void extract(Context context, File binary) throws IOException {
        File temporary = new File(binary.getAbsolutePath() + ".new");
        InputStream in = context.getAssets().open(ASSET_NAME);
        try {
            OutputStream out = new FileOutputStream(temporary);
            try {
                byte[] buffer = new byte[16384];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            } finally {
                out.close();
            }
        } finally {
            in.close();
        }

        // Rename over a running executable; if the filesystem refuses to
        // replace the old inode, unlink it first and try again.
        if (!temporary.renameTo(binary)) {
            if (binary.exists()) {
                binary.delete();
            }
            if (!temporary.renameTo(binary)) {
                temporary.delete();
                throw new IOException("cannot replace " + binary.getAbsolutePath());
            }
        }
    }

    private static void writeScript(File script, File binary, File state, File log,
                                    PairCodeChoice choice) throws IOException {
        String text = "#!/system/bin/sh\n"
                + "BIN=" + shellQuote(binary.getAbsolutePath()) + "\n"
                + "STATE=" + shellQuote(state.getAbsolutePath()) + "\n"
                + "LOG=" + shellQuote(log.getAbsolutePath()) + "\n"
                + "chmod 755 \"$BIN\" 2>/dev/null\n"
                + (choice.resetUsed
                        ? "rm -f \"$STATE/" + PAIR_USED_NAME + "\"\n"
                        : "")
                + ": > \"$LOG\"\n"
                + "chmod 644 \"$LOG\" 2>/dev/null\n"
                + "\"$BIN\" --port " + AGENT_PORT + " --state-dir \"$STATE\" "
                + "--audit \"$STATE/audit.log\""
                + " --pair-code " + shellQuote(choice.code)
                + " >> \"$LOG\" 2>&1\n";
        OutputStream out = new FileOutputStream(script);
        try {
            out.write(text.getBytes("UTF-8"));
        } finally {
            out.close();
        }
    }

    private static void startScript(File script) throws IOException {
        String scriptArg = shellQuote(script.getAbsolutePath());
        String rootCommand = shellQuote("/system/bin/sh " + script.getAbsolutePath());
        String shell = "if su -c true </dev/null >/dev/null 2>&1; then "
                + "su -c " + rootCommand + " </dev/null >/dev/null 2>&1 & "
                + "else /system/bin/sh " + scriptArg
                + " </dev/null >/dev/null 2>&1 & fi";
        Runtime.getRuntime().exec(new String[] { "/system/bin/sh", "-c", shell });
    }

    private static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }
}

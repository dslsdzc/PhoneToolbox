package com.phonetoolbox.agent;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Android 1.0-4.4 bootstrap for the static ARM phone_agent executable.
 *
 * The APK has no UI. It extracts assets/phone_agent into the app's files
 * directory, then tries to start it through su first and falls back to the
 * app uid. agent.out holds the PORT/PID/PUBKEY lines the desktop reads over
 * adb (su is normally available on these rooted legacy devices).
 */
public final class LegacyBootstrap extends BroadcastReceiver {
    private static final String ASSET_NAME = "phone_agent";
    private static final String BINARY_NAME = "phone_agent";
    private static final String SCRIPT_NAME = "start-agent.sh";
    private static final String LOG_NAME = "agent.out";
    private static final String PAIR_CODE_NAME = "pair_code";
    // Fixed device-side port: the desktop can adb-forward to it without
    // reading the app-private agent.out file (unavailable before run-as).
    private static final int AGENT_PORT = 32145;

    @Override
    public void onReceive(Context context, Intent intent) {
        start(context, intent == null ? null : intent.getStringExtra(PAIR_CODE_NAME));
    }

    public static synchronized void start(Context context) {
        start(context, null);
    }

    public static synchronized void start(Context context, String pairCode) {
        try {
            File base = context.getFilesDir();
            File binary = new File(base, BINARY_NAME);
            File state = new File(base, "state");
            File script = new File(base, SCRIPT_NAME);
            File log = new File(base, LOG_NAME);
            if (!state.isDirectory()) {
                state.mkdirs();
            }
            // A desktop-supplied code always starts a new pairing round.
            if (pairCode != null && pairCode.trim().length() > 0)
                new File(state, "pair_code_used").delete();
            // Persist the pair code before touching the running binary: an
            // in-place overwrite fails with ETXTBSY on old yaffs kernels.
            final String resolvedCode = resolvePairCode(context, pairCode);
            try {
                extract(context, binary);
            } catch (Throwable error) {
                logError(context, error);
            }
            stopPreviousAgent(context);
            writeScript(script, binary, state, log, resolvedCode);
            startScript(script);
        } catch (Throwable ignored) {
            logError(context, ignored);
        }
    }

    private static void stopPreviousAgent(Context context) {
        try {
            File log = new File(context.getFilesDir(), LOG_NAME);
            java.io.BufferedReader reader = new java.io.BufferedReader(
                    new java.io.FileReader(log));
            int pid = -1;
            try {
                String line;
                while ((line = reader.readLine()) != null) {
                    if (line.startsWith("PID=")) {
                        pid = Integer.parseInt(line.substring(4).trim());
                        break;
                    }
                }
            } finally {
                reader.close();
            }
            if (pid > 0) {
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
                // Give the old process time to release the fixed port.
                Thread.sleep(400);
            }
        } catch (Throwable ignored) {
            // No previous agent or it already exited.
        }
    }

    private static String resolvePairCode(Context context, String supplied) {
        File file = new File(context.getFilesDir(), PAIR_CODE_NAME);
        if (supplied != null) {
            String trimmed = supplied.trim();
            if (trimmed.length() > 0) {
                try {
                    OutputStream out = new FileOutputStream(file);
                    try {
                        out.write(trimmed.getBytes("UTF-8"));
                    } finally {
                        out.close();
                    }
                } catch (Throwable ignored) {
                    // Still use the code for this launch.
                }
                return trimmed;
            }
        }
        try {
            InputStream in = new java.io.FileInputStream(file);
            try {
                byte[] buffer = new byte[64];
                int read = in.read(buffer);
                if (read > 0) {
                    return new String(buffer, 0, read, "UTF-8").trim();
                }
            } finally {
                in.close();
            }
        } catch (Throwable ignored) {
            // First run without a desktop-supplied code.
        }
        return null;
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
                                    String pairCode) throws IOException {
        String text = "#!/system/bin/sh\n"
                + "BIN=" + shellQuote(binary.getAbsolutePath()) + "\n"
                + "STATE=" + shellQuote(state.getAbsolutePath()) + "\n"
                + "LOG=" + shellQuote(log.getAbsolutePath()) + "\n"
                + "chmod 755 \"$BIN\" 2>/dev/null\n"
                + "\"$BIN\" --port " + AGENT_PORT + " --state-dir \"$STATE\" "
                + "--audit \"$STATE/audit.log\""
                + (pairCode == null || pairCode.length() == 0
                        ? ""
                        : " --pair-code " + shellQuote(pairCode))
                + " > \"$LOG\" 2>&1\n";
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

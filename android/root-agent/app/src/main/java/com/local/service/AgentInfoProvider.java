package com.local.service;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;

import java.io.File;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * 只允许 adb shell（uid 2000）或 root（uid 0）读取的 agent 信息。
 *
 * 普通 App 即使知道 authorities 也会被 uid 检查挡住；桌面端用
 *   adb shell content query --uri content://<包名>.agentinfo/agent
 * 读取随机端口和配对状态。配对码**不**在这里返回，必须由用户从 APK
 * 界面手动输入（一次性凭据，不通过 adb 暴露）。
 */
public class AgentInfoProvider extends ContentProvider {

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        final int uid = Binder.getCallingUid();
        if (uid != 2000 && uid != 0) {
            throw new SecurityException("only adb shell/root may read agent info");
        }

        Context context = getContext();
        if ("/apps".equals(uri.getPath())) {
            return queryApps(context);
        }
        if ("/audit".equals(uri.getPath())) {
            MatrixCursor audit = new MatrixCursor(new String[]{"audit"});
            audit.addRow(new Object[]{readAudit(context)});
            return audit;
        }

        SharedPreferences prefs = context.getSharedPreferences("prefs", Context.MODE_PRIVATE);
        MatrixCursor cursor = new MatrixCursor(new String[]{"port", "paired"});
        cursor.addRow(new Object[]{
                prefs.getInt("agent_port", 0),
                prefs.getInt("paired", 0)
        });
        return cursor;
    }

    // 一次返回全部应用：包名、应用名、版本、系统/用户、APK 路径。
    // 由已安装的 Root 代理 APK 自己查 PackageManager，桌面端不需要
    // 单独 push dex，也不需要逐个 pull APK 解析 label。
    private Cursor queryApps(Context context) {
        MatrixCursor cursor = new MatrixCursor(new String[]{"data"});
        PackageManager pm = context.getPackageManager();
        List<PackageInfo> packages = pm.getInstalledPackages(0);
        Collections.sort(packages, new Comparator<PackageInfo>() {
            @Override
            public int compare(PackageInfo a, PackageInfo b) {
                final String left = String.valueOf(pm.getApplicationLabel(a.applicationInfo));
                final String right = String.valueOf(pm.getApplicationLabel(b.applicationInfo));
                return left.compareToIgnoreCase(right);
            }
        });

        for (PackageInfo info : packages) {
            ApplicationInfo app = info.applicationInfo;
            if (app == null)
                continue;

            String label;
            try {
                label = String.valueOf(pm.getApplicationLabel(app));
            } catch (Exception ignored) {
                label = app.packageName;
            }
            label = label.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');

            String version = info.versionName == null ? "" : info.versionName;
            version = version.replace('\t', ' ').replace('\n', ' ');

            final boolean system = (app.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
            cursor.addRow(new Object[]{
                    app.packageName + "\t" + label + "\t" + version + "\t"
                            + (system ? "1" : "0") + "\t" + app.sourceDir
            });
        }
        return cursor;
    }

    private String readAudit(Context context) {
        // Prefer the private file used by current builds; keep reading the old
        // external location once for migration. Show the newest 256 KiB.
        File privateFile = new File(context.getFilesDir(), "audit.log");
        File external = context.getExternalFilesDir(null);
        File legacyFile = new File(external != null ? external : context.getFilesDir(),
                "audit.log");
        File file = privateFile.exists() ? privateFile : legacyFile;
        if (!file.exists())
            return "";
        try (java.io.RandomAccessFile reader =
                     new java.io.RandomAccessFile(file, "r")) {
            final long length = reader.length();
            final long start = Math.max(0L, length - 256L * 1024L);
            reader.seek(start);
            byte[] data = new byte[(int) (length - start)];
            reader.readFully(data);
            return new String(data, "UTF-8");
        } catch (Exception exception) {
            return "audit read error: " + exception.getMessage();
        }
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
                      String[] selectionArgs) {
        return 0;
    }
}

package com.local.service;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Gravity;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;

/**
 * 最小壳：显示配对码、已配对电脑、启动/停止 native agent。
 * 不做开机自启；桌面端可以通过广播后台启动 agent，不必打开这个界面。
 */
public class MainActivity extends Activity {

    private static final class Peer {
        final String hex;
        final String name;
        final String capabilities;

        Peer(String hex, String name, String capabilities) {
            this.hex = hex;
            this.name = name;
            this.capabilities = capabilities;
        }
    }

    private TextView mStatus;
    private TextView mPairCodeView;
    private TextView mPackageView;
    private TextView mPeerEmpty;
    private LinearLayout mPeerList;
    private volatile int mPort = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 防 adb screencap / 录屏 / 最近任务预览：配对码窗口不可被系统捕获。
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_SECURE,
                WindowManager.LayoutParams.FLAG_SECURE);
        setContentView(R.layout.activity_main);

        mStatus = findViewById(R.id.status);
        mPairCodeView = findViewById(R.id.pair_code);
        mPackageView = findViewById(R.id.package_name);
        mPeerList = findViewById(R.id.peer_list);
        mPeerEmpty = findViewById(R.id.peer_empty);
        Button restart = findViewById(R.id.restart);
        Button resetPairing = findViewById(R.id.reset_pairing);
        Button stop = findViewById(R.id.stop);
        Button regenerate = findViewById(R.id.regenerate_code);
        Button refreshPeers = findViewById(R.id.refresh_peers);

        mPackageView.setText(getPackageName());
        mPairCodeView.setText(AgentLauncher.getOrCreatePairCode(this));

        restart.setOnClickListener(v -> restartAgent());
        stop.setOnClickListener(v -> stopAgent());
        resetPairing.setOnClickListener(v -> resetPairing());
        refreshPeers.setOnClickListener(v -> refreshPeers());
        regenerate.setOnClickListener(v -> {
            final String code = AgentLauncher.generatePairCode();
            getSharedPreferences(AgentLauncher.PREFS, MODE_PRIVATE).edit()
                    .putString(AgentLauncher.KEY_PAIR_CODE, code).apply();
            mPairCodeView.setText(code);
            // 新码必须进 agent 进程；否则界面显示和进程参数会不一致。
            restartAgent();
        });

        // 打开 APK 即自动拉起 agent（先清理旧 pid，避免多个实例）。
        mStatus.postDelayed(this::restartAgent, 400);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Pairing rotates the stored code; refresh whenever the activity
        // comes back to the foreground.
        mPairCodeView.setText(AgentLauncher.getOrCreatePairCode(this));
        refreshPeers();
    }

    private String agentPath() {
        return getApplicationInfo().nativeLibraryDir + "/libservice.so";
    }

    private String stateDir() {
        return getFilesDir().getAbsolutePath();
    }

    private void refreshPeers() {
        new Thread(() -> {
            final List<Peer> peers = loadPeers();
            runOnUiThread(() -> renderPeers(peers));
        }).start();
    }

    private List<Peer> loadPeers() {
        final String command = agentPath() + " --list-peers --state-dir " + stateDir();
        try {
            Process process = Runtime.getRuntime().exec(
                    new String[]{"su", "-c", command});
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            List<Peer> peers = new ArrayList<>();
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.startsWith("PEER="))
                    continue;
                String[] parts = line.substring(5).split("\t", -1);
                if (parts.length < 1 || parts[0].length() != 64)
                    continue;
                final String name = parts.length > 1 && !parts[1].isEmpty()
                        ? parts[1] : "电脑";
                final String caps = parts.length > 2 && !parts[2].isEmpty()
                        ? parts[2] : "exec,push,pull";
                peers.add(new Peer(parts[0], name, caps));
            }
            process.waitFor();
            return peers;
        } catch (Exception exception) {
            return null;
        }
    }

    private void renderPeers(List<Peer> peers) {
        mPeerList.removeAllViews();
        if (peers == null) {
            mPeerEmpty.setVisibility(TextView.VISIBLE);
            mPeerEmpty.setText("读取已配对电脑失败（需要 root）");
            return;
        }
        if (peers.isEmpty()) {
            mPeerEmpty.setVisibility(TextView.VISIBLE);
            mPeerEmpty.setText("暂无已配对电脑");
            return;
        }

        mPeerEmpty.setVisibility(TextView.GONE);
        for (Peer peer : peers) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);

            TextView label = new TextView(this);
            final String shortHex = peer.hex.length() >= 16
                    ? peer.hex.substring(0, 16) + "…" : peer.hex;
            label.setText(peer.name + "\n" + shortHex + "\n" + peer.capabilities);
            label.setTextSize(12);
            row.addView(label, new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

            Button delete = new Button(this);
            delete.setText("删除");
            delete.setOnClickListener(v -> confirmRemove(peer));
            row.addView(delete);

            mPeerList.addView(row);
        }
    }

    private void confirmRemove(Peer peer) {
        new AlertDialog.Builder(this)
                .setTitle("删除配对")
                .setMessage("删除后，这台电脑必须重新输入配对码才能连接。")
                .setPositiveButton("删除", (dialog, which) -> removePeer(peer))
                .setNegativeButton("取消", null)
                .show();
    }

    private void removePeer(Peer peer) {
        mStatus.setText("正在删除配对…");
        new Thread(() -> {
            final String command = agentPath() + " --remove-peer " + peer.hex
                    + " --state-dir " + stateDir();
            boolean removed = false;
            try {
                Process process = Runtime.getRuntime().exec(
                        new String[]{"su", "-c", command});
                BufferedReader reader = new BufferedReader(
                        new InputStreamReader(process.getInputStream()));
                String line;
                while ((line = reader.readLine()) != null) {
                    if (line.trim().equals("REMOVED=1"))
                        removed = true;
                }
                removed = process.waitFor() == 0 && removed;
            } catch (Exception ignored) {
            }

            final boolean ok = removed;
            runOnUiThread(() -> {
                if (ok) {
                    mStatus.setText("已删除配对：" + peer.name);
                    Toast.makeText(this, "已删除配对", Toast.LENGTH_SHORT).show();
                    refreshPeers();
                } else {
                    mStatus.setText("删除配对失败");
                    Toast.makeText(this, "删除配对失败", Toast.LENGTH_SHORT).show();
                }
            });
        }).start();
    }

    private void resetPairing() {
        mStatus.setText("正在清除配对记录…");
        new Thread(() -> {
            try {
                final String peers = stateDir() + "/peers.txt";
                Runtime.getRuntime().exec(
                        new String[]{"su", "-c", "rm -f " + peers}).waitFor();
            } catch (Exception ignored) {
            }
            runOnUiThread(() -> {
                mStatus.setText("已清除配对记录");
                refreshPeers();
                restartAgent();
            });
        }).start();
    }

    private void restartAgent() {
        // AgentLauncher 会先 kill 上一个 pid，避免多实例。
        startAgent();
    }

    private void startAgent() {
        final String binary = agentPath();
        if (!new File(binary).canExecute()) {
            mStatus.setText("服务不可执行：" + binary);
            return;
        }

        mStatus.setText("正在请求授权并启动服务…");
        mPort = -1;
        AgentLauncher.start(this, new AgentLauncher.Listener() {
            @Override
            public void onStarted(int port, int pid) {
                mPort = port;
                runOnUiThread(() -> mStatus.setText("服务运行中（pid " + pid + "）"));
            }

            @Override
            public void onExited(int exitCode, String lastLine) {
                runOnUiThread(() -> mStatus.setText(
                        mPort > 0
                                ? "服务已退出（code " + exitCode + "）"
                                : "启动失败：" + (lastLine == null ? "无输出" : lastLine)));
            }

            @Override
            public void onFailed(String error) {
                runOnUiThread(() -> mStatus.setText("启动失败：" + error));
            }
        });
    }

    private void stopAgent() {
        final int pid = getSharedPreferences(AgentLauncher.PREFS, MODE_PRIVATE)
                .getInt(AgentLauncher.KEY_PID, -1);
        if (pid <= 0) {
            mStatus.setText("没有记录到运行中的服务");
            return;
        }

        new Thread(() -> {
            try {
                Runtime.getRuntime().exec(
                        new String[]{"su", "-c", "kill " + pid}).waitFor();
                getSharedPreferences(AgentLauncher.PREFS, MODE_PRIVATE).edit()
                        .remove(AgentLauncher.KEY_PID).apply();
                runOnUiThread(() -> mStatus.setText("已停止"));
            } catch (Exception exception) {
                runOnUiThread(() -> mStatus.setText("停止失败：" + exception.getMessage()));
            }
        }).start();
    }
}

# 漏洞库格式

漏洞库是一个本地 JSON 文件，在程序内加载后用于设备匹配、检测和利用。仓库不内置 CVE 数据，`vulndb.json` 默认不纳入版本控制。

```json
{
  "vuln_db_version": "2.0.0",
  "entries": [
    {
      "id": "CVE-2025-XXXXX",
      "summary": "漏洞描述",
      "severity": "CRITICAL",
      "cvss_score": 9.8,
      "affected": {
        "android_version_min": "14",
        "android_version_max": "14",
        "patch_level_before": "2025-04-01",
        "sdk_min": 34,
        "sdk_max": 34,
        "platforms": ["google/pixel_*"]
      },
      "exploit": {
        "detect": ["echo '检测脚本...'", "echo 'VULNERABLE'"],
        "exploit": ["echo '利用脚本...'"],
        "verify": ["echo '验证脚本...'", "echo 'EXPLOIT_SUCCESS'"]
      }
    }
  ]
}
```

字段说明：

- `id`：CVE 或自定义条目 ID；BROM 等非 CVE 条目使用明确名称并标注“非 CVE”。
- `affected`：版本 / 补丁 / SDK / 平台匹配条件，留空表示不限制。
- `exploit.detect` / `exploit.exploit` / `exploit.verify`：按顺序执行的 shell 片段；通过输出中的 `VULNERABLE` / `EXPLOIT_SUCCESS` 等标记判定结果。
- `requiresRoot`：脚本需要 root 时设为 `true`。

只加载自己信任的漏洞库。执行第三方脚本前先阅读内容。

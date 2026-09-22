// src/ui/chip_code_text.cpp —— 见 .h 的分层说明。
//
// 口径（两条入口共用，改这里等于同时改两处）：
//   * 前缀 `0x` / `0X` 可有可无；**不带前缀的文本按十六进制解释**（`6735` = 0x6735）——
//     芯片码的书写习惯就是 `0x6735`，"6735" 是同一颗芯片的省写，不是十进制 6735；
//   * 字符集显式判（**不用** `QString::toUShort` 的宽松解析）：`0x` 单独一个前缀、`-1`、`0x1FG`
//     这类都当场拒，并给出可行动的中文原因 —— 0 芯片码会与表里占位语义撞车，不能靠"截断/静默 0"兜。
#include "ui/chip_code_text.h"

namespace bromui {

bool parseHwCodeText(const QString &text, quint16 *out, QString *error)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (error) *error = QStringLiteral("请填写芯片码 hw_code（十六进制，如 0x6735；设备信息面板里有）");
        return false;
    }

    QString body = t;
    if (body.startsWith(QLatin1String("0x")) || body.startsWith(QLatin1String("0X")))
        body = body.mid(2);
    if (body.isEmpty()) {
        if (error) *error = QStringLiteral("芯片码只有 `0x` 前缀：请补上十六进制数字（如 0x6735）");
        return false;
    }

    for (const QChar c : body) {
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                      || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
        if (!hex) {
            if (error) *error = QStringLiteral("芯片码含非十六进制字符 `%1`：请填十六进制（如 0x6735）").arg(c);
            return false;
        }
    }

    // 字符集已保证是纯十六进制，故 ok 只可能因**超范围**为假（此时 toUInt 给 0 —— 靠 ok 判，不看值）。
    bool ok = false;
    const uint v = body.toUInt(&ok, 16);
    if (!ok || v > 0xFFFFu) {
        if (error) *error = QStringLiteral("芯片码超出 16 位（hw_code 是 quint16，如 0x6735）");
        return false;
    }
    if (out) *out = quint16(v);
    return true;
}

QString formatHwCodeHint(quint16 hwCode)
{
    // 形态与清单的 chipHint 一致（小写 0x + 4 位十六进制）。改这里等于改匹配面 —— 用例钉住字面。
    return QStringLiteral("0x%1").arg(hwCode, 4, 16, QLatin1Char('0'));
}

} // namespace bromui

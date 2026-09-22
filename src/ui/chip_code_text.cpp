// src/ui/chip_code_text.cpp —— 见 .h 的分层说明。
//
// 口径（两条入口共用，改这里等于同时改两处）：
//   * 前缀 `0x` / `0X` 可有可无；**不带前缀的文本按十六进制解释**（`6735` = 0x6735）——
//     芯片码的书写习惯就是 `0x6735`，"6735" 是同一颗芯片的省写，不是十进制 6735；
//   * 字符集显式判（**不用** `QString::toUShort` 的宽松解析）：`0x` 单独一个前缀、`-1`、`0x1FG`
//     这类都当场拒，并给出可行动的中文原因 —— 0 芯片码会与表里占位语义撞车，不能靠"截断/静默 0"兜。
//
// **一份实现、两个包装**：字符集/前缀/范围机制在 `parseHexCore`（唯一处），`parseHwCodeText` 与
// `parseVar1Text` 只各自组织文案（不各自再解一遍）。W8 终审 I4 的要求是"同一判据别在两处各写"
// —— 这条是它对**文本判据**的同款落地。
#include "ui/chip_code_text.h"

namespace bromui {

namespace {

// 十六进制文本解析的**唯一实现**。失败时给出失败类型与（可能的）肇事字符，由包装函数组织文案。
enum class HexFail { None, Empty, PrefixOnly, BadChar, OutOfRange };

HexFail parseHexCore(const QString &text, quint32 maxValue, quint32 *out, QChar *badChar)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return HexFail::Empty;

    QString body = t;
    if (body.startsWith(QLatin1String("0x")) || body.startsWith(QLatin1String("0X")))
        body = body.mid(2);
    if (body.isEmpty())
        return HexFail::PrefixOnly;

    for (const QChar c : body) {
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                      || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
        if (!hex) {
            if (badChar) *badChar = c;
            return HexFail::BadChar;
        }
    }

    // 字符集已保证是纯十六进制，故 ok 只可能因**超范围**为假（此时 toUInt 给 0 —— 靠 ok 判，不看值）。
    bool ok = false;
    const uint v = body.toUInt(&ok, 16);
    if (!ok || v > maxValue)
        return HexFail::OutOfRange;
    if (out) *out = v;
    return HexFail::None;
}

} // namespace

bool parseHwCodeText(const QString &text, quint16 *out, QString *error)
{
    quint32 v = 0;
    QChar bad;                                   // 失败类型为 BadChar 时由 parseHexCore 回填
    const HexFail f = parseHexCore(text, 0xFFFFu, &v, &bad);
    switch (f) {
    case HexFail::None:
        if (out) *out = quint16(v);
        return true;
    case HexFail::Empty:
        if (error) *error = QStringLiteral("请填写芯片码 hw_code（十六进制，如 0x6735；设备信息面板里有）");
        return false;
    case HexFail::PrefixOnly:
        if (error) *error = QStringLiteral("芯片码只有 `0x` 前缀：请补上十六进制数字（如 0x6735）");
        return false;
    case HexFail::BadChar:
        if (error) *error = QStringLiteral("芯片码含非十六进制字符 `%1`：请填十六进制（如 0x6735）").arg(bad);
        return false;
    case HexFail::OutOfRange:
        if (error) *error = QStringLiteral("芯片码超出 16 位（hw_code 是 quint16，如 0x6735）");
        return false;
    }
    return false;   // 不可达（枚举已穷尽）
}

bool parseVar1Text(const QString &text, quint8 *out, QString *error)
{
    quint32 v = 0;
    QChar bad;                                   // 同上：BadChar 时由 parseHexCore 回填
    const HexFail f = parseHexCore(text, 0xFFu, &v, &bad);
    switch (f) {
    case HexFail::None:
        if (out) *out = quint8(v);
        return true;
    case HexFail::Empty:
        if (error) *error = QStringLiteral("请填写 var1（十六进制，0x00..0xFF，如 0x0A）");
        return false;
    case HexFail::PrefixOnly:
        if (error) *error = QStringLiteral("var1 只有 `0x` 前缀：请补上十六进制数字（如 0x0A）");
        return false;
    case HexFail::BadChar:
        if (error) *error = QStringLiteral("var1 含非十六进制字符 `%1`：请填十六进制（如 0x0A）").arg(bad);
        return false;
    case HexFail::OutOfRange:
        if (error) *error = QStringLiteral("var1 超出 8 位（控制传输的 wIndex 是 quint8 语义，"
                                           "取值 0x00..0xFF）");
        return false;
    }
    return false;   // 不可达（枚举已穷尽）
}

QString formatHwCodeHint(quint16 hwCode)
{
    // 形态与清单的 chipHint 一致（小写 0x + 4 位十六进制）。改这里等于改匹配面 —— 用例钉住字面。
    return QStringLiteral("0x%1").arg(hwCode, 4, 16, QLatin1Char('0'));
}

} // namespace bromui

#include "huawei_image.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QtEndian>

namespace imghw {

namespace {
constexpr int kHeaderSize = 512;
constexpr int kEntrySize = 64;

// 签名头处理类型（update.bin）: "08"=直接读 4B 签名长度; "06"=先跳 16B 再读 4B
bool isValidSigType(const QString &type)
{
    return type == QStringLiteral("08") || type == QStringLiteral("06");
}

// JSON 文本 → SignConfig（loadSignConfig 与 fetchSignConfig 共用）
bool parseSignConfigJson(const QByteArray &json, SignConfig &out, QString *error)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("签名配置 JSON 解析失败: %1").arg(pe.errorString());
        return false;
    }
    const QJsonObject o = doc.object();
    const QJsonValue typeVal = o.value(QStringLiteral("sigHeaderType"));
    const QJsonValue offVal = o.value(QStringLiteral("sigLenOffset"));
    const QJsonValue keyVal = o.value(QStringLiteral("haveKey"));
    const QJsonValue pathVal = o.value(QStringLiteral("keyPath"));
    if (!typeVal.isString() || !isValidSigType(typeVal.toString())) {
        if (error) *error = QStringLiteral("sigHeaderType 缺失或非法（应为 \"08\"/\"06\"）");
        return false;
    }
    if (!offVal.isUndefined() && (!offVal.isDouble() || offVal.toDouble() < 0 ||
                                  offVal.toDouble() > 0xFFFFFFFFull)) {
        if (error) *error = QStringLiteral("sigLenOffset 非法");
        return false;
    }
    out.sigHeaderType = typeVal.toString();
    out.sigLenOffset = offVal.isUndefined() ? 0 : static_cast<quint32>(offVal.toDouble());
    out.haveKey = keyVal.isBool() ? keyVal.toBool() : false;
    out.keyPath = pathVal.isString() ? pathVal.toString() : QString();
    if (out.haveKey && out.keyPath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("haveKey 为真但 keyPath 为空");
        return false;
    }
    return true;
}
} // namespace

bool isUpdateApp(const QByteArray &header)
{
    return header.size() >= 2 && static_cast<uchar>(header[0]) == 0x55 &&
           static_cast<uchar>(header[1]) == 0xAA;
}

bool parseUpdateApp(const QByteArray &app, QList<AppFile> &out, QString *error)
{
    if (!isUpdateApp(app) || app.size() < kHeaderSize) {
        if (error) *error = "非法 update.app 头";
        return false;
    }
    const quint32 fileCount = qFromLittleEndian<quint32>(app.constData() + 8);
    const quint32 tableOff = qFromLittleEndian<quint32>(app.constData() + 12);
    const quint32 dataOff = qFromLittleEndian<quint32>(app.constData() + 16);
    if (tableOff < kHeaderSize) {
        if (error) *error = "文件表偏移非法";
        return false;
    }
    if (fileCount > 4096) {
        if (error) *error = "文件数异常";
        return false;
    }
    out.clear();
    for (quint32 i = 0; i < fileCount; ++i) {
        const qint64 off = tableOff + static_cast<qint64>(i) * kEntrySize;
        if (off + kEntrySize > app.size()) {
            if (error) *error = "文件表越界";
            return false;
        }
        const char *e = app.constData() + off;
        AppFile f;
        f.type = qFromLittleEndian<quint32>(e + 4);
        f.rawSize = qFromLittleEndian<quint32>(e + 8);
        f.compSize = qFromLittleEndian<quint32>(e + 12);
        f.offset = qFromLittleEndian<quint32>(e + 16);
        // 文件名: UTF-16LE 或 ASCII（两种厂商变体），取 \0 截断
        QByteArray nameRaw(e + 32, 32);
        if (nameRaw.size() >= 2 && nameRaw[1] == 0 && nameRaw[3] == 0)
            f.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(nameRaw.constData()),
                                        nameRaw.size() / 2).split(QChar(0)).first();
        else
            f.name = QString::fromLatin1(nameRaw).split('\0').first();
        out.append(f);
    }
    Q_UNUSED(dataOff);
    return true;
}

QByteArray extractFile(const QByteArray &app, const AppFile &f, QString *error)
{
    // 防呆: 头不足 512B 不读取偏移字段（全局契约: 非法输入返回空, 不崩溃）
    if (app.size() < kHeaderSize) {
        if (error) *error = "非法 update.app（头不足 512B）";
        return {};
    }
    const quint32 dataOff = qFromLittleEndian<quint32>(app.constData() + 16);
    const qint64 off = dataOff + f.offset;
    if (off + f.compSize > app.size()) {
        if (error) *error = QString("文件 %1 数据越界").arg(f.name);
        return {};
    }
    return app.mid(off, f.compSize);
}

QByteArray buildUpdateApp(const QList<AppFile> &files)
{
    QByteArray hdr(kHeaderSize, 0);
    hdr[0] = char(0x55); hdr[1] = char(0xAA);
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v); hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16); hdr[off + 3] = char(v >> 24);
    };
    put32(8, static_cast<quint32>(files.size()));
    put32(12, kHeaderSize);
    put32(16, kHeaderSize + static_cast<quint32>(files.size()) * kEntrySize);
    QByteArray out = hdr;
    quint32 seq = 0;
    quint32 dataCursor = 0;
    for (const AppFile &f : files) {
        QByteArray entry(kEntrySize, 0);
        auto e32 = [&](int off, quint32 v) {
            entry[off] = char(v); entry[off + 1] = char(v >> 8);
            entry[off + 2] = char(v >> 16); entry[off + 3] = char(v >> 24);
        };
        e32(0, seq++);
        e32(4, f.type);
        e32(8, f.rawSize);
        e32(12, f.compSize);
        e32(16, dataCursor);
        const QByteArray name = f.name.toUtf8().left(32);
        entry.replace(32, name.size(), name);
        out.append(entry);
        dataCursor += f.compSize;
    }
    // 数据段按文件表顺序紧凑排列（保持文件表顺序与数据段偏移对齐）——
    // 本接口仅构建容器骨架（头 + 文件表）; 数据段填充由 buildUpdateAppWithData 完成。
    return out;
}

QByteArray buildUpdateAppWithData(const QList<QPair<AppFile, QByteArray>> &files,
                                  const SignConfig *sign)
{
    // 防呆: 非法输入返回空（全局契约）
    if (files.size() > 4096)
        return {};
    for (const auto &p : files) {
        if (p.second.size() < 0 || static_cast<quint32>(p.second.size()) != p.first.compSize)
            return {}; // 数据长度必须与条目 compSize 一致
    }
    if (sign && (sign->sigHeaderType.trimmed().isEmpty() ||
                 (sign->haveKey && sign->keyPath.trimmed().isEmpty())))
        return {}; // 非法签名配置
    const bool withSign = sign != nullptr;
    const quint32 entryCount = static_cast<quint32>(files.size()) + (withSign ? 1 : 0);

    QByteArray hdr(kHeaderSize, 0);
    hdr[0] = char(0x55); hdr[1] = char(0xAA);
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v); hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16); hdr[off + 3] = char(v >> 24);
    };
    put32(8, entryCount);
    put32(12, kHeaderSize);
    put32(16, kHeaderSize + entryCount * kEntrySize);
    QByteArray out = hdr;
    quint32 seq = 0, cursor = 0;
    for (const auto &p : files) {
        QByteArray entry(kEntrySize, 0);
        auto e32 = [&](int off, quint32 v) {
            entry[off] = char(v); entry[off + 1] = char(v >> 8);
            entry[off + 2] = char(v >> 16); entry[off + 3] = char(v >> 24);
        };
        e32(0, seq++);
        e32(4, p.first.type);
        e32(8, p.first.rawSize);
        e32(12, p.first.compSize);
        e32(16, cursor);
        const QByteArray name = p.first.name.toUtf8().left(32);
        entry.replace(32, name.size(), name);
        out.append(entry);
        cursor += p.first.compSize;
    }
    if (withSign) {
        // 签名占位条目: type 0x05, 数据段留空（实际签名应用留扩展; 提供配置即标记签名类型）
        QByteArray entry(kEntrySize, 0);
        auto e32 = [&](int off, quint32 v) {
            entry[off] = char(v); entry[off + 1] = char(v >> 8);
            entry[off + 2] = char(v >> 16); entry[off + 3] = char(v >> 24);
        };
        e32(0, seq++);
        e32(4, 0x05);
        e32(16, cursor);
        const QByteArray sigName = QByteArrayLiteral("signature");
        entry.replace(32, sigName.size(), sigName);
        out.append(entry);
    }
    // 数据段按文件表顺序紧凑排列
    for (const auto &p : files)
        out.append(p.second);
    return out;
}

bool parseUpdateBin(const QByteArray &bin, QList<BinPartition> &out, QString *error)
{
    // L2 型: 178B 文件头 + 2B 分区信息总长度 + N×87B + "update/info.bin"(16B)
    constexpr int kHead = 178;
    if (bin.size() < kHead + 2) {
        if (error) *error = "update.bin 头过短";
        return false;
    }
    const quint16 infoLen = static_cast<quint16>(static_cast<uchar>(bin[kHead])) |
                            (static_cast<quint16>(static_cast<uchar>(bin[kHead + 1])) << 8);
    if (infoLen == 0 || infoLen % 87 != 0) {
        if (error) *error = QString("分区信息长度异常 %1 (非 87 的倍数)").arg(infoLen);
        return false;
    }
    const int entryCount = infoLen / 87;
    if (kHead + 2 + infoLen + 16 > bin.size()) {
        if (error) *error = "update.bin 数据越界";
        return false;
    }
    out.clear();
    for (int i = 0; i < entryCount; ++i) {
        const char *e = bin.constData() + kHead + 2 + i * 87;
        BinPartition p;
        // 87B 记录: 大小 8B @47-54 + GUID 32B @55-86（与 unpack_huawei_package.py 一致:
        // 记录内 OFFSET+=47 后读 8B 大小; brief 原文"长度@48" 与 GUID@55 重叠 1B, 按真实布局修正）
        p.size = qFromLittleEndian<quint64>(e + 47);
        const QByteArray guidRaw(e + 55, 32);
        p.guid = QString::fromLatin1(guidRaw.toHex());
        // 分区名: 非 L2 变体有 ASCII 名；L2 主要靠 GUID —— 用固定 "partN" 占位，
        // 完整映射在真实样本验证阶段按 unpack_huawei_package.py 核对
        p.name = QString("part%1").arg(i);
        out.append(p);
    }
    return true;
}

bool loadSignConfig(const QString &jsonPath, SignConfig &out, QString *error)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开签名配置: %1").arg(jsonPath);
        return false;
    }
    return parseSignConfigJson(f.readAll(), out, error);
}

bool fetchSignConfig(const QString &repoUrl, const QString &version, SignConfig &out, QString *error)
{
    // 骨架（spec 2026-08-04 补充）: 从外部仓库下载签名配置，不内置签名材料。
    // 下载需要 Qt6::Network 接入 image_engine（CMake 接线不在 B6 文件清单内），
    // 本任务实现: URL/版本校验 + 用户数据目录缓存优先读取。
    if (repoUrl.trimmed().isEmpty() || version.trimmed().isEmpty()) {
        if (error) *error = "仓库 URL 或版本为空";
        return false;
    }
    QString v = version;
    v.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    const QString cachePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                              + QStringLiteral("/sign_config/") + v + QStringLiteral(".json");
    if (QFile::exists(cachePath))
        return loadSignConfig(cachePath, out, error);
    if (error) *error = QStringLiteral("签名配置未缓存（%1 @ %2）; 外部仓库下载接线未实现")
                            .arg(repoUrl, version);
    return false;
}

} // namespace imghw

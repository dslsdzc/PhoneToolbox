#include "huawei_image.h"
#include <QtEndian>

namespace imghw {

namespace {
constexpr int kHeaderSize = 512;
constexpr int kEntrySize = 64;
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
    // 数据段按顺序紧凑排列（保持文件表顺序与数据段对齐不变）
    for (const AppFile &f : files) {
        // 注意: buildUpdateApp 的调用方需提供 data —— 本接口按"文件表驱动"重建容器，
        // 数据由调用方在重打包流程中写入（见 spec: 华为重打包需保持数据段偏移对齐）。
        // 此实现构建容器骨架；数据段填充由重打包流程任务 (B6) 完成。
        Q_UNUSED(f);
    }
    return out;
}

} // namespace imghw

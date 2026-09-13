// src/core/edl/firehose.cpp
//
// 实现见头文件的分层说明。所有命令构造是纯函数；只有末尾两个函数碰 IEdlTransport。
#include "firehose.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QXmlStreamReader>

namespace edl {
namespace {

// XML 属性值转义：& 必须最先语义化处理（逐字符一遍扫完，天然无二次转义问题）。
// 参照侧由 libxml2 的 xmlSetProp/xml_setpropf 代劳（reference/qdl/src/firehose.c:510-515 等），
// bkerler 是裸字符串拼接（edl/edlclient/Library/firehose.py:940-947 一带），故这里显式做。
QString xmlEscape(const QString &v)
{
    QString out;
    out.reserve(v.size());
    for (const QChar c : v) {
        switch (c.unicode()) {
        case u'&':  out += QLatin1String("&amp;");  break;
        case u'"':  out += QLatin1String("&quot;"); break;
        case u'<':  out += QLatin1String("&lt;");   break;
        case u'>':  out += QLatin1String("&gt;");   break;
        default:    out += c;                       break;
        }
    }
    return out;
}

// 裸元素拼装：`<name k="v" …/>`（属性顺序即入参顺序 —— 逐字节测试依赖它稳定）
QByteArray element(const QString &name, const QList<QPair<QString, QString>> &attrs)
{
    QString s = QLatin1Char('<') + name;
    for (const QPair<QString, QString> &a : attrs) {
        s += QLatin1Char(' ') + a.first + QLatin1String("=\"") + xmlEscape(a.second)
             + QLatin1Char('"');
    }
    s += QLatin1String("/>");
    return s.toUtf8();
}

// start_sector 取值（program / patch / erase 同款）：表达式非空 → **原样透传**；
// 否则十进制。参照注释明确主机侧解释表达式会写错地址
// （reference/qdl/src/firehose.c:874-879；patch 的 value 同理 :1420-1421）。
// 模型契约：表达式非空时 PlanEntry::startSector==0（flash_plan.h:17-19），
// 绝不能把这个 0 发出去。
QString startSectorAttr(const PlanEntry &e)
{
    return e.startSectorExpr.isEmpty() ? QString::number(e.startSector) : e.startSectorExpr;
}

// 设备方向的包裹：`<?xml …?><data>…</data>`，**无长度前缀**
// （reference/qdl/src/firehose.c:399-405 把 root=<data> 的文档整篇 xmlDocDumpMemory 出去；
// bkerler 的 connectcmd 同样自带声明 + <data>：edl/edlclient/Library/firehose.py:904-922）。
// 命令构造器给的是裸元素，包裹统一在此补；调用方若已自带声明/根则原样发。
QByteArray firehoseFrame(const QByteArray &xml)
{
    const QByteArray body = xml.trimmed();
    if (body.startsWith("<?xml") || body.startsWith("<data"))
        return xml;
    QByteArray out("<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data>");
    out += body;
    out += "</data>";
    return out;
}

// 响应侧的反包裹：设备把 <log/> 与 <response/> 作为**多个顶层元素**连发
// （"Generally <log/> messages are coming prior to the <response/>, but on MSM8916
// it's been observed that <log/> messages can arrive after the <response/>" ——
// reference/qdl/src/firehose.c:250-270），而 QXmlStreamReader 碰到第二个顶层元素就报
// "Extra content at end of document" 并**停止投递**（Qt6 实测）。故剥掉所有 `<?xml …?>`
// 声明后把整串包进合成根；流式传输上多条响应粘连（firehose.c:270-290 同款问题）也一并覆盖。
QByteArray firehoseEnvelope(const QByteArray &xml)
{
    QByteArray body;
    int i = 0;
    while (i < xml.size()) {
        const int p = xml.indexOf("<?xml", i);
        if (p < 0) { body += xml.mid(i); break; }
        body += xml.mid(i, p - i);
        const int e = xml.indexOf("?>", p);
        i = (e < 0) ? xml.size() : e + 2;
    }
    QByteArray out("<firehose>");
    out += body;
    out += "</firehose>";
    return out;
}

// 从响应原文里取 <response> 元素的某个属性（configure 协商用）
quint32 responseAttrU32(const QString &rawXml, const QString &attr)
{
    QXmlStreamReader rd(firehoseEnvelope(rawXml.toUtf8()));
    while (!rd.atEnd()) {
        const QXmlStreamReader::TokenType t = rd.readNext();
        if (rd.hasError())
            break;
        if (t != QXmlStreamReader::StartElement)
            continue;
        if (rd.name().toString() != QLatin1String("response"))
            continue;
        bool ok = false;
        const quint32 v = rd.attributes().value(attr).toString().toUInt(&ok);
        return ok ? v : 0;
    }
    return 0;
}

// 从一段文本里切出**平衡的** JSON 对象（bkerler 的日志常带 "INFO:" 前缀：
// edl/edlclient/Library/firehose.py:1312-1313 注释同款处理即 replace("INFO:", "")）。
QString extractJsonObject(const QString &s)
{
    const int start = s.indexOf(QLatin1Char('{'));
    if (start < 0)
        return QString();
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (int i = start; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inStr) {
            if (esc)                       esc = false;
            else if (c == QLatin1Char('\\')) esc = true;
            else if (c == QLatin1Char('"'))  inStr = false;
            continue;
        }
        if (c == QLatin1Char('"'))      inStr = true;
        else if (c == QLatin1Char('{')) ++depth;
        else if (c == QLatin1Char('}') && --depth == 0)
            return s.mid(start, i - start + 1);
    }
    return QString();
}

// JSON 数值（也接受数字串：设备两种都发过 —— bkerler 对同一批日志既做 int(x) 也做
// int(x, 16)：edl/edlclient/Library/firehose.py:1253-1275）
bool jsonU64(const QJsonObject &o, const QString &key, quint64 *out)
{
    const QJsonValue v = o.value(key);
    if (v.isDouble()) {
        *out = quint64(v.toDouble());
        return true;
    }
    if (v.isString()) {
        bool ok = false;
        const quint64 n = v.toString().toULongLong(&ok);
        if (ok) {
            *out = n;
            return true;
        }
    }
    return false;
}

// bkerler 的文本键收集：逐行按 '=' 切，切不开再按 ':' 切
// （edl/edlclient/Library/firehose.py:1303-1317 就是这个顺序），键做 trim。
bool textKeyValue(const QString &text, const QString &key, QString *out)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        int sep = line.indexOf(QLatin1Char('='));
        if (sep <= 0)
            sep = line.indexOf(QLatin1Char(':'));
        if (sep <= 0)
            continue;
        const QString k = line.left(sep).trimmed();
        const QString v = line.mid(sep + 1).trimmed();
        if (k == key) {
            *out = v;
            return true;
        }
    }
    return false;
}

// 另一存储类型：bkerler 只示范 eMMC → UFS（edl/edlclient/Library/firehose.py:936-940），
// 本项目 FlashPlan::storageType 只有 "ufs"/"emmc"（flash_plan.h:33），故双向换一次；
// 其它值（NAND/SPINOR 等）不猜，返回空串让调用方走失败分支。
QString otherStorageType(const QString &name)
{
    const QString lower = name.toLower();
    if (lower.contains(QLatin1String("emmc")))
        return QStringLiteral("ufs");
    if (lower.contains(QLatin1String("ufs")))
        return QStringLiteral("emmc");
    return QString();
}

// 错误文案里的设备原文（截断，避免把整包日志灌进 UI）
QString excerpt(const QString &s, int maxLen = 200)
{
    return s.size() <= maxLen ? s : s.left(maxLen) + QStringLiteral("…");
}

constexpr int kMaxReadsPerResponse = 64;        // 读次数上限（防"永远不含 <response"时死循环）
constexpr int kConfigureTimeoutMs  = 10000;     // configure 一次往返的超时预算
// LUN 数防呆上限：设备报怪值（或日志里出现无关数字）时不得让调用方去发成千上万条 getstorageinfo。
// 真实 eMMC/UFS 的 LUN 数 ≤ 6（bkerler 的 maxlun 同量级），8 只作上限、不是对设备的假设。
constexpr quint32 kMaxProbedLuns = 8;

} // namespace

// ---------------------------------------------------------------------------
// 命令构造
// ---------------------------------------------------------------------------

QByteArray xmlConfigure(const QString &memoryName, quint32 maxPayloadBytes)
{
    // reference/qdl/src/firehose.c:510-515：MemoryName、MaxPayloadSizeToTargetInBytes（0 → 省略，
    // 让设备回 MaxPayloadSizeToTargetInBytesSupported）、Verbose=0、ZlpAwareHost=1、SkipStorageInit=0。
    // 注意大小写：qdl 用 "ZlpAwareHost"（:514），bkerler 用 "ZLPAwareHost"（firehose.py:901）——
    // 以主证据 qdl 为准。（qdl 的 slot/NAND 附加属性项目无对应字段，不发。）
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("MemoryName"), memoryName);
    if (maxPayloadBytes > 0)
        attrs << qMakePair(QStringLiteral("MaxPayloadSizeToTargetInBytes"),
                           QString::number(maxPayloadBytes));
    attrs << qMakePair(QStringLiteral("Verbose"), QStringLiteral("0"))
          << qMakePair(QStringLiteral("ZlpAwareHost"), QStringLiteral("1"))
          << qMakePair(QStringLiteral("SkipStorageInit"), QStringLiteral("0"));
    return element(QStringLiteral("configure"), attrs);
}

QByteArray xmlProgram(const PlanEntry &e)
{
    // 属性集与顺序 = reference/qdl/src/firehose.c:1021-1030（bkerler 同四个、不发 filename：
    // edl/edlclient/Library/firehose.py:491-499）。
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("SECTOR_SIZE_IN_BYTES"), QString::number(e.sectorSize))
          << qMakePair(QStringLiteral("num_partition_sectors"), QString::number(e.numSectors))
          << qMakePair(QStringLiteral("physical_partition_number"), QString::number(e.lun))
          << qMakePair(QStringLiteral("start_sector"), startSectorAttr(e));
    if (!e.imageFile.isEmpty())
        // 只发文件名（设备侧无从理解主机路径；rawprogram XML 的 filename 本就是裸名）
        attrs << qMakePair(QStringLiteral("filename"), QFileInfo(e.imageFile).fileName());
    return element(QStringLiteral("program"), attrs);
}

QByteArray xmlPatch(const PlanEntry &e)
{
    // reference/qdl/src/firehose.c:1414-1424 的 7 个属性，**不发 what**（:1408 只进调试日志）；
    // bkerler 同（edl/edlclient/Library/firehose.py:427-443 的属性清单）。
    // filename 原样（"DISK" 表示下发设备；其它文件名由计划层跳过，:1405-1406）。
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("SECTOR_SIZE_IN_BYTES"), QString::number(e.sectorSize))
          << qMakePair(QStringLiteral("byte_offset"), QString::number(e.byteOffset))
          << qMakePair(QStringLiteral("filename"), e.imageFile)
          << qMakePair(QStringLiteral("physical_partition_number"), QString::number(e.lun))
          << qMakePair(QStringLiteral("size_in_bytes"), QString::number(e.sizeInBytes))
          << qMakePair(QStringLiteral("start_sector"), startSectorAttr(e))
          << qMakePair(QStringLiteral("value"), e.value);
    return element(QStringLiteral("patch"), attrs);
}

QByteArray xmlErase(const PlanEntry &e)
{
    // reference/qdl/src/firehose.c:611-621：省略 num_partition_sectors/start_sector 即整 LUN 擦；
    // 判断条件是"num_sectors > 0 才补两属性"。表达式非空时 numSectors==0 不代表整 LUN
    // （flash_plan.h:17-20 的模型契约），故补 `|| !startSectorExpr.isEmpty()`。
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("SECTOR_SIZE_IN_BYTES"), QString::number(e.sectorSize))
          << qMakePair(QStringLiteral("physical_partition_number"), QString::number(e.lun));
    if (e.numSectors > 0 || !e.startSectorExpr.isEmpty()) {
        attrs << qMakePair(QStringLiteral("num_partition_sectors"), QString::number(e.numSectors))
              << qMakePair(QStringLiteral("start_sector"), startSectorAttr(e));
    }
    return element(QStringLiteral("erase"), attrs);
}

QByteArray xmlRead(quint32 lun, quint64 startSector, quint64 numSectors, quint32 sectorSize)
{
    // reference/qdl/src/firehose.c:1197-1207（属性名是 num_partition_sectors —— 既有
    // edl_handler.cpp:716-725 的 num_sectors 是缺陷，协议速查 §6.5）。
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("SECTOR_SIZE_IN_BYTES"), QString::number(sectorSize))
          << qMakePair(QStringLiteral("num_partition_sectors"), QString::number(numSectors))
          << qMakePair(QStringLiteral("physical_partition_number"), QString::number(lun))
          << qMakePair(QStringLiteral("start_sector"), QString::number(startSector));
    return element(QStringLiteral("read"), attrs);
}

QByteArray xmlGetStorageInfo(quint32 lun)
{
    // reference/qdl/src/firehose.c:1907-1908（bkerler 固定 "0"：firehose.py:1293-1294）
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("physical_partition_number"), QString::number(lun));
    return element(QStringLiteral("getstorageinfo"), attrs);
}

QByteArray xmlSetBootableStorageDrive(quint32 lun)
{
    // reference/qdl/src/firehose.c:1561-1569；value = bootloader 分区所在 LUN
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("value"), QString::number(lun));
    return element(QStringLiteral("setbootablestoragedrive"), attrs);
}

QByteArray xmlReset()
{
    // reference/qdl/src/firehose.c:1590-1592（DelayInSeconds 是 qdl 补的"防重启失败"，
    // bkerler 只发 value：edl/edlclient/Library/firehose.py:331-334）
    QList<QPair<QString, QString>> attrs;
    attrs << qMakePair(QStringLiteral("value"), QStringLiteral("reset"))
          << qMakePair(QStringLiteral("DelayInSeconds"), QStringLiteral("10"));
    return element(QStringLiteral("power"), attrs);
}

// ---------------------------------------------------------------------------
// 响应解析
// ---------------------------------------------------------------------------

FirehoseResponse parseFirehoseResponse(const QByteArray &xml)
{
    FirehoseResponse r;
    r.raw = QString::fromUtf8(xml);

    QStringList logs;
    QXmlStreamReader rd(firehoseEnvelope(xml));
    while (!rd.atEnd()) {
        const QXmlStreamReader::TokenType t = rd.readNext();
        if (rd.hasError())
            break;                       // 截断/畸形：已收下的部分仍有效（不把 log 当响应）
        if (t != QXmlStreamReader::StartElement)
            continue;
        const QString name = rd.name().toString();

        if (name == QLatin1String("response")) {
            // **严格相等**判定：contains 会被"log 文本里出现 ACK"骗过
            // （反面参照 src/core/modes/edl_handler.cpp:494-510；两参照都是属性值相等判定：
            //  reference/qdl/src/firehose.c:1869-1872 的 xmlStrcmp、bkerler getstatus
            //  edl/edlclient/Library/firehose.py:235-239）。
            const QString v = rd.attributes().value(QLatin1String("value")).toString();
            if (v == QLatin1String("ACK"))
                r.ack = true;
            else if (v == QLatin1String("NAK"))
                r.nak = true;
        } else if (name == QLatin1String("log")) {
            // 取 value 属性；XML 实体（&quot; &amp; …）由 QXmlStreamReader 解码，
            // 无需也不会二次反转义。极少数实现把文本放元素体（<log>text</log>）→ 兜底。
            QString v = rd.attributes().value(QLatin1String("value")).toString();
            if (v.isEmpty() && !rd.attributes().hasAttribute(QLatin1String("value")))
                v = rd.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement);
            if (v.isEmpty())
                continue;
            // 分流照 qdl：含 storage_info 的 JSON → storageInfoJson（firehose.c:1874-1884
            // 只解析这种 log，其余才当调试/错误文本）。
            if (v.contains(QLatin1String("storage_info"))) {
                const QString json = extractJsonObject(v);
                if (!json.isEmpty()) {
                    r.storageInfoJson = json;
                    continue;
                }
            }
            logs << v;                   // 多条日志以 \n 连接（bkerler 逐行遍历 rsp.error）
        }
    }
    r.errorText = logs.join(QLatin1Char('\n'));
    return r;
}

bool parseStorageInfo(const FirehoseResponse &r, quint32 lun, StorageInfo &out, QString *error)
{
    out = StorageInfo();                 // 先归零：blockSize 的 4096 默认值不得冒充"读到了"
    out.lun = lun;

    bool gotSector = false;   // 仅用于"文本键回退是否还需要跑"（成败判据见函数末尾）

    // ① JSON 口径（reference/qdl/src/firehose.c:1874-1884）
    if (!r.storageInfoJson.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(r.storageInfoJson.toUtf8());
        QJsonObject si = doc.object().value(QLatin1String("storage_info")).toObject();
        if (si.isEmpty()) {
            // bkerler 侧 storage_info 可能是数组（它对 si 直接迭代取值：
            // edl/edlclient/Library/firehose.py:1315-1318）——取第一个对象元素。
            const QJsonArray arr = doc.object().value(QLatin1String("storage_info")).toArray();
            if (!arr.isEmpty() && arr.first().isObject())
                si = arr.first().toObject();
        }
        quint64 n = 0;
        if (jsonU64(si, QStringLiteral("total_blocks"), &n))
            out.totalBlocks = n;
        if (jsonU64(si, QStringLiteral("block_size"), &n)) {
            out.blockSize = quint32(n);
            gotSector = true;
        } else if (jsonU64(si, QStringLiteral("page_size"), &n)) {
            // bkerler 把 page_size 当扇区大小（firehose.py:1326-1327）
            out.blockSize = quint32(n);
            gotSector = true;
        }
    }

    // ② 文本键回退（bkerler：edl/edlclient/Library/firehose.py:1272-1275 的
    //    SECTOR_SIZE_IN_BYTES / num_physical_partitions；分隔符形态见 :1303-1317）。
    //    注意 num_physical_partitions 是 **LUN 数**，StorageInfo 无对应字段（LUN 集合来自计划），
    //    故只用 SECTOR_SIZE_IN_BYTES；它只补扇区大小，**不能**单独构成"几何可用"。
    if (!gotSector) {
        QString v;
        if (textKeyValue(r.errorText, QStringLiteral("SECTOR_SIZE_IN_BYTES"), &v)) {
            bool ok = false;
            const uint n = v.toUInt(&ok);
            if (ok && n > 0) {
                out.blockSize = n;
                gotSector = true;
            }
        }
    }

    // 成功判据 = **totalBlocks > 0**，唯一来源是 storage_info.total_blocks
    // （qdl 口径 reference/qdl/src/firehose.c:1880-1883）。只拿到扇区大小不算成功：
    // totalBlocks==0 会让 validatePlan 的越界判定（flash_plan.cpp:608-609 的
    // `e.startSector > it->totalBlocks || e.numSectors > it->totalBlocks - e.startSector`）
    // 把**每条 Program 条目**报成"越界"，而不是报"LUN 无设备几何"——假几何比"几何不可用"
    // 难查得多，宁可在此失败。缺 block_size 时保留默认 4096（validatePlan 只用 blockSize
    // 做一致性 warning，flash_plan.cpp:580-582，不参与越界判定）。
    if (out.totalBlocks > 0)
        return true;

    if (error) {
        QString detail = r.storageInfoJson.isEmpty() ? r.errorText : r.storageInfoJson;
        if (detail.isEmpty())
            detail = r.raw;
        *error = QStringLiteral("getstorageinfo 未返回可用存储几何（LUN %1：需要 "
                                "storage_info.total_blocks > 0；只拿到扇区大小不足以判越界）")
                     .arg(lun);
        if (!detail.isEmpty())
            *error += QStringLiteral("；设备返回：%1").arg(excerpt(detail));
    }
    return false;
}

// 设备上报的 LUN 数（listPartitions 据此逐个查几何）。三个键都是 bkerler 的存储信息文本键
// （edl/edlclient/Library/firehose.py:1266-1275）；解析口径复用既有的 textKeyValue
// （先 '=' 后 ':'，键值 trim、逐行）——与 SECTOR_SIZE_IN_BYTES 的回退同一条路，不另立一套。
quint32 parseLunCount(const FirehoseResponse &r)
{
    struct KeySpec { const char *name; int base; };
    static const KeySpec keys[] = {
        {"num_physical_partitions", 10},   // bkerler:1274-1275
        {"bNumberLu",               10},   // qdl/设备侧别名
        {"UFS Total Active LU",     16},   // bkerler:1271-1272 用 int(x, 16)
    };

    for (const KeySpec &k : keys) {
        QString v;
        if (!textKeyValue(r.errorText, QString::fromLatin1(k.name), &v))
            continue;
        v = v.trimmed();
        if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
            v = v.mid(1, v.size() - 2);                       // 设备可能带引号：`= "6"`
        if (v.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
            v = v.mid(2);                                     // 十六进制源常带 0x 前缀
        bool ok = false;
        const uint n = v.toUInt(&ok, k.base);
        if (!ok || n == 0)
            continue;                                         // 键在但值不可用 → 换下一个键
        return quint32(qMin<uint>(n, kMaxProbedLuns));         // 防呆上限（见常量注释）
    }
    return 0;   // 没报 → 调用方按"只查 LUN 0"
}

// ---------------------------------------------------------------------------
// 会话级小工具
// ---------------------------------------------------------------------------

bool firehoseSendCommand(IEdlTransport &t, const QByteArray &xml, FirehoseResponse &resp,
                         int timeoutMs, QString *error)
{
    const QByteArray payload = firehoseFrame(xml);
    QString ioErr;
    if (!t.write(payload, &ioErr)) {
        if (error)
            *error = QStringLiteral("firehose 命令发送失败：%1（命令：%2）")
                         .arg(ioErr, excerpt(QString::fromUtf8(payload), 120));
        return false;
    }

    // 命令帧的 ZLP：载荷长度恰为端点包长整数倍时，必须补一次 0 字节写 —— 否则设备侧的 bulk 读
    // 看不到"短包"，会一直等下一批数据（真机上表现为卡住/超时）。qdl 的规则对**所有**写生效
    // （reference/qdl/src/usb.c:548-553）；本项目的责任划分是"**发起该笔写的上层**补"：
    // 数据块在 edl_session 的数据面（edl_session.cpp 不变量 3），命令帧在这里。
    // 传输层是纯字节管道，不补（edl_libusb_transport.h 顶部）。
    // 离线保护：tests/test_edl_firehose.cpp 的 addsZlpWhenCommandPayloadHitsPacketBoundary。
    // 命令帧的 ZLP：载荷长度恰为端点包长整数倍时，必须补一次 0 字节写 —— 否则设备侧的 bulk 读
    // 看不到"短包"，会一直等下一批数据（真机上表现为卡住/超时）。qdl 的规则对**所有**写生效
    // （reference/qdl/src/usb.c:548-553）；本项目的责任划分是"**发起该笔写的上层**补"：
    // 数据块在 edl_session 的数据面（edl_session.cpp 不变量 3），命令帧在这里。
    // 传输层是纯字节管道，不补（edl_libusb_transport.h 顶部）。
    // 离线保护：tests/test_edl_firehose.cpp 的 addsZlpWhenCommandPayloadHitsPacketBoundary。
    const int maxPacket = t.maxPacketSize();
    if (maxPacket > 0 && !payload.isEmpty() && payload.size() % maxPacket == 0) {
        QString zlpErr;
        if (!t.write(QByteArray(), &zlpErr)) {
            if (error)
                *error = QStringLiteral("firehose 命令帧 ZLP 发送失败（载荷 %1 字节恰为包长 %2 的整数倍）：%3")
                             .arg(payload.size()).arg(maxPacket).arg(zlpErr);
            return false;
        }
    }

    // 读到 <response 才停：<log> 可能先到也可能后到（firehose.c:250-270），
    // 一次 read 也可能只拿到半条（bkerler 的 `while b"<response value" not in rdata`：
    // edl/edlclient/Library/firehose.py:269-281）。响应之后的 <log> 不在本轮消费
    // —— 那是下一个命令的 drain 范围（qdl 用"见到响应后再读到超时"来处理，:229-250）。
    QByteArray buf;
    QElapsedTimer clock;
    clock.start();
    int reads = 0;
    while (!buf.contains("<response") && reads < kMaxReadsPerResponse
           && clock.elapsed() < timeoutMs) {
        QString readErr;
        const QByteArray chunk = t.read(4096, timeoutMs, &readErr);
        if (chunk.isEmpty())
            break;                       // 超时/掉线（read 约定：超时 → 空 + error）
        buf += chunk;
        ++reads;
    }

    resp = parseFirehoseResponse(buf);
    if (!resp.ack && !resp.nak) {
        if (error) {
            *error = QStringLiteral("等待 firehose 响应超时（%1 ms 内未收到 <response>）").arg(timeoutMs);
            if (!buf.isEmpty())
                *error += QStringLiteral("；设备返回：%1").arg(excerpt(QString::fromUtf8(buf)));
        }
        return false;
    }
    return true;
}

bool firehoseConfigure(IEdlTransport &t, QString &memoryName, quint32 &maxPayloadBytes, QString *error)
{
    bool switchedStorage = false;   // 换存储类型最多一次（firehose.py:936-940）
    bool negotiated = false;        // 载荷协商最多重发一次（qdl：恰好两次发送，firehose.c:534-548）
    quint32 payload = maxPayloadBytes;

    for (;;) {
        FirehoseResponse resp;
        QString sendErr;
        if (!firehoseSendCommand(t, xmlConfigure(memoryName, payload), resp,
                                 kConfigureTimeoutMs, &sendErr)) {
            if (error)
                *error = QStringLiteral("configure 失败：%1").arg(sendErr);
            return false;
        }

        if (resp.nak) {
            const QString text = resp.errorText.isEmpty() ? resp.raw : resp.errorText;

            // 小米 EDL 鉴权（edl/edlclient/Library/firehose.py:941-956）——本项目明确不做
            // （spec §8）：直接失败，不回小米鉴权分支、不重试。
            if (text.contains(QLatin1String("Only nop and sig tag can be"))) {
                if (error)
                    *error = QStringLiteral("设备要求 EDL 鉴权（仅接受 nop/sig 标签），"
                                            "本工具暂不支持；设备返回：%1").arg(excerpt(text));
                return false;
            }
            // 存储类型不被支持 → 换另一类型重试一次（firehose.py:936-940）
            if (!switchedStorage && text.contains(QLatin1String("Not support configure MemoryName"))) {
                const QString other = otherStorageType(memoryName);
                if (!other.isEmpty()) {
                    memoryName = other;
                    switchedStorage = true;
                    payload = maxPayloadBytes;      // 新类型从调用方给的初始值重来
                    continue;
                }
            }
            if (error)
                *error = QStringLiteral("configure 失败（设备返回 NAK）：%1").arg(excerpt(text));
            return false;
        }
        // 走到这里必为 ACK：firehoseSendCommand 只在 ack||nak 时返回 true（见其注释），
        // NAK 已在上方分支处理，故不再有"既非 ACK 也非 NAK"的第三条路。

        // 两轮协商：响应带 MaxPayloadSizeToTargetInBytesSupported → 用该值重发一次
        // （reference/qdl/src/firehose.c:534-548；bkerler 同，edl/edlclient/Library/firehose.py:926-935）。
        // **最多两次发送**：negotiated 落地后即使设备再报新值也只采纳、不再发（与 qdl 一致 ——
        // 它第二次 send 后直接 `qdl->max_payload_size = size` 收尾，:544-548）；
        // 否则"每轮回报不同值"会让 10s 预算的循环无限重发，等于挂死。
        const quint32 supported =
            responseAttrU32(resp.raw, QStringLiteral("MaxPayloadSizeToTargetInBytesSupported"));
        if (supported > 0 && supported != payload) {
            if (negotiated) {
                payload = supported;    // 已重发过一次：采纳设备最新值，就此定案
            } else {
                negotiated = true;
                payload = supported;
                continue;               // 第二次（也是最后一次）发送
            }
        }

        maxPayloadBytes = payload;
        return true;
    }
}

// 发命令前的 drain（共享实现：会话层与 EDLHandler 的命令都走它）。
// 单次轮询上限 4096 字节、总次数上限 64：每次轮询都是 0 超时（非阻塞），正常几次就静默；
// 上限的作用是"话痨日志"下不死循环（上限 64×4096 字节）。
void drainResidual(IEdlTransport &t)
{
    constexpr int kDrainChunkBytes = 4096;
    constexpr int kMaxDrainPolls   = 64;
    QString ignored;
    for (int i = 0; i < kMaxDrainPolls; ++i) {
        // 0 超时 = 轮询（传输实现负责换算成"立即返回"，见 edl_transport.h 的 read 契约）
        if (t.read(kDrainChunkBytes, 0, &ignored).isEmpty())
            break;                       // 端点静默
    }
}

} // namespace edl

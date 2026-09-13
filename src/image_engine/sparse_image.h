#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

namespace imgsparse {

// 整内存接口（小文件/测试用；内部与流式接口共享同一核心逻辑）
bool isSparse(const QByteArray &header);                    // magic 0xED26FF3A

// 只读 sparse 头（前 28 字节）算**去 sparse 后**的 raw 字节数：total_blks × blk_sz
// （AOSP sparse 头布局，qdl `sparse.h` 同构；参照按去 sparse 后的大小算扇区数，
//  见 `edl/edlclient/Library/sparse.py:53-76` + `firehose.py:475-486`）。
// header 为文件开头至少 28 字节；成功返回 true 并写 rawBytes。
// 失败（头太短 / magic 不符 / blk_sz==0 / total_blks==0）返回 false 且**不修改** rawBytes ——
// 纯函数不产文案，调用方按自己的上下文给错误/警告。u32×u32 < 2^64，无溢出。
bool sparseRawSizeFromHeader(const QByteArray &header, quint64 &rawBytes);
QByteArray simg2img(const QByteArray &sparse);              // 失败返回空
QByteArray img2simg(const QByteArray &raw, quint32 blockSize = 4096);

// ---- 流式接口（GB 级大文件路径，内存 O(chunk)/O(block)）----
// progress(bytes)：字节级进度回调，单调递增、范围 [0, 输入文件大小]（已消费的输入字节数）；
//                 每 chunk/块回调一次，可传空回调。
// 失败返回 false 且 error 非空（可传 nullptr 忽略）；失败时输出文件会被删除（本接口拥有输出文件）。
bool simg2imgStream(const QString &inPath, const QString &outPath,
                    const std::function<void(quint64)> &progress = {},
                    QString *error = nullptr);
bool img2simgStream(const QString &inPath, const QString &outPath, quint32 blockSize = 4096,
                    const std::function<void(quint64)> &progress = {},
                    QString *error = nullptr);

// ---- 按块产出（EDL 数据面用：sparse 展开发生在发送时，不生成临时 raw 文件）----
//
// `SparseChunk` 描述 raw 镜像里的一段：RAW 段带 data（原始字节）；FILL 段只带 fillValue
// （4 字节 pattern 按 pattern[i % 4] 重复整段，调用方**流式生成**，不在这里 materialize）；
// DONT_CARE 段不产出数据，调用方按 rawBytes 推进偏移（reference/qdl/src/program.c:139-170：
// 只下发 RAW/FILL，DONT_CARE 不下发但 start_sector 仍前移）。
// **大段会切成多片**（每片 ≤ 8 MiB，rawOffsetBytes 逐片推进）：GB 级镜像不得一次性进内存，
// 消费方（edl_session 数据面）按片推送即可，字节序与 `simg2imgStream` 逐字节一致。
struct SparseChunk {
    quint64    rawOffsetBytes = 0;   // 该块在 raw 镜像中的起始字节
    quint64    rawBytes = 0;         // 覆盖的 raw 字节数
    bool       raw = false;          // 真数据块（data 有效）
    bool       fill = false;         // FILL 块（用 fillValue 填充，data 为空）
    bool       dontCare = false;     // DONT_CARE（不产出数据，调用方仍需推进偏移）
    quint32    fillValue = 0;        // FILL 的 4 字节 pattern，按小端读回的 u32（原样写回即还原）
    QByteArray data;
};

// 流式逐块产出（不落盘）。cb 返回 false 表示调用方要求中止（此时函数返回 false 且 error 说明"被调用方中止"）。
// 块遍历与 `simg2imgStream` 同源（同一套 chunk 头解析与越界判定），仅把"写输出"换成回调。
bool sparseWalk(const QString &inPath, const std::function<bool(const SparseChunk &)> &cb,
                QString *error = nullptr);

} // namespace imgsparse

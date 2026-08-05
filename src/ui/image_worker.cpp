#include "ui/image_worker.h"
#include <QFile>
#include <QFileInfo>

ImageWorker::ImageWorker(QObject *parent)
    : QObject(parent)
{
    // 跨线程 queued 连接要求参数类型运行时已注册（Qt 6 中 Q_DECLARE_METATYPE
    // 生成的 qt_metatype_id() 首次使用即自动注册；此处显式调用双保险）
    qRegisterMetaType<imgreg::Detected>("imgreg::Detected");
    qRegisterMetaType<patcher::PatchConfig>("patcher::PatchConfig");

    // 工作对象迁移到专用线程：runXxx 投递的 QueuedConnection 事件在此线程
    // 的事件循环中按提交顺序串行处理。先迁移后启动，事件队列已就绪。
    moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("image-worker"));
    m_thread.start();
}

ImageWorker::~ImageWorker()
{
    // 停止工作线程并阻塞等待其退出，防止线程悬挂与资源泄漏。
    // 注意：若正在执行长操作，wait() 会等待其完成（Qt 文档建议常规场景
    // 用 finished() 信号驱动收尾；析构场景 quit()+wait() 是标准做法）。
    m_thread.quit();
    m_thread.wait();
}

void ImageWorker::runDetect(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path] { doDetect(path); },
                              Qt::QueuedConnection);
}

void ImageWorker::runUnpack(const QString &path, const QString &outDir,
                            const imgreg::Detected &detected)
{
    QMetaObject::invokeMethod(this,
                              [this, path, outDir, detected] { doUnpack(path, outDir, detected); },
                              Qt::QueuedConnection);
}

void ImageWorker::runPack(const QString &outPath, const QStringList &inputs,
                          const imgreg::Detected &detected)
{
    QMetaObject::invokeMethod(this,
                              [this, outPath, inputs, detected] { doPack(outPath, inputs, detected); },
                              Qt::QueuedConnection);
}

void ImageWorker::runConvert(const QString &path, const QString &outPath,
                             const imgreg::Detected &detected)
{
    QMetaObject::invokeMethod(this,
                              [this, path, outPath, detected] { doConvert(path, outPath, detected); },
                              Qt::QueuedConnection);
}

void ImageWorker::runPatch(const QString &path, const patcher::PatchConfig &config)
{
    QMetaObject::invokeMethod(this, [this, path, config] { doPatch(path, config); },
                              Qt::QueuedConnection);
}

void ImageWorker::doDetect(const QString &path)
{
    emit progress(0, QStringLiteral("识别"));

    imgreg::Detected result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // 契约：失败以 error 形式上报，绝不崩溃
        result.detail = QStringLiteral("读取失败: ") + file.errorString();
        emit detectFinished(path, result);
        return;
    }
    // 魔数判定最大读取 2120B（pac 辅助信号 @2116），4096 一次覆盖全部判定点
    const QByteArray header = file.read(4096);
    file.close();

    if (header.isEmpty()) {
        result.detail = QStringLiteral("读取失败: 文件为空");
        emit detectFinished(path, result);
        return;
    }

    result = imgreg::detect(header, QFileInfo(path).fileName());
    emit progress(100, QStringLiteral("识别"));
    emit detectFinished(path, result);
}

void ImageWorker::doUnpack(const QString &path, const QString &outDir,
                           const imgreg::Detected &detected)
{
    Q_UNUSED(path);
    Q_UNUSED(outDir);
    Q_UNUSED(detected);
    emit progress(0, QStringLiteral("解包"));
    // D3 实现：按 detected.format 分派到对应引擎；当前按契约上报未实现错误
    emit unpackFinished(false, QStringList(),
                        QStringLiteral("解包尚未实现（Task D3 接线）"));
}

void ImageWorker::doPack(const QString &outPath, const QStringList &inputs,
                         const imgreg::Detected &detected)
{
    Q_UNUSED(outPath);
    Q_UNUSED(inputs);
    Q_UNUSED(detected);
    emit progress(0, QStringLiteral("打包"));
    // D6 实现
    emit packFinished(false, QString(),
                      QStringLiteral("打包尚未实现（Task D6 接线）"));
}

void ImageWorker::doConvert(const QString &path, const QString &outPath,
                            const imgreg::Detected &detected)
{
    Q_UNUSED(path);
    Q_UNUSED(outPath);
    Q_UNUSED(detected);
    emit progress(0, QStringLiteral("转换"));
    // D3 实现
    emit convertFinished(false, QString(),
                         QStringLiteral("转换尚未实现（Task D3 接线）"));
}

void ImageWorker::doPatch(const QString &path, const patcher::PatchConfig &config)
{
    Q_UNUSED(path);
    Q_UNUSED(config);
    emit progress(0, QStringLiteral("修补"));
    // D4 实现：调 patcher::patchFile(path, config, &outPath, &error)
    emit patchFinished(false, QString(),
                       QStringLiteral("修补尚未实现（Task D4 接线）"));
}

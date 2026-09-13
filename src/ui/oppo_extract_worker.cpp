#include "oppo_extract_worker.h"

#include "image_engine/oppo_extract.h"

OppoExtractWorker::OppoExtractWorker(QObject *parent)
    : QObject(parent)
{
    // 先迁移后启动（同 ImageWorker）：doExtract 以 QueuedConnection 投递，在此线程的事件
    // 循环里按提交顺序执行。
    moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("oppo-extract-worker"));
    m_thread.start();
}

OppoExtractWorker::~OppoExtractWorker()
{
    // quit()+wait()：等当前解包跑完再退出（引擎无半途停钩子，见类注释）。
    // 调用方（对话框）在取结果前已显式 waitForFinished()，此处属兜底。
    m_thread.quit();
    m_thread.wait();
}

void OppoExtractWorker::start(const QString &packagePath, const QString &outDir)
{
    QMetaObject::invokeMethod(this,
                              [this, packagePath, outDir] { doExtract(packagePath, outDir); },
                              Qt::QueuedConnection);
}

void OppoExtractWorker::requestCancel()
{
    // 只置标志：真正的取消点是引擎循环里的条目边界（imgopp::ExtractCancel）。
    m_cancelRequested.store(true);
}

void OppoExtractWorker::waitForFinished()
{
    // 若工作线程还在跑，quit() 要等当前任务返回后才生效 —— 即"等这一趟解包结束"。
    m_thread.quit();
    m_thread.wait();
}

void OppoExtractWorker::doExtract(const QString &packagePath, const QString &outDir)
{
    // 扩展名分派与 flash_plan_dialog 的入口校验同一判据（.ops vs .ofp；调用方已校验过，
    // 此处按自身事实再判一次，避免"worker 拿到 .ops 却按 OFP 解"）。
    const bool ops = packagePath.toLower().endsWith(QStringLiteral(".ops"));
    const auto progress = [this](const QString &name, int percent) {
        emit this->progress(name, percent);
    };
    const auto cancel = [this] { return m_cancelRequested.load(); };

    QString err;
    m_ok = ops ? imgopp::extractOPS(packagePath, outDir, progress, &err, cancel)
               : imgopp::extractOFP(packagePath, outDir, progress, &err, cancel);
    m_error = err;

    // 先置结果就绪、再发 finished：接收方可能"收到信号后立刻查 isFinished()"，
    // 反过来（先发信号后置位）会给它一个假阴性。
    m_finished.store(true);
    emit finished();
}

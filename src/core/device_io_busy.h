#ifndef DEVICE_IO_BUSY_H
#define DEVICE_IO_BUSY_H

#include <QSet>
#include <QString>

#include <mutex>

// 按设备记录"正在被写操作占用"（刷机 / 擦除 / push / sideload ...）。
//
// 为什么不是全局暂停：刷机过程中设备本来就会重启、切换模式，设备探测必须
// 继续跑，UI 才能反映这些变化。全局停掉探测会把"设备重启"和"设备消失"
// 一起藏起来，反而更糟。
//
// 这里只做一件事：让探测知道"这台设备正在被写"，从而
//   - 不把它本轮暂时消失判成 disconnected（重启窗口）；
//   - 不重复对它发详情查询（缓存已覆盖）。
// 其余设备照常探测。
//
// 跨线程：写操作在主线程标记，探测在工作线程读取，用 mutex 保护。
namespace deviceio {

inline std::mutex &busyMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline QSet<QString> &busySerials()
{
    static QSet<QString> serials;
    return serials;
}

inline void markBusy(const QString &serial)
{
    if (serial.isEmpty())
        return;
    std::lock_guard<std::mutex> lock(busyMutex());
    busySerials().insert(serial);
}

inline void unmarkBusy(const QString &serial)
{
    if (serial.isEmpty())
        return;
    std::lock_guard<std::mutex> lock(busyMutex());
    busySerials().remove(serial);
}

inline bool isBusy(const QString &serial)
{
    if (serial.isEmpty())
        return false;
    std::lock_guard<std::mutex> lock(busyMutex());
    return busySerials().contains(serial);
}

// RAII：作用域内把这台设备视为"正在写"。析构一定配对解除。
struct Guard {
    explicit Guard(const QString &serial)
        : m_serial(serial)
    {
        markBusy(m_serial);
    }
    ~Guard() { unmarkBusy(m_serial); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;

private:
    QString m_serial;
};

} // namespace deviceio

#endif // DEVICE_IO_BUSY_H

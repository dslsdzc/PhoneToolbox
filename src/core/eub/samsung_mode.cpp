#include "samsung_mode.h"

#include "core/eub/eub_libusb_transport.h"
#include "core/odin/odin_libusb_transport.h"

namespace eub {

SamsungMode samsungModeFor(quint16 vid, quint16 pid,
                           const QList<quint8> &interfaceClasses, bool hasBulkInOut)
{
    // 顺序即契约：EUB 先于 Odin（facts §E2）。两判据互不蕴含。
    if (LibusbEubTransport::isEubDevice(vid, pid))
        return SamsungMode::Eub;
    if (odin::LibusbOdinTransport::isOdinDevice(vid, pid, interfaceClasses, hasBulkInOut))
        return SamsungMode::Odin;
    return SamsungMode::NotSamsung;
}

} // namespace eub

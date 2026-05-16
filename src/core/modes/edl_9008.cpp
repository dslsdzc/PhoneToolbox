#include "edl_9008.h"
#include <libusb.h>
#include <QDebug>
#include <cstring>

// Known EDL VID/PID pairs from bkerler/edl (GPLv3)
static const QList<QPair<uint16_t, uint16_t>> s_knownEDLIds = {
    {0x05c6, 0x9008},  // Qualcomm EDL
    {0x05c6, 0x900e},  // Qualcomm EDL
    {0x05c6, 0x9025},  // Qualcomm EDL
    {0x0fce, 0x9dde},  // Sony EDL
    {0x0fce, 0xade3},  // Sony EDL
    {0x0fce, 0xade5},  // Sony EDL
    {0x0fce, 0xaded},  // Sony EDL
    {0x1199, 0x9062},  // Sierra Wireless EDL
    {0x1199, 0x9070},  // Sierra Wireless EDL
    {0x1199, 0x9090},  // Sierra Wireless EDL
    {0x0846, 0x68e0},  // Netgear EDL
    {0x19d2, 0x0076},  // ZTE Download
};

EDL9008::EDL9008(QObject *parent)
    : QObject(parent)
    , m_usbCtx(nullptr)
    , m_initialized(false)
{
}

EDL9008::~EDL9008()
{
    exitUSB();
}

bool EDL9008::initUSB()
{
    if (m_initialized) return true;
    int ret = libusb_init(&m_usbCtx);
    if (ret != 0) {
        qWarning() << "EDL9008: libusb_init failed:" << libusb_error_name(ret);
        return false;
    }
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(m_usbCtx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
#endif
    m_initialized = true;
    return true;
}

void EDL9008::exitUSB()
{
    if (m_usbCtx) {
        libusb_exit(m_usbCtx);
        m_usbCtx = nullptr;
    }
    m_initialized = false;
}

bool EDL9008::isEDLDevice(uint16_t vid, uint16_t pid) const
{
    for (const auto &id : s_knownEDLIds) {
        if (id.first == vid && id.second == pid)
            return true;
    }
    return false;
}

const QList<QPair<uint16_t, uint16_t>> &EDL9008::knownEDLIds()
{
    return s_knownEDLIds;
}

QList<EDLDeviceInfo> EDL9008::listDevices()
{
    QList<EDLDeviceInfo> devices;
    if (!initUSB()) return devices;

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(m_usbCtx, &list);
    if (count < 0) {
        qWarning() << "EDL9008: libusb_get_device_list failed";
        return devices;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *dev = list[i];
        libusb_device_descriptor desc;
        int ret = libusb_get_device_descriptor(dev, &desc);
        if (ret != 0) continue;

        if (!isEDLDevice(desc.idVendor, desc.idProduct))
            continue;

        EDLDeviceInfo info;
        info.vid = desc.idVendor;
        info.pid = desc.idProduct;
        info.busNumber = libusb_get_bus_number(dev);
        info.deviceAddress = libusb_get_device_address(dev);

        // Try to read serial number
        libusb_device_handle *handle = nullptr;
        ret = libusb_open(dev, &handle);
        if (ret == 0 && handle) {
            unsigned char buf[256] = {};
            ret = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, buf, sizeof(buf));
            if (ret > 0)
                info.serialNumber = QString::fromUtf8(reinterpret_cast<const char *>(buf), ret);
            libusb_close(handle);
        }

        devices.append(info);
    }

    libusb_free_device_list(list, 1);
    return devices;
}

bool EDL9008::detectDevice(EDLDeviceInfo &info)
{
    auto devices = listDevices();
    if (devices.isEmpty()) return false;
    info = devices.first();
    return true;
}

QString EDL9008::describeDevice(const EDLDeviceInfo &info) const
{
    return QString("EDL Device [%1:%2] Bus %3 Addr %4%5")
        .arg(info.vid, 4, 16, QChar('0'))
        .arg(info.pid, 4, 16, QChar('0'))
        .arg(info.busNumber)
        .arg(info.deviceAddress)
        .arg(info.serialNumber.isEmpty() ? "" : " SN: " + info.serialNumber);
}

#ifndef EDL_9008_H
#define EDL_9008_H

// EDL (Emergency Download) mode detection via USB enumeration
//
// Based on bkerler/edl (GPLv3) - see edl/LICENSE
// Copyright (C) B.Kerler 2018-2025
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include <QObject>
#include <QString>
#include <QList>
#include <QPair>

struct EDLDeviceInfo {
    uint16_t vid;
    uint16_t pid;
    QString serialNumber;
    uint8_t busNumber;
    uint8_t deviceAddress;
};

class EDL9008 : public QObject
{
    Q_OBJECT

public:
    explicit EDL9008(QObject *parent = nullptr);
    ~EDL9008();

    bool detectDevice(EDLDeviceInfo &info);
    QList<EDLDeviceInfo> listDevices();
    QString describeDevice(const EDLDeviceInfo &info) const;

    static const QList<QPair<uint16_t, uint16_t>> &knownEDLIds();

private:
    struct libusb_context *m_usbCtx;
    bool m_initialized;

    bool initUSB();
    void exitUSB();
    bool isEDLDevice(uint16_t vid, uint16_t pid) const;
};

#endif // EDL_9008_H

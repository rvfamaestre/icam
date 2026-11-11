#pragma once

// usbmuxd client. On Windows, Apple Mobile Device Service (or the Apple Devices app) serves the
// usbmuxd plist protocol on TCP 127.0.0.1:27015; we verified on this PC that the listener comes up
// once an iPhone is attached. A successful Connect turns the socket into a raw TCP tunnel to a port
// on the phone.

#include <cstdint>
#include <string>
#include <vector>

#include "icam/net.h"

namespace icam::usbmux {

struct Device {
    uint32_t id = 0;            // usbmuxd DeviceID (changes on every re-attach)
    std::string serial;         // UDID
    std::string connection;     // "USB" or "Network"
    uint32_t product_id = 0;    // USB PID, e.g. 0x12A8 for iPhone
};

enum class ConnectResult {
    Ok,
    NoService,      // usbmuxd not reachable (Apple Mobile Device Support missing/stopped)
    BadDevice,      // device id no longer valid (unplugged)
    Refused,        // nothing listening on that port: app not open / not in foreground
    ProtocolError,
};

const char* ToString(ConnectResult r);

// Separates a missing USB device from a device Windows sees but hasn't started. No system changes.
enum class UsbPresence { Absent, NotStarted, Started };
UsbPresence WindowsUsbPresence();

// Address of usbmuxd: ICAM_USBMUXD=host:port overrides 127.0.0.1:27015 (tests).
void ServiceAddress(std::string& host, uint16_t& port);

// Lists attached devices. Returns false if usbmuxd is unreachable or replies garbage.
bool ListDevices(std::vector<Device>& out, int timeout_ms = 2000);

// Opens a tunnel to `port` on the device. On Ok, `sock` is the tunnel.
ConnectResult Connect(uint32_t device_id, uint16_t port, net::Socket& sock, int timeout_ms = 3000);

// Low-level: one request/response exchange on an open usbmuxd socket (exposed for tests).
bool Exchange(net::Socket& s, const std::string& request_xml, uint32_t tag, std::string& reply_xml,
              int timeout_ms);

}  // namespace icam::usbmux

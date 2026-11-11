#include "icam/usbmux.h"

#include <cstdlib>
#include <cstring>
#include <windows.h>
#include <cfgmgr32.h>

#include "icam/bytes.h"
#include "icam/plist.h"

namespace icam::usbmux {

namespace {
constexpr uint32_t kPlistVersion = 1;
constexpr uint32_t kPlistMessage = 8;
constexpr uint32_t kMaxReply = 1024 * 1024;
const char* kClient = "icam-agent";
}  // namespace

const char* ToString(ConnectResult r) {
    switch (r) {
        case ConnectResult::Ok: return "ok";
        case ConnectResult::NoService: return "usbmuxd unreachable";
        case ConnectResult::BadDevice: return "bad device";
        case ConnectResult::Refused: return "connection refused";
        case ConnectResult::ProtocolError: return "protocol error";
    }
    return "?";
}

UsbPresence WindowsUsbPresence() {
    ULONG len = 0;
    constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    if (CM_Get_Device_ID_List_SizeW(&len, L"USB", flags) != CR_SUCCESS || !len) return UsbPresence::Absent;
    std::vector<wchar_t> ids(len);
    if (CM_Get_Device_ID_ListW(L"USB", ids.data(), len, flags) != CR_SUCCESS) return UsbPresence::Absent;
    UsbPresence result = UsbPresence::Absent;
    for (wchar_t* p = ids.data(); *p; p += wcslen(p) + 1) {
        if (!wcsstr(p, L"VID_05AC&PID_12") || wcsstr(p, L"&MI_")) continue;
        result = UsbPresence::NotStarted;
        DEVINST node;
        ULONG status = 0, problem = 0;
        if (CM_Locate_DevNodeW(&node, p, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
            CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS && (status & DN_STARTED))
            return UsbPresence::Started;
    }
    return result;
}

void ServiceAddress(std::string& host, uint16_t& port) {
    host = "127.0.0.1";
    port = 27015;
    char buf[128] = {};
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof(buf), "ICAM_USBMUXD") == 0 && n > 1) {
        std::string v(buf);
        size_t c = v.rfind(':');
        if (c != std::string::npos) {
            host = v.substr(0, c);
            port = static_cast<uint16_t>(std::atoi(v.c_str() + c + 1));
        }
    }
}

bool Exchange(net::Socket& s, const std::string& request_xml, uint32_t tag, std::string& reply_xml,
              int timeout_ms) {
    ByteWriter w;
    w.u32(static_cast<uint32_t>(16 + request_xml.size()));
    w.u32(kPlistVersion);
    w.u32(kPlistMessage);
    w.u32(tag);
    w.bytes(request_xml.data(), request_xml.size());
    if (!s.SendAll(w.data().data(), w.size(), timeout_ms)) return false;
    uint8_t hdr[16];
    if (!s.RecvAll(hdr, 16, timeout_ms)) return false;
    ByteReader r(hdr, 16);
    uint32_t len = 0, ver = 0, msg = 0, rtag = 0;
    r.u32(len);
    r.u32(ver);
    r.u32(msg);
    r.u32(rtag);
    if (len < 16 || len - 16 > kMaxReply || msg != kPlistMessage) return false;
    reply_xml.resize(len - 16);
    if (len > 16 && !s.RecvAll(reply_xml.data(), len - 16, timeout_ms)) return false;
    return true;
}

static std::string Request(const std::string& type) {
    return plist::DictWriter()
        .String("MessageType", type)
        .String("ClientVersionString", kClient)
        .String("ProgName", kClient)
        .Xml();
}

bool ListDevices(std::vector<Device>& out, int timeout_ms) {
    out.clear();
    std::string host;
    uint16_t port;
    ServiceAddress(host, port);
    net::Socket s;
    if (!s.Connect(host, port, timeout_ms)) return false;
    std::string reply;
    if (!Exchange(s, Request("ListDevices"), 1, reply, timeout_ms)) return false;
    plist::Node root;
    if (!plist::Parse(reply, root) || root.kind != plist::Node::Kind::Dict) return false;
    const plist::Node* list = root.Get("DeviceList");
    if (!list || list->kind != plist::Node::Kind::Array) return false;
    for (const auto& d : list->array) {
        const plist::Node* props = d.Get("Properties");
        if (!props) continue;
        Device dev;
        dev.id = static_cast<uint32_t>(d.GetInt("DeviceID", props->GetInt("DeviceID", 0)));
        dev.serial = props->GetString("SerialNumber");
        dev.connection = props->GetString("ConnectionType");
        dev.product_id = static_cast<uint32_t>(props->GetInt("ProductID", 0));
        if (dev.id != 0) out.push_back(dev);
    }
    return true;
}

ConnectResult Connect(uint32_t device_id, uint16_t port, net::Socket& sock, int timeout_ms) {
    std::string host;
    uint16_t mux_port;
    ServiceAddress(host, mux_port);
    net::Socket s;
    if (!s.Connect(host, mux_port, timeout_ms)) return ConnectResult::NoService;
    // usbmuxd expects the port in network byte order, read as a host integer.
    uint16_t swapped = static_cast<uint16_t>((port >> 8) | (port << 8));
    std::string req = plist::DictWriter()
                          .String("MessageType", "Connect")
                          .Integer("DeviceID", device_id)
                          .Integer("PortNumber", swapped)
                          .String("ClientVersionString", kClient)
                          .String("ProgName", kClient)
                          .Xml();
    std::string reply;
    if (!Exchange(s, req, 2, reply, timeout_ms)) return ConnectResult::ProtocolError;
    plist::Node root;
    if (!plist::Parse(reply, root)) return ConnectResult::ProtocolError;
    int64_t n = root.GetInt("Number", -1);
    switch (n) {
        case 0:
            sock = std::move(s);
            return ConnectResult::Ok;
        case 2: return ConnectResult::BadDevice;
        case 3: return ConnectResult::Refused;
        default: return ConnectResult::ProtocolError;
    }
}

}  // namespace icam::usbmux

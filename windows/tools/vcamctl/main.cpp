// icam-vcamctl: registers/removes the "iPhone Camera" virtual camera and helps diagnose it.
//
//   icam-vcamctl register      Persistent virtual camera for the calling user only.
//   icam-vcamctl remove        Remove the calling user's virtual camera.
//   icam-vcamctl remove-legacy Remove the old all-users registration (installer migration, admin).
//   icam-vcamctl list          List video capture devices visible to Media Foundation.
//   icam-vcamctl status        Dump the shared-memory header (agent/camera state).
//   icam-vcamctl feed [secs]   Act as a synthetic agent: write test-pattern frames at the size the
//                              camera's consumer requested (vertical slice without the iPhone).

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "icam/image.h"
#include "icam/protocol.h"
#include "icam/usbmux.h"
#include "icam/shm.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClsid[] = L"{4883E02E-087A-4FE6-8468-2339757D3864}";
constexpr wchar_t kName[] = L"iPhone Camera";

int Fail(const char* what, HRESULT hr) {
    std::fprintf(stderr, "%s failed: 0x%08lx\n", what, hr);
    return 1;
}

HRESULT OpenCamera(ComPtr<IMFVirtualCamera>& cam, MFVirtualCameraAccess access = MFVirtualCameraAccess_CurrentUser) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                 access, kName, kClsid, nullptr, 0, &cam);
}

int Register() {
    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = OpenCamera(cam);
    if (FAILED(hr)) return Fail("MFCreateVirtualCamera", hr);
    hr = cam->Start(nullptr);
    if (FAILED(hr)) return Fail("IMFVirtualCamera::Start", hr);
    std::printf("Registered \"%ls\" (source %ls)\n", kName, kClsid);
    return 0;
}

int Remove(MFVirtualCameraAccess access = MFVirtualCameraAccess_CurrentUser) {
    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = OpenCamera(cam, access);
    if (FAILED(hr)) return Fail("MFCreateVirtualCamera", hr);
    hr = cam->Remove();
    if (FAILED(hr)) return Fail("IMFVirtualCamera::Remove", hr);
    std::printf("Removed \"%ls\"\n", kName);
    return 0;
}

int List() {
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 1);
    a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
    if (FAILED(hr)) return Fail("MFEnumDeviceSources", hr);
    for (UINT32 i = 0; i < n; ++i) {
        wchar_t* name = nullptr;
        wchar_t* link = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
        std::printf("%u: %ls\n   %ls\n", i, name ? name : L"?", link ? link : L"?");
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    std::printf("%u device(s)\n", n);
    return 0;
}

int Status() {
    icam::shm::Objects o;
    if (!o.Open()) {
        std::printf("shared memory not present (error %lu): no app has opened the camera since it was loaded\n",
                    o.last_error());
        return 0;
    }
    const auto* h = o.header();
    ULONGLONG now = GetTickCount64();
    std::printf("consumer_active=%ld request=%ux%u@%u seq=%ld vcam_pid=%u vcam_heartbeat=%lldms ago\n",
                h->consumer_active, h->req_width, h->req_height, h->req_fps, h->request_seq, h->vcam_pid,
                h->vcam_heartbeat_ms ? (long long)(now - h->vcam_heartbeat_ms) : -1);
    std::printf("agent_pid=%u agent_state=%u agent_heartbeat=%lldms ago latest_frame=%lld\n", h->agent_pid,
                h->agent_state, h->agent_heartbeat_ms ? (long long)(now - h->agent_heartbeat_ms) : -1,
                h->latest_frame);
    std::printf("samples=%lld repeats=%lld placeholders=%lld\n", h->vcam_samples, h->vcam_repeats,
                h->vcam_placeholders);
    return 0;
}

// One-line checks of the whole chain, in the order a user would fix them.
int Doctor() {
    int problems = 0;
    auto line = [&](bool ok, const char* what, const std::string& detail) {
        std::printf("[%s] %s%s%s\n", ok ? " OK " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
        if (!ok) ++problems;
    };
    // COM registration (HKLM) of the media source.
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path);
    std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid + L"\\InprocServer32";
    bool com = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS;
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
    line(com && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, "camera DLL registered", narrow);
    // Virtual camera visible to Media Foundation.
    bool listed = false;
    {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 1);
        a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devs = nullptr;
        UINT32 n = 0;
        if (SUCCEEDED(MFEnumDeviceSources(a.Get(), &devs, &n))) {
            for (UINT32 i = 0; i < n; ++i) {
                wchar_t* name = nullptr;
                UINT32 len = 0;
                devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
                if (name && wcsstr(name, kName)) listed = true;
                CoTaskMemFree(name);
                devs[i]->Release();
            }
            CoTaskMemFree(devs);
        }
    }
    line(listed, "\"iPhone Camera\" visible to apps", listed ? "" : "run windows\\scripts\\install.ps1");
    // Apple Mobile Device Service and the iPhone.
    std::vector<icam::usbmux::Device> devices;
    bool mux = icam::usbmux::ListDevices(devices, 1500);
    line(mux, "Apple Mobile Device Service (usbmuxd 127.0.0.1:27015)",
         mux ? "" : "not reachable: install Apple Devices/iTunes, or connect the iPhone (the port opens on attach)");
    const icam::usbmux::Device* usb = nullptr;
    for (const auto& d : devices)
        if (d.connection == "USB") usb = &d;
    std::string usb_help;
    if (!usb) {
        switch (icam::usbmux::WindowsUsbPresence()) {
        case icam::usbmux::UsbPresence::Absent:
            usb_help = "not detected by Windows: connect the cable and unlock the iPhone";
            break;
        case icam::usbmux::UsbPresence::NotStarted:
            usb_help = "Apple USB device is STOPPED in Windows: unplug/replug; restart Windows if it stays stopped";
            break;
        case icam::usbmux::UsbPresence::Started:
            usb_help = "Windows USB is started but Apple's list is empty: replug or restart Apple Mobile Device Service";
            break;
        }
    }
    line(usb != nullptr, "iPhone on USB", usb ? usb->serial : usb_help);
    icam::shm::Objects o;
    const bool have_shm = o.Open();
    bool agent_alive = false, agent_connected = false;
    if (have_shm) {
        const auto* h = o.header();
        agent_alive = h->agent_heartbeat_ms && GetTickCount64() - static_cast<ULONGLONG>(h->agent_heartbeat_ms) < 2000;
        const auto st = static_cast<icam::shm::AgentState>(h->agent_state);
        agent_connected = agent_alive && (st == icam::shm::AgentState::Connected ||
                          st == icam::shm::AgentState::Streaming || st == icam::shm::AgentState::PausedOnPhone);
    }
    if (usb) {
        if (agent_connected) {
            // Opening a second connection would displace the agent (newest-host-wins on iOS).
            line(true, "iCam app on the iPhone", "confirmed by the active agent; connection left untouched");
        } else {
            icam::net::Socket s;
            auto r = icam::usbmux::Connect(usb->id, icam::proto::kDevicePort, s, 2000);
            line(r == icam::usbmux::ConnectResult::Ok, "iCam app listening on the iPhone",
                 r == icam::usbmux::ConnectResult::Ok ? "" : "open iCam on the iPhone and keep it in front");
        }
    }
    // Agent.
    if (have_shm) {
        line(agent_alive, "agent running", agent_alive ? "state " + std::to_string(o.header()->agent_state) : "start icam-agent.exe");
    } else {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ICam.Agent.SingleInstance.Global");
        line(m != nullptr, "agent running", m ? "(no app has opened the camera yet)" : "start icam-agent.exe");
        if (m) CloseHandle(m);
    }
    std::printf("%s\n", problems ? "Fix the first FAIL above, then run doctor again." : "Everything looks fine.");
    return problems ? 1 : 0;
}

int Feed(int seconds) {
    icam::shm::Objects o;
    std::printf("waiting for the camera to be opened by an app...\n");
    ULONGLONG start = GetTickCount64();
    while (!o.Open()) {
        if (seconds > 0 && GetTickCount64() - start > ULONGLONG(seconds) * 1000) return 1;
        Sleep(250);
    }
    auto* h = o.header();
    h->agent_pid = GetCurrentProcessId();
    uint64_t frame = static_cast<uint64_t>(h->latest_frame) + 1;
    icam::image::Nv12 f;
    uint32_t idx = 0;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    std::printf("feeding test pattern (Ctrl+C to stop)\n");
    // Absolute schedule on a high-resolution timer: Sleep() rounds up to the 15.6 ms tick.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    uint64_t next = icam::shm::QpcNow100ns();
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (seconds > 0 && now - start > ULONGLONG(seconds) * 1000) break;
        InterlockedExchange64(&h->agent_heartbeat_ms, static_cast<LONG64>(now));
        h->agent_state = static_cast<uint32_t>(icam::shm::AgentState::Streaming);
        uint32_t w = h->req_width, ht = h->req_height, fps = h->req_fps ? h->req_fps : 30;
        if (!h->consumer_active || w == 0 || ht == 0) {
            Sleep(100);
            continue;
        }
        if (f.width != w || f.height != ht) f.Allocate(w, ht);
        icam::image::DrawTestPattern(f, idx);
        icam::image::Stamp st;
        st.counter = idx & 0xFFFFFF;
        st.qpc_ms = static_cast<uint32_t>(icam::shm::QpcNow100ns() / 10000);
        icam::image::DrawStamp(f.y(), w, w, ht, st);
        icam::shm::WriteFrame(o, frame++, w, ht, f.y(), w, f.uv(), w, true, icam::shm::QpcNow100ns());
        ++idx;
        if (idx % (fps * 5) == 0) std::printf("  %u frames at %ux%u@%u\n", idx, w, ht, fps);
        next += 10000000ull / fps;
        uint64_t now100 = icam::shm::QpcNow100ns();
        if (next < now100) next = now100;  // fell behind: don't burst
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(next - now100);
        if (due.QuadPart < 0 && timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, 1000);
    }
    if (timer) CloseHandle(timer);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: icam-vcamctl register|remove|list|status|doctor|feed [seconds]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    std::wstring cmd = argv[1];
    int rc = 2;
    if (cmd == L"register") rc = Register();
    else if (cmd == L"remove") rc = Remove();
    else if (cmd == L"remove-legacy") rc = Remove(MFVirtualCameraAccess_AllUsers);
    else if (cmd == L"list") rc = List();
    else if (cmd == L"status") rc = Status();
    else if (cmd == L"feed") rc = Feed(argc > 2 ? _wtoi(argv[2]) : 0);
    else if (cmd == L"doctor") rc = Doctor();
    else std::printf("unknown command\n");
    MFShutdown();
    CoUninitialize();
    return rc;
}

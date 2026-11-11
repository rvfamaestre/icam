// icam-agent: runs in the user's session (tray icon), connects to the iPhone and feeds the
// "iPhone Camera" virtual camera.
//
//   icam-agent                      normal start (tray)
//   icam-agent --console            no tray; log to the console too; Ctrl+C to quit
//   icam-agent --tcp host[:port]    connect to fakephone instead of usbmuxd
//   icam-agent --always-on [WxH@F]  stream even when no app has the camera open (measurements)
//   icam-agent --software-decode    don't try the hardware decoder

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>
#include <string>

#include "engine.h"
#include "icam/log.h"
#include "media.h"
#include "tray.h"

using namespace icam;

namespace {

HANDLE g_quit = nullptr;

BOOL WINAPI CtrlHandler(DWORD) {
    if (g_quit) SetEvent(g_quit);
    return TRUE;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    agent::Options opt;
    bool console = false;
    std::wstring log_path;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--console") {
            console = true;
        } else if (a == L"--tcp" && i + 1 < argc) {
            std::string v = Narrow(argv[++i]);
            size_t c = v.rfind(':');
            opt.tcp_host = c == std::string::npos ? v : v.substr(0, c);
            if (c != std::string::npos) opt.tcp_port = static_cast<uint16_t>(std::stoi(v.substr(c + 1)));
        } else if (a == L"--always-on") {
            opt.always_on = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-')
                swscanf_s(argv[++i], L"%ux%u@%u", &opt.always_w, &opt.always_h, &opt.always_fps);
        } else if (a == L"--software-decode") {
            opt.prefer_hw_decode = false;
        } else if (a == L"--log" && i + 1 < argc) {
            log_path = argv[++i];
        }
    }
    LocalFree(argv);

    // One agent per user session and shared-memory namespace (tests run a second agent against
    // ICAM_SHM_NAMESPACE=Local next to the installed one).
    wchar_t ns[32] = L"Global";
    DWORD nslen = GetEnvironmentVariableW(L"ICAM_SHM_NAMESPACE", ns, 32);
    if (nslen == 0 || nslen >= 32) wcscpy_s(ns, L"Global");
    std::wstring mutex_name = std::wstring(L"Local\\ICam.Agent.SingleInstance.") + ns;
    HANDLE single = CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    if (console) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
    std::wstring dir = log::DefaultDir();
    if (log_path.empty() && !dir.empty()) log_path = dir + L"\\agent.log";
    log::Init(log_path, console);
    ICAM_INFO("icam-agent %s starting (pid %lu)%s", ICAM_VERSION, GetCurrentProcessId(),
              opt.tcp_host.empty() ? "" : (" tcp " + opt.tcp_host + ":" + std::to_string(opt.tcp_port)).c_str());

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    media::Startup();
    // The agent must keep running when its window has no focus; don't let EcoQoS throttle the decode.
    PROCESS_POWER_THROTTLING_STATE throttle{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                            PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttle, sizeof(throttle));

    agent::Engine engine(opt, agent::LoadSettings());
    int rc = 0;
    if (console) {
        g_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        engine.Start();
        WaitForSingleObject(g_quit, INFINITE);
    } else {
        agent::Tray tray(engine);
        if (!tray.Create(inst)) {
            ICAM_ERR("cannot create the tray icon");
            return 1;
        }
        engine.Start();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        engine.Stop();  // join engine threads before the callback and the tray window go away
        engine.on_change = nullptr;
    }
    engine.Stop();
    ICAM_INFO("icam-agent exiting");
    media::Shutdown();
    CoUninitialize();
    if (single) CloseHandle(single);
    return rc;
}

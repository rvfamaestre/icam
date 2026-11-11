// Tray UI: a status icon whose colour follows the agent state, and a small menu (camera, mirror,
// autostart, logs, exit). Settings live under HKCU\Software\iCam.

#include "tray.h"

#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "icam/log.h"

namespace icam::agent {

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kRefreshMsg = WM_APP + 2;
constexpr UINT kTrayId = 1;
enum : UINT { kCmdFront = 100, kCmdBack, kCmdMirror, kCmdMirrorCards, kCmdAutostart, kCmdLogs, kCmdExit };
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kSettingsKey[] = L"Software\\iCam";

DWORD ReadDword(const wchar_t* name, DWORD def) {
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return def;
    return v;
}

void WriteDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"iCam", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"iCam", REG_SZ, v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
    } else {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"iCam");
            RegCloseKey(k);
        }
    }
}

// Draws a 32x32 round status badge with a small "camera" mark.
HICON MakeIcon(COLORREF fill) {
    const int s = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = s;
    bi.bV5Height = -s;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return LoadIcon(nullptr, IDI_APPLICATION);
    auto* px = static_cast<uint32_t*>(bits);
    const uint32_t r = GetRValue(fill), g = GetGValue(fill), b = GetBValue(fill);
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            double dx = x - 15.5, dy = y - 15.5, d = dx * dx + dy * dy;
            uint32_t a = d <= 15.0 * 15.0 ? 255 : (d <= 16.0 * 16.0 ? 128 : 0);
            uint32_t cr = r, cg = g, cb = b;
            // White camera body (rectangle) and lens (dark dot).
            bool body = x >= 8 && x <= 23 && y >= 11 && y <= 21;
            bool bump = x >= 12 && x <= 17 && y >= 9 && y <= 10;
            double lx = x - 15.5, ly = y - 16.0;
            bool lens = lx * lx + ly * ly <= 9.0;
            if ((body || bump) && a) cr = cg = cb = 255;
            if (lens && a) {
                cr = r / 2;
                cg = g / 2;
                cb = b / 2;
            }
            // Premultiplied alpha.
            px[y * s + x] = (a << 24) | ((cr * a / 255) << 16) | ((cg * a / 255) << 8) | (cb * a / 255);
        }
    }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

COLORREF StateColor(shm::AgentState s) {
    switch (s) {
        case shm::AgentState::Streaming: return RGB(220, 50, 47);       // red: camera live
        case shm::AgentState::Connected: return RGB(46, 160, 67);       // green: ready
        case shm::AgentState::AppNotOpen:
        case shm::AgentState::PausedOnPhone:
        case shm::AgentState::NoAppleService: return RGB(214, 150, 20); // amber: needs the user
        default: return RGB(120, 120, 128);                             // grey: no phone
    }
}

}  // namespace

Settings LoadSettings() {
    Settings s;
    s.position = ReadDword(L"Position", 2) == 1 ? proto::Position::Back : proto::Position::Front;
    s.mirror = ReadDword(L"Mirror", 0) != 0;
    s.mirror_cards = ReadDword(L"MirrorCards", 1) != 0;
    return s;
}

void SaveSettings(const Settings& s) {
    WriteDword(L"Position", s.position == proto::Position::Back ? 1 : 2);
    WriteDword(L"Mirror", s.mirror ? 1 : 0);
    WriteDword(L"MirrorCards", s.mirror_cards ? 1 : 0);
}

Tray::Tray(Engine& engine) : engine_(engine) {}

Tray::~Tray() {
    if (hwnd_) {
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(hwnd_);
    }
    if (icon_) DestroyIcon(icon_);
}

bool Tray::Create(HINSTANCE inst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &Tray::WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"ICamAgentTray";
    RegisterClassW(&wc);
    hwnd_ = CreateWindowW(wc.lpszClassName, L"iCam", 0, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    AddIcon();
    engine_.on_change = [hwnd = hwnd_] { PostMessageW(hwnd, kRefreshMsg, 0, 0); };
    return true;
}

void Tray::AddIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    icon_state_ = shm::AgentState::Unknown;
    icon_ = MakeIcon(StateColor(icon_state_));
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"iCam");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void Tray::Refresh() {
    Snapshot s = engine_.GetSnapshot();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    if (s.state != icon_state_) {
        HICON old = icon_;
        icon_ = MakeIcon(StateColor(s.state));
        icon_state_ = s.state;
        nid.uFlags |= NIF_ICON;
        nid.hIcon = icon_;
        if (old) DestroyIcon(old);
    }
    std::wstring tip = L"iCam — " + s.title;
    if (s.state == shm::AgentState::Streaming)
        tip += L"\n" + std::to_wstring(s.width) + L"×" + std::to_wstring(s.height) + L" " +
               std::to_wstring(static_cast<int>(s.fps_decoded + 0.5)) + L" fps";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::ShowMenu() {
    Snapshot s = engine_.GetSnapshot();
    Settings st = engine_.GetSettings();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"iCam — " + s.title).c_str());
    if (!s.detail.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s.detail.c_str());
    if (s.state == shm::AgentState::Streaming) {
        wchar_t line[200];
        swprintf_s(line, L"%u×%u · %.0f fps · %.1f Mbps · %.0f ms · %hs", s.width, s.height, s.fps_decoded, s.mbps,
                   s.latency_ms, s.decoder.c_str());
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
        if (s.phone.thermal >= 2) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"⚠ iPhone caliente: calidad reducida");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Show the camera the phone is actually using once it has told us.
    const proto::Position shown = s.phone_camera != proto::Position::Unspecified ? s.phone_camera : st.position;
    AppendMenuW(m, MF_STRING | (shown == proto::Position::Front ? MF_CHECKED : 0), kCmdFront, L"Cámara frontal");
    AppendMenuW(m, MF_STRING | (shown == proto::Position::Back ? MF_CHECKED : 0), kCmdBack, L"Cámara trasera");
    AppendMenuW(m, MF_STRING | (st.mirror ? MF_CHECKED : 0), kCmdMirror, L"Espejo horizontal");
    AppendMenuW(m, MF_STRING | (st.mirror_cards ? MF_CHECKED : 0), kCmdMirrorCards, L"Avisos compatibles con vista espejo");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kCmdAutostart, L"Iniciar con Windows");
    AppendMenuW(m, MF_STRING, kCmdLogs, L"Abrir registros");
    AppendMenuW(m, MF_STRING, kCmdExit, L"Salir");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    switch (cmd) {
        case kCmdFront:
        case kCmdBack:
            st.position = cmd == kCmdFront ? proto::Position::Front : proto::Position::Back;
            SaveSettings(st);
            engine_.SelectPosition(st.position);
            break;
        case kCmdMirror:
            st.mirror = !st.mirror;
            SaveSettings(st);
            engine_.UpdateSettings(st);
            break;
        case kCmdMirrorCards:
            st.mirror_cards = !st.mirror_cards;
            SaveSettings(st);
            engine_.UpdateSettings(st);
            break;
        case kCmdAutostart: SetAutostart(!AutostartEnabled()); break;
        case kCmdLogs: {
            std::wstring dir = log::DefaultDir();
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case kCmdExit: PostQuitMessage(0); break;
        default: break;
    }
}

LRESULT CALLBACK Tray::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Tray* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, wp, lp);
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    if (msg == self->taskbar_created_ && msg != 0) {  // Explorer restarted
        self->AddIcon();
        return 0;
    }
    switch (msg) {
        case kTrayMsg:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT: self->ShowMenu(); return 0;
            }
            return 0;
        case kRefreshMsg: self->Refresh(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace icam::agent

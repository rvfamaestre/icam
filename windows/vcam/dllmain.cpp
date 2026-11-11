// COM plumbing for icam-vcam.dll: class factory (WRL), self-registration under HKLM, the activator
// that Frame Server instantiates, and a small file trace shared by every process that loads us.

#include "vcam.h"
#include "icam/log.h"

#include <cstdarg>
#include <cstdio>
#include <string>

using namespace Microsoft::WRL;

namespace icam::vcam {

namespace {
HMODULE g_module = nullptr;
HANDLE g_trace = INVALID_HANDLE_VALUE;
SRWLOCK g_trace_lock = SRWLOCK_INIT;
bool g_trace_tried = false;

void OpenTrace() {
    g_trace_tried = true;
    wchar_t dir[MAX_PATH];
    std::wstring path;
    DWORD n = GetEnvironmentVariableW(L"ICAM_VCAM_LOG", dir, MAX_PATH);  // development override
    if (n > 0 && n < MAX_PATH) {
        path = dir;
    } else {
        const auto logs = log::DefaultDir();  // service profile for Frame Server, user profile in tests
        if (logs.empty()) return;
        path = logs + L"\\vcam.log";
    }
    // Keep the file small: start over once it passes 2 MB.
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 2 * 1024 * 1024) {
        std::wstring old = path + L".1";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_trace = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
}  // namespace

void Trace(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[640];
    int len = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu:%lu] %s\r\n", t.wYear, t.wMonth,
                       t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(),
                       GetCurrentThreadId(), msg);
    OutputDebugStringA(line);
    AcquireSRWLockExclusive(&g_trace_lock);
    if (!g_trace_tried) OpenTrace();
    if (g_trace != INVALID_HANDLE_VALUE && len > 0) {
        DWORD written = 0;
        WriteFile(g_trace, line, static_cast<DWORD>(len < (int)sizeof(line) ? len : sizeof(line) - 1), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_trace_lock);
}

Activator::Activator() { MFCreateAttributes(&attrs_, 4); }

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    if (!source_) {
        ComPtr<MediaSource> s = Make<MediaSource>();
        HRESULT hr = s ? s->Initialize(attrs_.Get()) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) source_ = s;
        if (FAILED(hr)) {
            Trace("ActivateObject: source init failed 0x%08lx", hr);
            return hr;
        }
    }
    return source_->QueryInterface(riid, ppv);
}

STDMETHODIMP Activator::ShutdownObject() {
    std::lock_guard<std::mutex> lock(mu_);
    if (source_) {
        source_->Shutdown();
        source_.Reset();
    }
    return S_OK;
}

STDMETHODIMP Activator::DetachObject() {
    std::lock_guard<std::mutex> lock(mu_);
    source_.Reset();
    return S_OK;
}

}  // namespace icam::vcam

using icam::vcam::Activator;
CoCreatableClass(Activator);

namespace {

std::wstring ModulePath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(icam::vcam::g_module, path, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(path, n) : std::wstring();
}

LSTATUS SetValue(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
    HKEY k;
    LSTATUS s = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (s != ERROR_SUCCESS) return s;
    s = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return s;
}

const std::wstring kClsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + icam::vcam::kClsidString;

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No DisableThreadLibraryCalls: the static CRT needs thread notifications.
        icam::vcam::g_module = module;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) {
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, ppv);
}

STDAPI DllCanUnloadNow() { return Module<InProc>::GetModule().Terminate() ? S_OK : S_FALSE; }

// Machine-wide registration is required: Frame Server (LocalService) and the Frame Server Monitor
// (LocalSystem) can't see per-user COM classes.
STDAPI DllRegisterServer() {
    std::wstring path = ModulePath();
    if (path.empty()) return E_UNEXPECTED;
    LSTATUS s = SetValue(HKEY_LOCAL_MACHINE, kClsidKey, nullptr, L"iCam iPhone Camera Source");
    if (s == ERROR_SUCCESS) s = SetValue(HKEY_LOCAL_MACHINE, kClsidKey + L"\\InprocServer32", nullptr, path);
    if (s == ERROR_SUCCESS) s = SetValue(HKEY_LOCAL_MACHINE, kClsidKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return HRESULT_FROM_WIN32(s);
}

STDAPI DllUnregisterServer() {
    LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey.c_str());
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}

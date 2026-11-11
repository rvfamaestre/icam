#include "icam/log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <mutex>

namespace icam::log {

namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
std::wstring g_path;
bool g_stderr = false;
constexpr long kMaxBytes = 4 * 1024 * 1024;

void Rotate() {
    if (!g_file || ftell(g_file) < kMaxBytes) return;
    fclose(g_file);
    std::wstring old = g_path + L".1";
    DeleteFileW(old.c_str());
    MoveFileW(g_path.c_str(), old.c_str());
    g_file = _wfsopen(g_path.c_str(), L"a", _SH_DENYNO);
}
}  // namespace

std::wstring DefaultDir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &p))) {
        dir = std::wstring(p) + L"\\iCam";
        CoTaskMemFree(p);
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

void Init(const std::wstring& file_path, bool to_stderr) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
    g_path = file_path;
    g_stderr = to_stderr;
    if (!file_path.empty()) g_file = _wfsopen(file_path.c_str(), L"a", _SH_DENYNO);
}

void Write(const char* level, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u %s [%lu] %s\n", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, GetCurrentThreadId(), msg);
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
        Rotate();
    }
    if (g_stderr) fputs(line, stderr);
    OutputDebugStringA(line);
}

}  // namespace icam::log

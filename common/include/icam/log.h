#pragma once

// Tiny thread-safe logger: timestamped lines to a file (rotated at 4 MB), the debugger and,
// optionally, stderr. No allocation-heavy formatting on hot paths; callers rate-limit.

#include <cstdarg>
#include <string>

namespace icam::log {

void Init(const std::wstring& file_path, bool to_stderr);
void Write(const char* level, const char* fmt, ...);
std::wstring DefaultDir();  // %LOCALAPPDATA%\iCam\logs (agent) — created on demand
std::string Utf8(const std::wstring& w);  // for logging wide strings with %s

}  // namespace icam::log

#define ICAM_INFO(...) ::icam::log::Write("I", __VA_ARGS__)
#define ICAM_WARN(...) ::icam::log::Write("W", __VA_ARGS__)
#define ICAM_ERR(...) ::icam::log::Write("E", __VA_ARGS__)

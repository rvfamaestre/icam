#include "icam/shm.h"

#include <sddl.h>
#include <aclapi.h>

#include <cstring>
#include <vector>

namespace icam::shm {

namespace {

bool LocalTestNamespace() {
    wchar_t ns[32] = {};
    return GetEnvironmentVariableW(L"ICAM_SHM_NAMESPACE", ns, 32) == 5 && wcscmp(ns, L"Local") == 0;
}

std::wstring AgentSid() {
    std::wstring sid;
    if (LocalTestNamespace()) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<uint8_t> info(size);
        if (GetTokenInformation(token, TokenUser, info.data(), size, &size)) {
            LPWSTR text = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid, &text)) {
                sid = text;
                LocalFree(text);
            }
        }
        CloseHandle(token);
    } else {
        wchar_t value[192] = {};
        DWORD bytes = sizeof(value);
        // Only the installer (administrator) may select the account that owns the camera.
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\iCam", L"AgentUserSid",
                         RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &bytes) == ERROR_SUCCESS)
            sid = value;
    }
    PSID parsed = nullptr;
    if (sid.empty() || !ConvertStringSidToSidW(sid.c_str(), &parsed)) return {};
    const bool valid = IsValidSid(parsed) && !IsWellKnownSid(parsed, WinWorldSid) &&
                       !IsWellKnownSid(parsed, WinInteractiveSid) && !IsWellKnownSid(parsed, WinAuthenticatedUserSid);
    LocalFree(parsed);
    return valid ? sid : std::wstring();
}

PSECURITY_DESCRIPTOR ObjectSecurity(bool mapping) {
    const auto sid = AgentSid();
    if (sid.empty()) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    // No IU/BU/Everyone access. The selected user and the two Frame Server service identities only.
    // Use object-specific masks: generic access is normalized differently for sections and events.
    const auto sddl = mapping
        ? L"D:P(A;;0xF001F;;;SY)(A;;0xF001F;;;LS)(A;;0x20006;;;" + sid + L")"
        : L"D:P(A;;0x1F0003;;;SY)(A;;0x1F0003;;;LS)(A;;0x120002;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return nullptr;
    return sd;
}

bool TrustedObject(HANDLE object, bool mapping) {
    PSECURITY_DESCRIPTOR expected = ObjectSecurity(mapping);
    if (!expected) return false;
    PACL actual = nullptr, wanted = nullptr;
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
                        &owner, nullptr, &actual, nullptr, &sd) != ERROR_SUCCESS) { LocalFree(expected); return false; }
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorDacl(expected, &present, &wanted, &defaulted);
    // Reject objects pre-created with a permissive ACL, including legacy iCam objects.
    bool valid = actual && wanted && actual->AceCount == wanted->AceCount;
    for (DWORD i = 0; valid && i < wanted->AceCount; ++i) {
        void *a = nullptr, *b = nullptr;
        valid = GetAce(actual, i, &a) && GetAce(wanted, i, &b);
        if (valid) {
            auto ah = static_cast<ACE_HEADER*>(a), bh = static_cast<ACE_HEADER*>(b);
            valid = ah->AceSize == bh->AceSize && std::memcmp(a, b, ah->AceSize) == 0;
        }
    }
    PSID user = nullptr;
    const auto sid = AgentSid();
    if (ConvertStringSidToSidW(sid.c_str(), &user)) {
        valid = valid && owner && (EqualSid(owner, user) || IsWellKnownSid(owner, WinLocalSystemSid) ||
                                 IsWellKnownSid(owner, WinLocalServiceSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
        LocalFree(user);
    } else valid = false;
    LocalFree(sd);
    LocalFree(expected);
    if (!valid) SetLastError(ERROR_ACCESS_DENIED);
    return valid;
}

}  // namespace

std::wstring Name(const wchar_t* base) {
    return std::wstring(LocalTestNamespace() ? L"Local\\" : L"Global\\") + base;
}

uint64_t QpcNow100ns() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // Split to avoid overflow: (c / f) * 1e7 + (c % f) * 1e7 / f
    uint64_t q = static_cast<uint64_t>(c.QuadPart), f = static_cast<uint64_t>(freq.QuadPart);
    return (q / f) * 10000000ull + (q % f) * 10000000ull / f;
}

bool HeaderLooksValid(const Header* h) {
    return h && h->magic == kMagic && h->version == kVersion && h->header_bytes == kHeaderBytes &&
           h->slot_count == kSlots && h->slot_bytes == kSlotBytes && h->total_bytes == kTotalBytes;
}

bool Objects::Map(HANDLE mapping, bool init) {
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(kTotalBytes));
    if (!view) {
        last_error_ = GetLastError();
        return false;
    }
    header_ = static_cast<Header*>(view);
    if (init) {
        std::memset(header_, 0, kHeaderBytes);
        header_->magic = kMagic;
        header_->version = kVersion;
        header_->header_bytes = kHeaderBytes;
        header_->slot_count = kSlots;
        header_->slot_bytes = kSlotBytes;
        header_->total_bytes = kTotalBytes;
        for (uint32_t i = 0; i < kSlots; ++i) header_->slots[i].data_offset = SlotOffset(i);  // informational
        MemoryBarrier();
    }
    if (!HeaderLooksValid(header_)) {
        UnmapViewOfFile(header_);
        header_ = nullptr;
        last_error_ = ERROR_INVALID_DATA;
        return false;
    }
    return true;
}

bool Objects::Create() {
    Close();
    last_error_ = 0;
    PSECURITY_DESCRIPTOR sd = ObjectSecurity(true);
    if (!sd) {
        last_error_ = GetLastError();
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, static_cast<DWORD>(kTotalBytes >> 32),
                                  static_cast<DWORD>(kTotalBytes & 0xFFFFFFFF), Name(kFramesBase).c_str());
    DWORD err = GetLastError();
    bool existed = mapping_ && err == ERROR_ALREADY_EXISTS;
    LocalFree(sd);
    sd = ObjectSecurity(false);
    sa.lpSecurityDescriptor = sd;
    if (mapping_ && sd) {
        frame_ready_ = CreateEventW(&sa, FALSE, FALSE, Name(kFrameReadyBase).c_str());
        control_ = CreateEventW(&sa, FALSE, FALSE, Name(kControlBase).c_str());
    } else {
        last_error_ = err;
    }
    DWORD create_err = GetLastError();
    if (!mapping_ || !frame_ready_ || !control_) {
        LocalFree(sd);
        if (!last_error_) last_error_ = create_err;
        Close();
        // Objects that already exist are opened with *_ALL_ACCESS by Create*; a caller that only has
        // the selected user's rights of our DACL (tests, a second non-service host) opens them instead.
        if (last_error_ == ERROR_ACCESS_DENIED) return Open();
        return false;
    }
    const bool trusted = TrustedObject(mapping_, true) && TrustedObject(frame_ready_, false) && TrustedObject(control_, false);
    LocalFree(sd);
    if (!trusted) { last_error_ = ERROR_ACCESS_DENIED; Close(); return false; }
    // Reuse a valid existing layout (the agent may hold it across a Frame Server restart).
    if (existed) {
        if (Map(mapping_, false)) return true;
        last_error_ = 0;
    }
    if (!Map(mapping_, true)) {
        Close();
        return false;
    }
    return true;
}

bool Objects::Open() {
    Close();
    last_error_ = 0;
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE | READ_CONTROL, FALSE, Name(kFramesBase).c_str());
    if (!mapping_) {
        last_error_ = GetLastError();
        return false;
    }
    frame_ready_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE | READ_CONTROL, FALSE, Name(kFrameReadyBase).c_str());
    control_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE | READ_CONTROL, FALSE, Name(kControlBase).c_str());
    const bool trusted = frame_ready_ && control_ && TrustedObject(mapping_, true) &&
                         TrustedObject(frame_ready_, false) && TrustedObject(control_, false);
    if (!trusted || !Map(mapping_, false)) {
        if (!last_error_) last_error_ = GetLastError();
        Close();
        return false;
    }
    return true;
}

void Objects::Close() {
    if (header_) UnmapViewOfFile(header_);
    header_ = nullptr;
    for (HANDLE* h : {&mapping_, &frame_ready_, &control_}) {
        if (*h) CloseHandle(*h);
        *h = nullptr;
    }
}

uint8_t* Objects::SlotData(uint32_t i) const {
    return reinterpret_cast<uint8_t*>(header_) + SlotOffset(i);
}

bool WriteFrame(Objects& o, uint64_t frame_number, uint32_t width, uint32_t height, const uint8_t* y,
                uint32_t y_pitch, const uint8_t* uv, uint32_t uv_pitch, bool live, uint64_t capture_qpc_100ns) {
    Header* h = o.header();
    if (!h || !y || !uv || y_pitch < width || uv_pitch < width || width == 0 || height == 0 ||
        width > kMaxWidth || height > kMaxHeight || (width & 1) || (height & 1))
        return false;
    uint32_t idx = static_cast<uint32_t>(frame_number % kSlots);
    Slot& s = h->slots[idx];
    // A writer that died mid-write left the sequence odd; make it even again first so this write
    // still goes odd (writing) -> even (complete).
    if (s.seq & 1) InterlockedIncrement64(&s.seq);
    InterlockedIncrement64(&s.seq);  // odd: writing
    uint8_t* dst = o.SlotData(idx);
    for (uint32_t r = 0; r < height; ++r) std::memcpy(dst + size_t(r) * width, y + size_t(r) * y_pitch, width);
    uint8_t* dst_uv = dst + size_t(width) * height;
    for (uint32_t r = 0; r < height / 2; ++r)
        std::memcpy(dst_uv + size_t(r) * width, uv + size_t(r) * uv_pitch, width);
    s.frame_number = frame_number;
    s.width = width;
    s.height = height;
    s.live = live ? 1 : 0;
    s.capture_qpc_100ns = capture_qpc_100ns;
    s.write_qpc_100ns = QpcNow100ns();
    InterlockedIncrement64(&s.seq);  // even: complete (full barrier)
    InterlockedExchange64(&h->latest_frame, static_cast<LONG64>(frame_number));
    if (o.frame_ready()) SetEvent(o.frame_ready());
    return true;
}

bool ReadLatest(const Objects& o, uint64_t last_frame, uint32_t width, uint32_t height, uint8_t* y,
                uint32_t y_pitch, uint8_t* uv, uint32_t uv_pitch, ReadResult& out) {
    const Header* h = o.header();
    if (!h || !y || !uv || width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight ||
        (width & 1) || (height & 1) || y_pitch < width || uv_pitch < width) return false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint64_t n = static_cast<uint64_t>(InterlockedCompareExchange64(const_cast<LONG64*>(&h->latest_frame), 0, 0));
        if (n == 0 || n == last_frame) return false;
        uint32_t idx = static_cast<uint32_t>(n % kSlots);
        const Slot& s = h->slots[idx];
        LONG64 s1 = InterlockedCompareExchange64(const_cast<LONG64*>(&s.seq), 0, 0);
        if (s1 & 1) continue;
        if (s.frame_number != n || s.width != width || s.height != height) return false;
        const uint8_t* src = o.SlotData(idx);
        for (uint32_t r = 0; r < height; ++r) std::memcpy(y + size_t(r) * y_pitch, src + size_t(r) * width, width);
        const uint8_t* src_uv = src + size_t(width) * height;
        for (uint32_t r = 0; r < height / 2; ++r)
            std::memcpy(uv + size_t(r) * uv_pitch, src_uv + size_t(r) * width, width);
        out.frame_number = n;
        out.live = s.live != 0;
        out.capture_qpc_100ns = s.capture_qpc_100ns;
        out.write_qpc_100ns = s.write_qpc_100ns;
        MemoryBarrier();
        LONG64 s2 = InterlockedCompareExchange64(const_cast<LONG64*>(&s.seq), 0, 0);
        if (s1 == s2) return true;  // not torn
    }
    return false;
}

}  // namespace icam::shm

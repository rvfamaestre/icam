#pragma once

// H.264 Annex-B helpers (start-code framed NAL units).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace icam::annexb {

// Calls fn(nal_ptr, nal_len) for each NAL unit (without start code) in an Annex-B buffer.
template <typename Fn>
void ForEachNal(const uint8_t* p, size_t n, Fn&& fn) {
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1))) {
            size_t sc = p[i + 2] == 1 ? 3 : 4;
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && p[end - 1] == 0) --end;  // trailing zero bytes belong to the next start code
                if (end > start) fn(p + start, end - start);
            }
            i += sc;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) fn(p + start, n - start);
}

inline int NalType(const uint8_t* nal) { return nal[0] & 0x1F; }

inline bool ContainsIdr(const uint8_t* p, size_t n) {
    bool idr = false;
    ForEachNal(p, n, [&](const uint8_t* nal, size_t len) {
        if (len > 0 && NalType(nal) == 5) idr = true;
    });
    return idr;
}

inline bool ContainsParameterSets(const uint8_t* p, size_t n) {
    bool sps = false;
    ForEachNal(p, n, [&](const uint8_t* nal, size_t len) {
        if (len > 0 && NalType(nal) == 7) sps = true;
    });
    return sps;
}

inline void AppendNal(std::vector<uint8_t>& out, const uint8_t* nal, size_t len) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), nal, nal + len);
}

// Converts AVCC (4-byte big-endian length prefixes) to Annex-B. Returns false on bad lengths.
inline bool AvccToAnnexB(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i + 4 <= n) {
        uint32_t len = (uint32_t(p[i]) << 24) | (uint32_t(p[i + 1]) << 16) | (uint32_t(p[i + 2]) << 8) | p[i + 3];
        i += 4;
        if (len == 0 || len > n - i) return false;
        AppendNal(out, p + i, len);
        i += len;
    }
    return i == n;
}

}  // namespace icam::annexb

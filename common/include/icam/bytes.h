#pragma once

// Little-endian byte writer/reader with bounds checking. Every protocol parser goes through
// ByteReader so a truncated or hostile payload can only make a read fail, never overrun.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace icam {

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void i32(int32_t v) { put(&v, 4); }
    void bytes(const void* p, size_t n) { put(p, n); }
    // Short string: u16 length + UTF-8 bytes (truncated to 65535).
    void str(std::string_view s) {
        size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        u16(static_cast<uint16_t>(n));
        put(s.data(), n);
    }
    // Short blob: u16 length + bytes.
    void blob16(const std::vector<uint8_t>& b) {
        size_t n = b.size() > 0xFFFF ? 0xFFFF : b.size();
        u16(static_cast<uint16_t>(n));
        put(b.data(), n);
    }
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t>& data() { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    void put(const void* p, size_t n) {
        // Both targets (x64, arm64) are little-endian; the wire format is little-endian.
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    bool u8(uint8_t& v) { return get(&v, 1); }
    bool u16(uint16_t& v) { return get(&v, 2); }
    bool u32(uint32_t& v) { return get(&v, 4); }
    bool u64(uint64_t& v) { return get(&v, 8); }
    bool i32(int32_t& v) { return get(&v, 4); }
    bool str(std::string& s) {
        uint16_t n = 0;
        if (!u16(n) || remaining() < n) return fail();
        s.assign(reinterpret_cast<const char*>(p_ + off_), n);
        off_ += n;
        return true;
    }
    bool blob16(std::vector<uint8_t>& b) {
        uint16_t n = 0;
        if (!u16(n) || remaining() < n) return fail();
        b.assign(p_ + off_, p_ + off_ + n);
        off_ += n;
        return true;
    }
    bool skip(size_t n) {
        if (remaining() < n) return fail();
        off_ += n;
        return true;
    }
    size_t remaining() const { return ok_ ? n_ - off_ : 0; }
    const uint8_t* cursor() const { return p_ + off_; }
    bool ok() const { return ok_; }

private:
    bool get(void* dst, size_t n) {
        if (!ok_ || n_ - off_ < n) return fail();
        std::memcpy(dst, p_ + off_, n);
        off_ += n;
        return true;
    }
    bool fail() {
        ok_ = false;
        return false;
    }
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

}  // namespace icam

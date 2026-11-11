#pragma once

#include <cstddef>
#include <cstdint>

namespace icam::video {

// The offset is relative to the start returned by Lock2DSize, not necessarily scanline zero.
inline bool RowsFit(size_t length, size_t offset, uint32_t pitch, uint32_t row_bytes, uint32_t rows) {
    if (!rows || !row_bytes || pitch < row_bytes || offset > length) return false;
    const uint64_t bytes = uint64_t(rows - 1) * pitch + row_bytes;
    return bytes <= length - offset;
}

}  // namespace icam::video

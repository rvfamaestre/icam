#pragma once

// NV12 helpers: buffers, scaling with letterbox, the status card, and the "stamp" barcode that
// fakephone embeds so tests can count unique frames and measure latency end to end.

#include <cstdint>
#include <string>
#include <vector>

namespace icam::image {

struct Nv12 {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> data;  // packed: Y (w*h) then interleaved UV (w*h/2)

    void Allocate(uint32_t w, uint32_t h) {
        width = w;
        height = h;
        data.assign(size_t(w) * h * 3 / 2, 0);
    }
    uint8_t* y() { return data.data(); }
    uint8_t* uv() { return data.data() + size_t(width) * height; }
    const uint8_t* y() const { return data.data(); }
    const uint8_t* uv() const { return data.data() + size_t(width) * height; }
    bool empty() const { return data.empty(); }
};

void Fill(Nv12& f, uint8_t y, uint8_t u, uint8_t v);

// Scales src into dst (bilinear), preserving aspect ratio with black bars when they differ.
void ScaleLetterbox(const uint8_t* sy, uint32_t sy_pitch, const uint8_t* suv, uint32_t suv_pitch, uint32_t sw,
                    uint32_t sh, uint8_t* dy, uint32_t dy_pitch, uint8_t* duv, uint32_t duv_pitch, uint32_t dw,
                    uint32_t dh);

// Copies a frame with pitches into a packed Nv12 of the same size.
void CopyPacked(const uint8_t* sy, uint32_t sy_pitch, const uint8_t* suv, uint32_t suv_pitch, Nv12& dst);

// --- stamp: 64 bits drawn as black/white blocks across the top-left of the frame ---
// Layout: [0xA5 sync:8][frame counter:24][host QPC milliseconds, low 32 bits:32].
struct Stamp {
    uint32_t counter = 0;  // 24 bits used
    uint32_t qpc_ms = 0;
};
void DrawStamp(uint8_t* y, uint32_t pitch, uint32_t width, uint32_t height, const Stamp& s);
bool ReadStamp(const uint8_t* y, uint32_t pitch, uint32_t width, uint32_t height, Stamp& s);

// Moving test pattern (colour bars + sweeping bar) for fakephone and the vcam self-test.
void DrawTestPattern(Nv12& f, uint32_t frame_index);

// Status card: dark background, a title line and a smaller detail line, rendered with GDI and
// cached per size+text. Works in session 0 (memory DC only). `mirror_safe` adds a reflected copy of
// the message below the normal one, so it is readable in both self-view and the remote video.
void RenderCard(Nv12& out, uint32_t width, uint32_t height, const std::wstring& title, const std::wstring& detail,
                bool mirror_safe = false);

}  // namespace icam::image

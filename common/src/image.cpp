#include "icam/image.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>

namespace icam::image {

void Fill(Nv12& f, uint8_t y, uint8_t u, uint8_t v) {
    size_t ys = size_t(f.width) * f.height;
    std::memset(f.data.data(), y, ys);
    uint8_t* p = f.data.data() + ys;
    for (size_t i = 0; i < ys / 2; i += 2) {
        p[i] = u;
        p[i + 1] = v;
    }
}

void CopyPacked(const uint8_t* sy, uint32_t sy_pitch, const uint8_t* suv, uint32_t suv_pitch, Nv12& dst) {
    for (uint32_t r = 0; r < dst.height; ++r)
        std::memcpy(dst.y() + size_t(r) * dst.width, sy + size_t(r) * sy_pitch, dst.width);
    for (uint32_t r = 0; r < dst.height / 2; ++r)
        std::memcpy(dst.uv() + size_t(r) * dst.width, suv + size_t(r) * suv_pitch, dst.width);
}

namespace {

// Bilinear resample of one plane with `ch` interleaved channels (1 for Y, 2 for UV), 16.16 fixed point.
void ResamplePlane(const uint8_t* src, uint32_t spitch, uint32_t sw, uint32_t sh, uint8_t* dst, uint32_t dpitch,
                   uint32_t dw, uint32_t dh, int ch) {
    if (dw == 0 || dh == 0 || sw == 0 || sh == 0) return;
    const uint32_t xstep = (uint32_t)(((uint64_t)sw << 16) / dw);
    const uint32_t ystep = (uint32_t)(((uint64_t)sh << 16) / dh);
    std::vector<uint32_t> x0(dw), fx(dw);
    for (uint32_t x = 0; x < dw; ++x) {
        int64_t sx = (int64_t)x * xstep + (xstep >> 1) - 0x8000;
        if (sx < 0) sx = 0;
        uint32_t ix = (uint32_t)(sx >> 16);
        if (ix >= sw - 1) {
            x0[x] = sw - 1;
            fx[x] = 0;
        } else {
            x0[x] = ix;
            fx[x] = (uint32_t)(sx & 0xFFFF) >> 8;  // 0..255
        }
    }
    for (uint32_t y = 0; y < dh; ++y) {
        int64_t sy = (int64_t)y * ystep + (ystep >> 1) - 0x8000;
        if (sy < 0) sy = 0;
        uint32_t iy = (uint32_t)(sy >> 16);
        uint32_t fy = (uint32_t)(sy & 0xFFFF) >> 8;
        if (iy >= sh - 1) {
            iy = sh - 1;
            fy = 0;
        }
        const uint8_t* r0 = src + size_t(iy) * spitch;
        const uint8_t* r1 = (iy + 1 < sh) ? r0 + spitch : r0;
        uint8_t* out = dst + size_t(y) * dpitch;
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t a = x0[x] * ch, b = (x0[x] + (x0[x] + 1 < sw ? 1 : 0)) * ch, f = fx[x];
            for (int c = 0; c < ch; ++c) {
                uint32_t top = r0[a + c] * (256 - f) + r0[b + c] * f;
                uint32_t bot = r1[a + c] * (256 - f) + r1[b + c] * f;
                out[x * ch + c] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
            }
        }
    }
}

}  // namespace

void ScaleLetterbox(const uint8_t* sy, uint32_t sy_pitch, const uint8_t* suv, uint32_t suv_pitch, uint32_t sw,
                    uint32_t sh, uint8_t* dy, uint32_t dy_pitch, uint8_t* duv, uint32_t duv_pitch, uint32_t dw,
                    uint32_t dh) {
    if (sw == dw && sh == dh) {
        for (uint32_t r = 0; r < dh; ++r) std::memcpy(dy + size_t(r) * dy_pitch, sy + size_t(r) * sy_pitch, dw);
        for (uint32_t r = 0; r < dh / 2; ++r) std::memcpy(duv + size_t(r) * duv_pitch, suv + size_t(r) * suv_pitch, dw);
        return;
    }
    // Fit rectangle, even-aligned.
    uint32_t rw = dw, rh = dh;
    if ((uint64_t)sw * dh > (uint64_t)sh * dw) {
        rh = (uint32_t)((uint64_t)dw * sh / sw);
    } else {
        rw = (uint32_t)((uint64_t)dh * sw / sh);
    }
    rw &= ~1u;
    rh &= ~1u;
    if (rw == 0 || rh == 0) return;
    uint32_t x0 = ((dw - rw) / 2) & ~1u, y0 = ((dh - rh) / 2) & ~1u;
    if (rw != dw || rh != dh) {
        for (uint32_t r = 0; r < dh; ++r) std::memset(dy + size_t(r) * dy_pitch, 16, dw);
        for (uint32_t r = 0; r < dh / 2; ++r) std::memset(duv + size_t(r) * duv_pitch, 128, dw);
    }
    ResamplePlane(sy, sy_pitch, sw, sh, dy + size_t(y0) * dy_pitch + x0, dy_pitch, rw, rh, 1);
    ResamplePlane(suv, suv_pitch, sw / 2, sh / 2, duv + size_t(y0 / 2) * duv_pitch + x0, duv_pitch, rw / 2, rh / 2, 2);
}

// --- stamp ---

namespace {
constexpr int kStampBits = 64;
constexpr uint8_t kSync = 0xA5;
uint32_t BlockSize(uint32_t width) { return std::max<uint32_t>(8, width / 80); }
}  // namespace

void DrawStamp(uint8_t* y, uint32_t pitch, uint32_t width, uint32_t height, const Stamp& s) {
    uint32_t b = BlockSize(width);
    if (b * kStampBits > width || b > height) return;
    uint64_t v = (uint64_t(kSync) << 56) | (uint64_t(s.counter & 0xFFFFFF) << 32) | s.qpc_ms;
    for (int i = 0; i < kStampBits; ++i) {
        uint8_t lum = ((v >> (63 - i)) & 1) ? 235 : 16;
        for (uint32_t r = 0; r < b; ++r) std::memset(y + size_t(r) * pitch + size_t(i) * b, lum, b);
    }
}

bool ReadStamp(const uint8_t* y, uint32_t pitch, uint32_t width, uint32_t height, Stamp& s) {
    uint32_t b = BlockSize(width);
    if (b * kStampBits > width || b > height) return false;
    uint64_t v = 0;
    for (int i = 0; i < kStampBits; ++i) {
        // Average the centre quarter of the block.
        uint32_t cx = i * b + b / 4, cy = b / 4, n = 0, sum = 0;
        for (uint32_t r = cy; r < cy + b / 2; ++r)
            for (uint32_t c = cx; c < cx + b / 2; ++c, ++n) sum += y[size_t(r) * pitch + c];
        v = (v << 1) | ((sum / std::max<uint32_t>(n, 1)) > 125 ? 1 : 0);
    }
    if ((v >> 56) != kSync) return false;
    s.counter = (uint32_t)((v >> 32) & 0xFFFFFF);
    s.qpc_ms = (uint32_t)(v & 0xFFFFFFFF);
    return true;
}

void DrawTestPattern(Nv12& f, uint32_t frame_index) {
    // Eight vertical colour bars (BT.709 limited range), plus a white bar sweeping left to right.
    static const uint8_t bars[8][3] = {{180, 128, 128}, {168, 44, 136}, {145, 147, 44}, {133, 63, 52},
                                       {63, 193, 204},  {51, 109, 212}, {28, 212, 120}, {16, 128, 128}};
    const uint32_t w = f.width, h = f.height;
    // Every row is identical before adding the moving marker. Build each plane's first row once,
    // then copy it; per-pixel division across a 4K frame makes the test generator itself CPU-bound.
    for (uint32_t bar = 0; bar < 8; ++bar) {
        const uint32_t begin = (bar * w + 7) / 8, end = ((bar + 1) * w + 7) / 8;
        std::memset(f.y() + begin, bars[bar][0], end - begin);
    }
    for (uint32_t r = 1; r < h; ++r) std::memcpy(f.y() + size_t(r) * w, f.y(), w);
    for (uint32_t c = 0; c < w; c += 2) {
        const uint8_t* b = bars[(c * 8) / w];
        f.uv()[c] = b[1];
        f.uv()[c + 1] = b[2];
    }
    for (uint32_t r = 1; r < h / 2; ++r) std::memcpy(f.uv() + size_t(r) * w, f.uv(), w);
    uint32_t bw = std::max<uint32_t>(4, w / 40) & ~1u;
    uint32_t x = (frame_index * (w / 120 + 1)) % (w - bw);
    x &= ~1u;
    for (uint32_t r = h / 4; r < h * 3 / 4; ++r) std::memset(f.y() + size_t(r) * w + x, 235, bw);
    for (uint32_t r = h / 8; r < h * 3 / 8; ++r) {
        uint8_t* row = f.uv() + size_t(r) * w + x;
        for (uint32_t c = 0; c < bw; c += 2) {
            row[c] = 128;
            row[c + 1] = 128;
        }
    }
}

// --- status card ---

namespace {

struct CardKey {
    uint32_t w, h;
    std::wstring title, detail;
    bool mirror_safe;
    bool operator==(const CardKey& o) const {
        return w == o.w && h == o.h && title == o.title && detail == o.detail && mirror_safe == o.mirror_safe;
    }
};

std::mutex g_card_mu;
std::list<std::pair<CardKey, Nv12>> g_card_cache;

void RenderCardUncached(Nv12& out, uint32_t w, uint32_t h, const std::wstring& title, const std::wstring& detail,
                        bool mirror_safe) {
    out.Allocate(w, h);
    Fill(out, 16, 128, 128);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT all{0, 0, (LONG)w, (LONG)h};
    HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    // Camera glyph: a rounded rectangle and a lens circle above the text.
    int unit = (int)h / 40;
    HPEN pen = CreatePen(PS_SOLID, std::max(2, unit / 3), RGB(160, 165, 175));
    HGDIOBJ oldpen = SelectObject(dc, pen);
    HGDIOBJ oldbrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int cx = (int)w / 2, cy = (int)h * (mirror_safe ? 18 : 30) / 100;
    RoundRect(dc, cx - unit * 4, cy - unit * 3, cx + unit * 4, cy + unit * 3, unit * 2, unit * 2);
    Ellipse(dc, cx - unit * 2, cy - unit * 2, cx + unit * 2, cy + unit * 2);
    SelectObject(dc, oldbrush);
    SelectObject(dc, oldpen);
    DeleteObject(pen);

    HFONT tf = CreateFontW(-(int)h / (mirror_safe ? 22 : 16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT df = CreateFontW(-(int)h / (mirror_safe ? 34 : 30), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldfont = SelectObject(dc, tf);
    SetTextColor(dc, RGB(240, 240, 245));
    RECT tr{(LONG)w / 20, (LONG)h * 45 / 100, (LONG)w * 19 / 20, (LONG)h * 60 / 100};
    if (mirror_safe) { tr.top = (LONG)h * 34 / 100; tr.bottom = (LONG)h * 44 / 100; }
    DrawTextW(dc, title.c_str(), (int)title.size(), &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, df);
    SetTextColor(dc, RGB(175, 180, 190));
    RECT dr{(LONG)w / 12, (LONG)h * 62 / 100, (LONG)w * 11 / 12, (LONG)h * 85 / 100};
    if (mirror_safe) { dr.top = (LONG)h * 46 / 100; dr.bottom = (LONG)h * 61 / 100; }
    DrawTextW(dc, detail.c_str(), (int)detail.size(), &dr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    SelectObject(dc, oldfont);
    DeleteObject(tf);
    DeleteObject(df);
    GdiFlush();

    // BGRA -> NV12 (BT.709 limited range). Grey palette, so chroma stays neutral except the bars.
    const uint8_t* px = static_cast<const uint8_t*>(bits);
    const uint32_t copy_top = h * 67 / 100, copy_height = h * 61 / 100 - h * 34 / 100;
    for (uint32_t r = 0; r < h; ++r) {
        const bool reflected = mirror_safe && r >= copy_top && r < copy_top + copy_height;
        const uint32_t source_row = reflected ? r - copy_top + h * 34 / 100 : r;
        const uint8_t* src = px + size_t(source_row) * w * 4;
        uint8_t* dy = out.y() + size_t(r) * w;
        for (uint32_t c = 0; c < w; ++c) {
            uint32_t b = src[c * 4], g = src[c * 4 + 1], rr = src[c * 4 + 2];
            dy[reflected ? w - 1 - c : c] = (uint8_t)(16 + ((47 * rr + 157 * g + 16 * b) >> 8));
        }
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

}  // namespace

void RenderCard(Nv12& out, uint32_t width, uint32_t height, const std::wstring& title, const std::wstring& detail,
                bool mirror_safe) {
    CardKey key{width, height, title, detail, mirror_safe};
    std::lock_guard<std::mutex> lock(g_card_mu);
    for (auto it = g_card_cache.begin(); it != g_card_cache.end(); ++it) {
        if (it->first == key) {
            out = it->second;
            g_card_cache.splice(g_card_cache.begin(), g_card_cache, it);
            return;
        }
    }
    Nv12 card;
    RenderCardUncached(card, width, height, title, detail, mirror_safe);
    out = card;
    g_card_cache.emplace_front(key, std::move(card));
    while (g_card_cache.size() > 4) g_card_cache.pop_back();
}

}  // namespace icam::image

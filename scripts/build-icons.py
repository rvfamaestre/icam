"""Render iCam's small vector mark to opaque iPhone icons (Pillow, development only).

Run: python scripts/build-icons.py
The generated files are committed, so building/installing the app does not need Pillow.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "ios" / "Assets"
SIZE = 1024
SCALE = 3

# Vector and raster versions of the same few shapes. Edit their geometry here and regenerate both.
SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <title>iCam: camera over USB</title>
  <rect width="1024" height="1024" fill="#111c2a"/>
  <rect x="330" y="218" width="202" height="112" rx="32" fill="#f4f7fb"/>
  <rect x="180" y="286" width="664" height="442" rx="102" fill="#f4f7fb"/>
  <circle cx="512" cy="507" r="166" fill="#111c2a"/>
  <circle cx="512" cy="507" r="131" fill="#38dfcf"/>
  <circle cx="512" cy="507" r="96" fill="#183447"/>
  <circle cx="547" cy="470" r="16" fill="#bafff6"/>
  <circle cx="743" cy="373" r="19" fill="#38dfcf"/>
  <path d="M512 728V820" stroke="#f4f7fb" stroke-width="36" stroke-linecap="round"/>
  <rect x="459" y="803" width="106" height="70" rx="23" fill="#38dfcf"/>
  <rect x="477" y="829" width="70" height="18" rx="9" fill="#111c2a"/>
</svg>
'''


def render() -> Image.Image:
    image = Image.new("RGB", (SIZE * SCALE, SIZE * SCALE), "#111c2a")
    draw = ImageDraw.Draw(image)

    def box(values):
        return tuple(v * SCALE for v in values)

    def rect(bounds, radius, fill):
        draw.rounded_rectangle(box(bounds), radius=radius * SCALE, fill=fill)

    def circle(x, y, radius, fill):
        draw.ellipse(box((x - radius, y - radius, x + radius, y + radius)), fill=fill)

    rect((330, 218, 532, 330), 32, "#f4f7fb")
    rect((180, 286, 844, 728), 102, "#f4f7fb")
    circle(512, 507, 166, "#111c2a")
    circle(512, 507, 131, "#38dfcf")
    circle(512, 507, 96, "#183447")
    circle(547, 470, 16, "#bafff6")
    circle(743, 373, 19, "#38dfcf")
    draw.line(box((512, 728, 512, 820)), fill="#f4f7fb", width=36 * SCALE)
    rect((459, 803, 565, 873), 23, "#38dfcf")
    rect((477, 829, 547, 847), 9, "#111c2a")
    return image


if __name__ == "__main__":
    ASSETS.mkdir(parents=True, exist_ok=True)
    (ASSETS / "icam-mark.svg").write_text(SVG, encoding="utf-8")
    master = render()
    for name, size in [("AppIcon.png", 1024), ("AppIcon60x60@2x.png", 120), ("AppIcon60x60@3x.png", 180)]:
        path = ASSETS / name
        master.resize((size, size), Image.Resampling.LANCZOS).save(path, optimize=True)
        print(f"{path.relative_to(ROOT)}: {size}x{size}, opaque RGB")

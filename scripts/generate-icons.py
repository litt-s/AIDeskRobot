from __future__ import annotations

from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "icons"
WEB_PUBLIC = ROOT / "web" / "public"

# Project palette
BG_TOP = (19, 118, 143, 255)       # #13768f
BG_BOTTOM = (14, 80, 104, 255)     # #0e5068
FACE = (255, 255, 255, 255)
FACE_SOFT = (247, 250, 252, 255)   # #f7fafc
TEAL = (18, 107, 130, 255)         # #126b82
CORAL = (237, 120, 103, 255)       # #ed7867

def lerp(a: tuple[int,int,int,int], b: tuple[int,int,int,int], t: float) -> tuple[int,int,int,int]:
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(4))

def rounded_gradient_background(size: int, radius_scale: float = 0.219) -> Image.Image:
    """A full-bleed teal gradient with a rounded mask."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    for y in range(size):
        color = lerp(BG_TOP, BG_BOTTOM, y / max(1, size - 1))
        for x in range(size):
            px[x, y] = color
    mask = Image.new("L", (size, size), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle((0, 0, size - 1, size - 1), radius=round(size * radius_scale), fill=255)
    img.putalpha(mask)
    return img

def draw_robot_head(img: Image.Image, box: tuple[float,float,float,float]) -> None:
    """Draw the robot mark inside the given square box."""
    left, top, right, bottom = box
    w = right - left
    h = bottom - top

    # Head shell
    head = (
        left + w * 0.03,
        top + h * 0.22,
        right - w * 0.03,
        bottom - h * 0.08,
    )
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(head, radius=w * 0.16, fill=FACE)

    # Side “ears”
    ear_w = w * 0.055
    ear_h = h * 0.24
    ear_y = top + h * 0.45
    d.rounded_rectangle((left - ear_w * 0.55, ear_y, left + ear_w * 0.45, ear_y + ear_h), radius=ear_w * 0.5, fill=FACE_SOFT)
    d.rounded_rectangle((right - ear_w * 0.45, ear_y, right + ear_w * 0.55, ear_y + ear_h), radius=ear_w * 0.5, fill=FACE_SOFT)

    # Antenna stem and signal bulb
    stem_w = w * 0.052
    stem_h = h * 0.17
    cx = (left + right) / 2
    d.rounded_rectangle(
        (cx - stem_w / 2, top + h * 0.075, cx + stem_w / 2, top + h * 0.245),
        radius=stem_w / 2,
        fill=TEAL,
    )
    bulb_r = w * 0.068
    d.ellipse(
        (cx - bulb_r, top + h * 0.012 - bulb_r, cx + bulb_r, top + h * 0.012 + bulb_r),
        fill=CORAL,
    )

    # Eyes and speaker mouth
    eye_w = w * 0.088
    eye_h = h * 0.145
    eye_y = top + h * 0.42
    for ex in (left + w * 0.23, right - w * 0.23):
        d.rounded_rectangle(
            (ex - eye_w / 2, eye_y, ex + eye_w / 2, eye_y + eye_h),
            radius=eye_w / 2,
            fill=TEAL,
        )
    mouth_w = w * 0.22
    mouth_h = h * 0.045
    d.rounded_rectangle(
        (cx - mouth_w / 2, top + h * 0.685, cx + mouth_w / 2, top + h * 0.685 + mouth_h),
        radius=mouth_h / 2,
        fill=TEAL,
    )

def make_master(size: int = 1024) -> Image.Image:
    """Render at 4x for clean downsampling, then return a crisp master."""
    ss = 4096
    img = rounded_gradient_background(ss)
    draw_robot_head(img, (ss * 0.245, ss * 0.255, ss * 0.755, ss * 0.735))
    return img.resize((size, size), Image.Resampling.LANCZOS)

def make_adaptive_background(size: int) -> Image.Image:
    """Android background layer: full-bleed, no rounded corners."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    for y in range(size):
        color = lerp(BG_TOP, BG_BOTTOM, y / max(1, size - 1))
        for x in range(size):
            px[x, y] = color
    return img

def make_adaptive_foreground(size: int) -> Image.Image:
    """Android foreground layer: centered mark with adaptive-icon safe margins."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw_robot_head(img, (size * 0.30, size * 0.31, size * 0.70, size * 0.69))
    return img

def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "android").mkdir(parents=True, exist_ok=True)
    (OUT / "windows").mkdir(parents=True, exist_ok=True)
    (OUT / "macos").mkdir(parents=True, exist_ok=True)

    master = make_master(1024)
    master.save(OUT / "aidesk-icon.png")

    svg = '''<svg width="1024" height="1024" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#13768f"/>
      <stop offset="1" stop-color="#0e5068"/>
    </linearGradient>
  </defs>
  <rect width="1024" height="1024" rx="224" fill="url(#bg)"/>
  <circle cx="512" cy="262" r="36" fill="#ed7867"/>
  <rect x="486" y="278" width="52" height="70" rx="26" fill="#126b82"/>
  <rect x="272" y="399" width="480" height="265" rx="82" fill="#ffffff"/>
  <rect x="246" y="489" width="56" height="124" rx="28" fill="#f7fafc"/>
  <rect x="722" y="489" width="56" height="124" rx="28" fill="#f7fafc"/>
  <rect x="384" y="503" width="48" height="80" rx="24" fill="#126b82"/>
  <rect x="592" y="503" width="48" height="80" rx="24" fill="#126b82"/>
  <rect x="452" y="620" width="120" height="25" rx="12.5" fill="#126b82"/>
</svg>
'''
    (OUT / "aidesk-icon.svg").write_text(svg, encoding="utf-8")

    for size in (512, 256, 192, 180, 152, 144, 128, 96, 64, 48, 32, 16):
        master.resize((size, size), Image.Resampling.LANCZOS).save(OUT / f"aidesk-icon-{size}.png")

    ico_src = master.resize((256, 256), Image.Resampling.LANCZOS)
    ico_src.save(
        OUT / "windows" / "aidesk-icon.ico",
        sizes=[(16,16), (24,24), (32,32), (48,48), (64,64), (128,128), (256,256)],
    )

    for size in (108, 162, 216, 324, 432):
        make_adaptive_background(size).save(OUT / "android" / f"aidesk-adaptive-background-{size}.png")
        make_adaptive_foreground(size).save(OUT / "android" / f"aidesk-adaptive-foreground-{size}.png")

    master.save(OUT / "macos" / "aidesk-icon.icns", format="ICNS")

    # Web console assets (Vite serves files from web/public at the site root).
    WEB_PUBLIC.mkdir(parents=True, exist_ok=True)
    master.resize((180, 180), Image.Resampling.LANCZOS).save(WEB_PUBLIC / "apple-touch-icon.png")
    master.resize((192, 192), Image.Resampling.LANCZOS).save(WEB_PUBLIC / "icon-192.png")
    master.resize((512, 512), Image.Resampling.LANCZOS).save(WEB_PUBLIC / "icon-512.png")
    ico_src.save(WEB_PUBLIC / "favicon.ico", sizes=[(16,16), (32,32), (48,48)])
    (WEB_PUBLIC / "favicon.svg").write_text(svg, encoding="utf-8")

    print(f"icons written to {OUT}")

if __name__ == "__main__":
    main()

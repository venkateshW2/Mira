#!/usr/bin/env python3
"""Generate mira's app icon from the MIRA wordmark.

Kept as a script, not a checked-in binary alone, so the icon can be regenerated at any
size and the letterform decisions stay readable. Output lands in assets/icon/.

The wordmark is M-I-R-A where the I is a tall stem with a dot above it, rising past the
cap height and dropping below the baseline -- the one deliberate break in an otherwise
plain grotesque, and the thing that makes the mark read as a mark rather than as four
letters. A is set smaller and raised, sitting on the shoulder of the R.

Typeface is IBM Plex Sans SemiBold, the same family (and the heaviest weight) the app
itself embeds -- vendor/fonts/ibm-plex-sans. A stroke is added on top because the mark
wants a blacker weight than SemiBold and Plex Black is not vendored; stroking SemiBold is
closer to the drawing than dropping to a different family would be.
"""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "vendor/fonts/ibm-plex-sans/IBMPlexSans-SemiBold.ttf"
OUT = ROOT / "assets/icon"
S = 1024

# Proportions, all relative to S so any size renders identically.
BOX_INSET   = 0.085   # rounded-rect outline inset from the canvas edge
BOX_RADIUS  = 0.175   # corner radius
BOX_STROKE  = 0.042   # outline thickness
CAP         = 0.300   # nominal cap height before the fit below rescales it
A_SCALE     = 0.58    # A is smaller than M/R
A_RISE      = 0.30    # ...and raised by this fraction of its own height
STEM_W      = 0.215   # the I stem, as a fraction of cap height
STEM_RISE   = 0.230   # how far the stem climbs above cap height
STEM_DROP   = 0.200   # ...and drops below the baseline
DOT_R       = 0.115   # the dot above the stem
DOT_GAP     = 0.105   # gap between dot and stem
THICKEN     = 0.060   # stroke added to the glyphs, as a fraction of cap height
GAP         = 0.150   # space either side of the I stem
A_GAP       = 0.060   # space between R and A
FIT_W       = 0.86    # mark occupies this much of the plate's inner width...
FIT_H       = 0.78    # ...and at most this much of its inner height

INK = (17, 17, 17, 255)


def wordmark(cap_px: int) -> Image.Image:
    """The M-I-R-A mark alone, on transparency, cropped tight.

    Drawn oversized on a scratch canvas and then cropped to its own bounding box, so the
    caller can scale it to fit rather than trusting font metrics to land where predicted.
    Cap height, not font size, is the unit: font size and cap height differ per family,
    and every proportion below is stated against the height of an M.
    """
    pad = cap_px * 2
    w, h = cap_px * 12, cap_px * 5
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Find the font size whose cap height is cap_px, rather than assuming a ratio.
    size = cap_px * 2
    for _ in range(24):
        f = ImageFont.truetype(str(FONT), size)
        l, t, r, b = d.textbbox((0, 0), "M", font=f)
        if b - t == 0:
            break
        size = max(1, int(round(size * cap_px / (b - t))))
    big = ImageFont.truetype(str(FONT), size)
    small = ImageFont.truetype(str(FONT), max(1, int(round(size * A_SCALE))))
    thick = max(1, int(round(cap_px * THICKEN)))

    def box(txt, font):
        l, t, r, b = d.textbbox((0, 0), txt, font=font, stroke_width=thick)
        return r - l, b - t, l, t

    mw, mh, mlx, mty = box("M", big)
    rw, _, rlx, _ = box("R", big)
    aw, ah, alx, aty = box("A", small)

    stem_w = max(1, int(round(cap_px * STEM_W)))
    gap = int(round(cap_px * GAP))
    x, top = pad, pad

    d.text((x - mlx, top - mty), "M", font=big, fill=INK, stroke_width=thick, stroke_fill=INK)
    x += mw + gap

    # The I: a plain stem, taller than the caps at both ends, with a floating dot. The one
    # deliberate break in an otherwise plain grotesque.
    stem_top = top - int(round(cap_px * STEM_RISE))
    stem_bot = top + mh + int(round(cap_px * STEM_DROP))
    d.rectangle([x, stem_top, x + stem_w, stem_bot], fill=INK)
    dot_r = max(1, int(round(cap_px * DOT_R)))
    cx = x + stem_w / 2
    cy = stem_top - int(round(cap_px * DOT_GAP)) - dot_r
    d.ellipse([cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r], fill=INK)
    x += stem_w + gap

    d.text((x - rlx, top - mty), "R", font=big, fill=INK, stroke_width=thick, stroke_fill=INK)
    x += rw + int(round(cap_px * A_GAP))

    # A sits on the R's shoulder rather than on the baseline.
    a_top = top + (mh - ah) - ah * A_RISE
    d.text((x - alx, a_top - aty), "A", font=small, fill=INK, stroke_width=thick, stroke_fill=INK)

    return img.crop(img.getbbox())


def render(size: int) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    px = lambda f: int(round(f * size))

    # Plate: a white rounded rect with a heavy dark outline. Filled rather than
    # transparent so the mark holds its shape against both a light and a dark dock.
    inset, stroke = px(BOX_INSET), max(1, px(BOX_STROKE))
    d.rounded_rectangle([inset, inset, size - inset - 1, size - inset - 1],
                        radius=px(BOX_RADIUS), fill=(255, 255, 255, 255),
                        outline=INK, width=stroke)

    # Fit the mark inside the plate with real breathing room, constrained on BOTH axes so
    # it never crowds the outline or collides with the corner radius.
    inner = size - 2 * (inset + stroke)
    mark = wordmark(max(8, px(CAP)))
    scale = min(inner * FIT_W / mark.width, inner * FIT_H / mark.height)
    mark = mark.resize((max(1, int(mark.width * scale)), max(1, int(mark.height * scale))),
                        Image.LANCZOS)
    img.alpha_composite(mark, ((size - mark.width) // 2, (size - mark.height) // 2))
    return img


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for s in (1024, 512, 256, 128, 64, 32):
        render(s).save(OUT / f"mira-icon-{s}.png")
    print("wrote " + str(OUT) + "/mira-icon-{1024,512,256,128,64,32}.png")


if __name__ == "__main__":
    main()

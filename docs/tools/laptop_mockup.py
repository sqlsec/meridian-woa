# SPDX-License-Identifier: GPL-3.0-or-later
# Draws the README illustration: a thin 16:10 laptop in the style of the
# Yoga Slim 7x (no vendor logo) showing a real Obsidian frame on its panel.
# Usage: python laptop_mockup.py <screen.png> <out.png>
import sys
from PIL import Image, ImageDraw, ImageFilter

SS = 2                      # supersampling factor for smooth edges
W, H = 2400, 1520           # output size


def s(v):
    return int(round(v * SS))


def vgradient(size, top, bottom):
    w, h = size
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return g.resize((w, h))


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return m


def main(screen_path, out_path):
    cw, ch = s(W), s(H)
    canvas = vgradient((cw, ch), (24, 28, 38), (8, 9, 13))

    # soft radial glow behind the machine
    glow = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(glow).ellipse((s(300), s(120), s(2100), s(1300)), fill=70)
    glow = glow.filter(ImageFilter.GaussianBlur(s(220)))
    canvas = Image.composite(Image.new("RGB", (cw, ch), (52, 66, 92)), canvas, glow)

    # panel geometry (16:10 screen, thin bezels)
    scr_w, scr_h = 1600, 1000
    bez_side, bez_top, bez_bot = 20, 30, 34
    lid_w, lid_h = scr_w + 2 * bez_side, scr_h + bez_top + bez_bot
    lid_x, lid_y = (W - lid_w) // 2, 200
    base_y = lid_y + lid_h

    # floor shadow
    shadow = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(shadow).ellipse((s(lid_x - 60), s(base_y + 18), s(lid_x + lid_w + 60), s(base_y + 110)), fill=200)
    shadow = shadow.filter(ImageFilter.GaussianBlur(s(34)))
    canvas = Image.composite(Image.new("RGB", (cw, ch), (2, 2, 4)), canvas, shadow)

    # lid: dark "cosmic blue" aluminium with a lighter rim
    rim = vgradient((s(lid_w), s(lid_h)), (92, 102, 124), (54, 60, 76))
    canvas.paste(rim, (s(lid_x), s(lid_y)), rounded_mask((s(lid_w), s(lid_h)), s(26)))
    inner = vgradient((s(lid_w - 6), s(lid_h - 6)), (14, 15, 19), (10, 11, 14))
    canvas.paste(inner, (s(lid_x + 3), s(lid_y + 3)), rounded_mask((s(lid_w - 6), s(lid_h - 6)), s(23)))

    # screen content: the real 2944x1840 render, downsampled
    shot = Image.open(screen_path).convert("RGB").resize((s(scr_w), s(scr_h)), Image.Resampling.LANCZOS)
    sx, sy = lid_x + bez_side, lid_y + bez_top
    canvas.paste(shot, (s(sx), s(sy)), rounded_mask((s(scr_w), s(scr_h)), s(6)))

    # faint glass reflection
    refl = Image.new("L", (s(scr_w), s(scr_h)), 0)
    ImageDraw.Draw(refl).polygon([(0, 0), (s(scr_w * 0.42), 0), (s(scr_w * 0.18), s(scr_h)), (0, s(scr_h))], fill=7)
    refl = refl.filter(ImageFilter.GaussianBlur(s(40)))
    region = canvas.crop((s(sx), s(sy), s(sx + scr_w), s(sy + scr_h)))
    region = Image.composite(Image.new("RGB", region.size, (255, 255, 255)), region, refl)
    canvas.paste(region, (s(sx), s(sy)), rounded_mask((s(scr_w), s(scr_h)), s(6)))

    # webcam
    d = ImageDraw.Draw(canvas)
    cx, cy = W // 2, lid_y + bez_top // 2
    d.ellipse((s(cx - 5), s(cy - 5), s(cx + 5), s(cy + 5)), fill=(30, 33, 42))
    d.ellipse((s(cx - 2), s(cy - 2), s(cx + 2), s(cy + 2)), fill=(58, 70, 96))

    # hinge and base deck seen slightly from above
    hinge_h, deck_h = 10, 30
    d.rounded_rectangle((s(lid_x + 40), s(base_y - 2), s(lid_x + lid_w - 40), s(base_y + hinge_h)), s(4), fill=(36, 40, 52))
    top_l, top_r = lid_x - 30, lid_x + lid_w + 30
    bot_l, bot_r = lid_x - 110, lid_x + lid_w + 110
    y0, y1 = base_y + hinge_h - 2, base_y + hinge_h + deck_h
    deck = vgradient((s(bot_r - bot_l), s(y1 - y0)), (128, 138, 160), (70, 77, 96))
    mask = Image.new("L", deck.size, 0)
    ImageDraw.Draw(mask).polygon(
        [(s(top_l - bot_l), 0), (s(top_r - bot_l), 0), (s(bot_r - bot_l), s(y1 - y0) - s(8)),
         (s(bot_r - bot_l) - s(14), s(y1 - y0)), (s(14), s(y1 - y0)), (0, s(y1 - y0) - s(8))], fill=255)
    canvas.paste(deck, (s(bot_l), s(y0)), mask)
    # front lip highlight and thumb scoop
    d.line((s(bot_l + 14), s(y1 - 1), s(bot_r - 14), s(y1 - 1)), fill=(150, 160, 182), width=s(2))
    d.rounded_rectangle((s(W / 2 - 130), s(y0), s(W / 2 + 130), s(y0 + 12)), s(6), fill=(58, 64, 80))

    canvas.resize((W, H), Image.Resampling.LANCZOS).save(out_path, optimize=True)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

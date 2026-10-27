#!/usr/bin/env python3
"""Turns the ui_sim dumps (128x64, status line + 7 lines, 8 vertical pixels per
byte, bit 0 on top) into PNG files that look like the radio's LCD.

  render_lcd.py DIR [SCALE]     writes DIR/NN_name.png and DIR/overview.png
"""
import glob, os, sys

from PIL import Image, ImageDraw

BG, FG = (152, 186, 132), (24, 36, 30)


def load(path):
    raw = open(path, "rb").read()
    img = Image.new("RGB", (128, 64), BG)
    px = img.load()
    for page in range(8):
        for x in range(128):
            b = raw[page * 128 + x]
            for bit in range(8):
                if b >> bit & 1:
                    px[x, page * 8 + bit] = FG
    return img


def main():
    d = sys.argv[1]
    scale = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    shots = []
    for path in sorted(glob.glob(os.path.join(d, "*.lcd"))):
        img = load(path).resize((128 * scale, 64 * scale), Image.NEAREST)
        out = path[:-4] + ".png"
        img.save(out)
        shots.append((os.path.basename(path)[:-4], img))
    if not shots:
        return
    cols = 3
    w, h = shots[0][1].size
    pad, label = 16, 22
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad + label) + pad), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, (name, img) in enumerate(shots):
        x = pad + (i % cols) * (w + pad)
        y = pad + (i // cols) * (h + pad + label)
        draw.text((x, y), name, fill=(230, 230, 230))
        sheet.paste(img, (x, y + label))
    sheet.save(os.path.join(d, "overview.png"))
    print("wrote %d screens and overview.png" % len(shots))


if __name__ == "__main__":
    main()

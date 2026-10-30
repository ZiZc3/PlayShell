"""assets/ui/bezel_full.png (drawn around a 960 x 864 game, 60 px frame) -> one bezel per console screen size:
assets/ui/bezel_<w>x<h>.png, half size (shown 2x). The game is as big as fits 1600 x 864 (the module's place_game);
corners (rounded, shine) are kept, the straight edges are fitted to the window (9-slice)."""
import os
from PIL import Image

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(HERE, 'assets', 'ui')
SIZES = [(160, 144), (256, 240), (256, 224), (240, 160), (320, 240), (640, 480), (512, 448), (256, 384),
         (341, 240), (512, 240), (224, 144), (144, 224), (400, 480)]  # + PC Engine's other two widths, WonderSwan (both ways)  # GB/GBC/Game Gear, NES, SNES, GBA, PS1 (4:3; 2x)
FRAME, CORNER = 60, 140

src = Image.open(os.path.join(UI, 'bezel_full.png')).convert('RGBA')
SW, SH = src.size


def nine_slice(w, h):
    out = Image.new('RGBA', (w, h))
    c = CORNER
    xs = [(0, c, 0, c), (c, SW - c, c, w - c), (SW - c, SW, w - c, w)]
    ys = [(0, c, 0, c), (c, SH - c, c, h - c), (SH - c, SH, h - c, h)]
    for sx0, sx1, dx0, dx1 in xs:
        for sy0, sy1, dy0, dy1 in ys:
            if dx1 > dx0 and dy1 > dy0:
                out.paste(src.crop((sx0, sy0, sx1, sy1)).resize((dx1 - dx0, dy1 - dy0), Image.LANCZOS), (dx0, dy0))
    return out


for w, h in SIZES:
    k = min(1600 / w, 864 / h)  # the module's place_game: 1600 x 864 at most
    gw, gh = round(w * k), round(h * k)
    full = nine_slice(gw + 2 * FRAME, gh + 2 * FRAME)
    half = full.resize(((gw + 2 * FRAME) // 2, (gh + 2 * FRAME) // 2), Image.LANCZOS)
    half.save(os.path.join(UI, 'bezel_%dx%d.png' % (w, h)), optimize=True)
    print('bezel_%dx%d' % (w, h), half.size)

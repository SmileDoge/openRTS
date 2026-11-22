"""Makes resources/openrts.ico, the programs' icon, from resources/logo.png.

python resources/make_icon.py

The sizes Windows asks for at 100..200 % scaling (16 .. 256 px), each
downscaled from the logo; the corners outside its circle stay transparent.
"""
import os

from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
logo = Image.open(os.path.join(here, "logo.png")).convert("RGBA")
sizes = [(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)]
# BMP frames: every Windows icon loader reads them.
logo.save(os.path.join(here, "openrts.ico"), sizes=sizes, bitmap_format="bmp")
print("openrts.ico:", ", ".join(str(s[0]) for s in sizes))

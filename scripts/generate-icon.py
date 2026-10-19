"""Render the original SVG's simple vector shapes with Pillow; no network or font dependency.

Install Pillow, then run `python scripts/generate-icon.py`. The SVG is the source
of truth. This deliberately supports only its circle/rect/polygon vocabulary;
an unsupported SVG edit fails instead of silently rendering a different icon.
"""
from pathlib import Path
import argparse
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 24, 32, 48, 64, 128, 256)


def render(svg: Path, size: int) -> Image.Image:
    document = ET.parse(svg).getroot()
    if document.get("viewBox") != "0 0 256 256":
        raise ValueError("The renderer expects the original 256-square viewBox")
    scale = size * 8 / 256
    image = Image.new("RGBA", (size * 8, size * 8))
    draw = ImageDraw.Draw(image)

    def bounds(x, y, width, height, spread=0):
        return tuple(v * scale for v in (x - spread, y - spread, x + width + spread, y + height + spread))

    for element in document:
        shape = element.tag.rsplit("}", 1)[-1]
        fill, stroke = element.get("fill"), element.get("stroke")
        width = float(element.get("stroke-width", "0")) if stroke else 0
        if shape == "circle":
            cx, cy, radius = (float(element.get(k)) for k in ("cx", "cy", "r"))
            if stroke:
                draw.ellipse(bounds(cx - radius, cy - radius, 2 * radius, 2 * radius, width / 2), fill=stroke)
            draw.ellipse(bounds(cx - radius, cy - radius, 2 * radius, 2 * radius, -width / 2), fill=fill)
        elif shape == "rect":
            x, y, w, h = (float(element.get(k)) for k in ("x", "y", "width", "height"))
            radius = float(element.get("rx", "0"))
            if stroke:
                draw.rounded_rectangle(bounds(x, y, w, h, width / 2), radius=(radius + width / 2) * scale, fill=stroke)
            draw.rounded_rectangle(bounds(x, y, w, h, -width / 2), radius=max(0, radius - width / 2) * scale, fill=fill)
        elif shape == "polygon" and not stroke:
            points = [tuple(float(v) * scale for v in point.split(",")) for point in element.get("points", "").split()]
            draw.polygon(points, fill=fill)
        else:
            raise ValueError(f"Unsupported SVG element: {shape}")
        if any(key in element.attrib for key in ("transform", "style", "opacity", "filter")):
            raise ValueError("Transforms, styles, opacity, and filters are not supported")
    return image.resize((size, size), Image.Resampling.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "assets", help="Output asset directory")
    args = parser.parse_args()
    destination = args.output
    icons = destination / "icons"
    icons.mkdir(parents=True, exist_ok=True)
    svg = ROOT / "assets" / "obs-bluray.svg"
    for size in SIZES:
        render(svg, size).save(icons / f"obs-bluray-{size}.png", optimize=True)
    render(svg, 512).save(destination / "obs-bluray.png", optimize=True)
    render(svg, 256).save(destination / "obs-bluray.ico", sizes=[(size, size) for size in SIZES])
    print(f"Generated PNG preview, {len(SIZES)} PNG sizes, and multi-resolution ICO")


if __name__ == "__main__":
    main()

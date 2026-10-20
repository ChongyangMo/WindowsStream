"""生成两端共用风格的 SVG、PNG 和 Windows 多尺寸 ICO。"""
from pathlib import Path
from xml.sax.saxutils import escape

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent
SCALE = 4
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


class Icon:
    def __init__(self, title):
        self.image = Image.new("RGBA", (256 * SCALE, 256 * SCALE))
        self.draw = ImageDraw.Draw(self.image)
        self.svg = [
            '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">',
            f"<title>{escape(title)}</title>",
        ]

    def box(self, bounds, radius, fill, outline=None, width=0):
        self.draw.rounded_rectangle(
            tuple(value * SCALE for value in bounds), radius * SCALE,
            fill=fill, outline=outline, width=width * SCALE,
        )
        x, y, right, bottom = bounds
        # Pillow 描边在边界内，SVG 描边在路径两侧。
        inset = width / 2
        self.svg.append(
            f'<rect x="{x + inset}" y="{y + inset}" '
            f'width="{right - x - width}" height="{bottom - y - width}" '
            f'rx="{max(0, radius - inset)}" fill="{fill or "none"}" '
            f'stroke="{outline or "none"}" stroke-width="{width}"/>'
        )

    def line(self, points, color, width):
        scaled = [(x * SCALE, y * SCALE) for x, y in points]
        self.draw.line(scaled, fill=color, width=width * SCALE, joint="curve")
        radius = width * SCALE / 2
        for x, y in scaled:
            self.draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)
        coords = " ".join(f"{x},{y}" for x, y in points)
        self.svg.append(
            f'<polyline points="{coords}" fill="none" stroke="{color}" '
            f'stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>'
        )

    def polygon(self, points, color, outline, width):
        coords = [(x * SCALE, y * SCALE) for x, y in points]
        self.draw.polygon(coords, fill=color)
        # 外描边加背景色留白，确保指针与屏幕边框在小尺寸下分开。
        self.draw.line(coords + [coords[0]], fill=outline, width=width * SCALE, joint="curve")
        self.draw.polygon(coords, fill=color)
        points_text = " ".join(f"{x},{y}" for x, y in points)
        self.svg.append(
            f'<polygon points="{points_text}" fill="{color}" stroke="{outline}" '
            f'stroke-width="{width}" stroke-linejoin="round" paint-order="stroke fill"/>'
        )

    def save(self, name):
        self.svg.append("</svg>")
        (ROOT / f"{name}.svg").write_text("\n".join(self.svg) + "\n", encoding="utf-8")
        self.image.resize((512, 512), Image.Resampling.LANCZOS).save(ROOT / f"{name}.png")
        self.image.save(ROOT / f"{name}.ico", sizes=[(size, size) for size in SIZES])


def create(name, color, controller):
    icon = Icon("WindowsStream 控制端" if controller else "WindowsStream 被控端")
    icon.box((16, 16, 240, 240), 52, color)
    icon.box((52, 62, 204, 166), 14, None, "#FFFFFF", 11)
    icon.line([(128, 168), (128, 190)], "#FFFFFF", 10)
    icon.line([(101, 192), (155, 192)], "#FFFFFF", 10)
    if controller:
        icon.polygon(
            [(135, 111), (135, 177), (151, 162), (166, 193),
             (181, 185), (166, 155), (189, 151)],
            "#FFFFFF", color, 12,
        )
    else:
        icon.line([(89, 114), (164, 114)], "#FFFFFF", 11)
        icon.line([(142, 92), (164, 114), (142, 136)], "#FFFFFF", 11)
    icon.save(name)


if __name__ == "__main__":
    create("controller", "#2563EB", True)
    create("host", "#0D9488", False)
    print("控制端和被控端图标已生成")

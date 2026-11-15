"""生成 AiPic 的图标与安装包用 logo。

一次画成 1024×1024 的大图再缩小到各尺寸：小尺寸直接按坐标画会糊，
而多档重采样能保住圆角与线条。
"""
from PIL import Image, ImageDraw

S = 1024


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def vertical_gradient(box, top, bottom):
    """在 box 范围内铺一条竖向渐变"""
    x0, y0, x1, y1 = box
    strip = Image.new("RGBA", (1, max(1, y1 - y0 + 1)))
    px = strip.load()
    h = max(1, y1 - y0)
    for y in range(y1 - y0 + 1):
        px[0, y] = lerp(top, bottom, y / h)
    return strip.resize((x1 - x0 + 1, y1 - y0 + 1), Image.NEAREST)


def draw_icon(size):
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # 底板：竖向渐变的圆角方块，蓝色系与 AiUI 的 accent 一致
    plate = vertical_gradient((0, 0, S - 1, S - 1), (0x3B, 0x82, 0xF6), (0x1D, 0x4E, 0xD8))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    canvas.paste(plate, (0, 0), mask)

    # 相框：白色圆角矩形，里面是「天空 + 山 + 太阳」
    fx0, fy0, fx1, fy1 = int(S * 0.20), int(S * 0.26), S - int(S * 0.20), S - int(S * 0.26)
    sky = vertical_gradient((fx0, fy0, fx1, fy1), (0xEF, 0xF6, 0xFF), (0xB6, 0xD3, 0xFB))

    scene = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    scene.paste(sky, (fx0, fy0))
    sd = ImageDraw.Draw(scene)

    # 太阳
    sun_r = int(S * 0.056)
    sun_c = (int(S * 0.355), int(S * 0.395))
    sd.ellipse([sun_c[0] - sun_r, sun_c[1] - sun_r, sun_c[0] + sun_r, sun_c[1] + sun_r],
               fill=(0xFF, 0xC1, 0x4D, 255))

    # 两座山：后山浅、前山深。山脚落在相框底边，留一点空隙当"平地"
    base = fy1 - int(S * 0.045)
    back = [(int(S * 0.46), int(S * 0.44)), (int(S * 0.74), base), (int(S * 0.20), base)]
    front = [(int(S * 0.63), int(S * 0.52)), (int(S * 0.84), base), (int(S * 0.44), base)]
    sd.polygon(back, fill=(0x60, 0xA5, 0xFA, 255))
    sd.polygon(front, fill=(0x25, 0x63, 0xEB, 255))
    # 地平线：一整条浅色，山脚落上去才不像是被截断的三角形
    sd.rectangle([fx0, base, fx1, fy1], fill=(0xE3, 0xEE, 0xFD, 255))

    # 只保留相框内的部分：山是三角形，超出角上的圆角会露到白框外
    frame_mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(frame_mask).rounded_rectangle(
        [fx0, fy0, fx1, fy1], radius=int(S * 0.05), fill=255)
    canvas.paste(scene, (0, 0), frame_mask)

    return canvas.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    icons = [draw_icon(s) for s in sizes]
    icons[-1].save("src/res/aipic.ico", format="ICO", sizes=[(s, s) for s in sizes])
    # 安装器/关于页用的 PNG：取 256 那档，缩放显示时不糊
    icons[-1].save("assets/logo.png", format="PNG")
    print("图标已生成：src/res/aipic.ico, assets/logo.png")


if __name__ == "__main__":
    main()

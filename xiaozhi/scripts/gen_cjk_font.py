"""生成小智界面用的中文字库子集（给 LVGL tiny_ttf 用）。

用法：
    python scripts/gen_cjk_font.py
    python scripts/gen_cjk_font.py --src C:\\Windows\\Fonts\\simhei.ttf --out main/assets/xz_cjk.ttf

裁剪范围：ASCII + GB2312 全集（6763 汉字 + 682 符号/标点），
结果约 2.5 MB，随固件编进 ota_0（8MB 分区）。

为什么不用 LVGL 内置的 lv_font_source_han_sans_sc_16_cjk：
那个只带 1195 个汉字，服务端回复里的常见字会缺字形，屏幕上显示成方块。

依赖：pip install fonttools
"""

import argparse
import os
import sys

from fontTools import subset

DEFAULT_SRC = r"C:\Windows\Fonts\simhei.ttf"
# 字库不进 app 镜像，直接写进 selector 的镜像库源目录，
# 由 tools/pack_storage.ps1 打进 storage 分区（components/cjk_font 运行期读取）
DEFAULT_OUT = os.path.join("..", "storage_src", "font-cjk.ttf")


def gb2312_chars():
    """GB2312 全集：0xA1A1-0xA9FE 符号 + 0xB0A1-0xF7FE 汉字。"""
    chars = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def main():
    parser = argparse.ArgumentParser(description="生成中文子集字库")
    parser.add_argument("--src", default=DEFAULT_SRC, help="源 TTF（默认系统黑体）")
    parser.add_argument("--out", default=DEFAULT_OUT, help="输出 TTF 路径")
    parser.add_argument("--size", type=int, default=0, help="仅作记录，位图字号在固件里定")
    args = parser.parse_args()

    if not os.path.isfile(args.src):
        sys.exit(f"源字体不存在：{args.src}")

    chars = gb2312_chars()
    codepoints = set(range(0x20, 0x7F))          # ASCII
    codepoints |= {ord(c) for c in chars}
    # GB2312 之外但中文回复里常见的标点/符号
    codepoints |= {ord(c) for c in "“”‘’—…·、。！？；：（）《》【】〈〉「」『』％°±×÷№→←↑↓★☆※"}

    options = subset.Options()
    options.hinting = False                       # stb_truetype 不用 hint，去掉省空间
    options.drop_tables += ["DSIG", "FFTM", "VDMX"]
    options.layout_features = ["*"]               # 保留 kern，ASCII 排版好看些
    options.name_IDs = ["*"]
    options.notdef_outline = True

    font = subset.load_font(args.src, options)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    glyphs = len(font.getGlyphOrder()) if hasattr(font, "getGlyphOrder") else -1

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    subset.save_font(font, args.out, options)
    font.close()
    print(f"源字体   : {args.src} ({os.path.getsize(args.src) / 1024 / 1024:.2f} MB)")
    print(f"字符数   : {len(codepoints)}（其中 GB2312 汉字 {len(chars)}）")
    print(f"输出     : {args.out} ({os.path.getsize(args.out) / 1024 / 1024:.2f} MB, {glyphs} glyphs)")
    print(f"位图字号 : 在固件里用 lv_tiny_ttf_create_data_ex(..., {args.size or 'font_size'}) 指定")


if __name__ == "__main__":
    main()

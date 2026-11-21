#!/usr/bin/env python3
"""生成 music app 的测试素材（48 kHz WAV + selector 磁贴图标）。

    python tools/gen_test_assets.py            # WAV + 图标都生成
    python tools/gen_test_assets.py --wav-only
    python tools/gen_test_assets.py --icon-only

输出（两个都进镜像库，不打进 app 二进制）：
    ../storage_src/music-test48k.wav    48 kHz / 16 bit / 单声道，8 秒
    ../storage_src/icon-music.png       256x256 透明底白色音符线条图标

WAV 内容：C 大调五声音阶琶音（每音 0.4 s，循环两遍）+ C3 低音垫，
自带起音/衰减，听起来像拨弦，用来判断采样率、左右声道和音量是否正常。
"""

from __future__ import annotations

import argparse
import math
import struct
import wave
from pathlib import Path

STORAGE_DIR = Path(__file__).resolve().parents[2] / "storage_src"

SAMPLE_RATE = 48000
SECONDS = 8.0

# C 大调五声音阶，音名 -> 频率（Hz）
MELODY = [
    ("C5", 523.25),
    ("D5", 587.33),
    ("E5", 659.25),
    ("G5", 783.99),
    ("A5", 880.00),
    ("C6", 1046.50),
    ("A5", 880.00),
    ("G5", 783.99),
    ("E5", 659.25),
    ("D5", 587.33),
]
NOTE_SEC = 0.4
BASS = ("C3", 130.81)
BASS_SEC = 1.6
BASS_LEN = 0.8


def _melody_samples(total: int) -> list[float]:
    buf = [0.0] * total
    pattern_len = int(NOTE_SEC * SAMPLE_RATE * len(MELODY))

    for i, (_name, freq) in enumerate(MELODY):
        start = i * int(NOTE_SEC * SAMPLE_RATE)
        length = int(NOTE_SEC * SAMPLE_RATE)
        attack = int(0.008 * SAMPLE_RATE)
        release = int(0.06 * SAMPLE_RATE)
        for n in range(length):
            idx = (start + n) % total
            t = n / SAMPLE_RATE
            # 拨弦包络：快起音 + 指数衰减 + 尾部淡出
            env = math.exp(-t / 0.18)
            if n < attack:
                env *= n / attack
            if n > length - release:
                env *= max(0.0, (length - n) / release)
            phase = 2.0 * math.pi * freq * t
            # 基频 + 二次/三次谐波，听感更接近乐器而不是纯正弦
            value = (math.sin(phase) + 0.18 * math.sin(2 * phase) + 0.06 * math.sin(3 * phase))
            buf[idx] += 0.55 * env * value

    # 低音垫：每 1.6 s 一个 C3，长度 0.8 s
    bass_period = int(BASS_SEC * SAMPLE_RATE)
    bass_len = int(BASS_LEN * SAMPLE_RATE)
    for start in range(0, total, bass_period):
        for n in range(bass_len):
            idx = (start + n) % total
            t = n / SAMPLE_RATE
            env = math.exp(-t / 0.45)
            if n < 240:
                env *= n / 240
            if n > bass_len - 480:
                env *= max(0.0, (bass_len - n) / 480)
            phase = 2.0 * math.pi * BASS[1] * t
            buf[idx] += 0.22 * env * (math.sin(phase) + 0.1 * math.sin(2 * phase))

    # 整段首尾 10 ms 淡入淡出，循环播放时接缝不炸
    fade = int(0.01 * SAMPLE_RATE)
    for n in range(fade):
        factor = n / fade
        buf[n] *= factor
        buf[total - 1 - n] *= factor
    return buf


def gen_wav(out_path: Path) -> None:
    total = int(SECONDS * SAMPLE_RATE)
    samples = _melody_samples(total)

    peak = max(abs(v) for v in samples) or 1.0
    scale = 0.55 / peak
    frames = bytearray()
    for value in samples:
        v = int(max(-1.0, min(1.0, value * scale)) * 32767)
        frames += struct.pack("<h", v)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(out_path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(bytes(frames))

    print(f"WAV  : {out_path} ({out_path.stat().st_size:,} B, "
          f"{SAMPLE_RATE} Hz / 16 bit / mono / {SECONDS:g} s)")


def gen_icon(out_path: Path) -> None:
    from PIL import Image, ImageDraw

    size = 256
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)

    # 符头：椭圆单独画一层再旋转 -20°，比正椭圆更像音符
    head_w, head_h = 74, 54
    head = Image.new("RGBA", (head_w, head_h), (0, 0, 0, 0))
    ImageDraw.Draw(head).ellipse((0, 0, head_w - 1, head_h - 1), fill=white)
    head = head.rotate(-20, expand=True, resample=Image.BICUBIC)
    img.alpha_composite(head, (30, 168))
    img.alpha_composite(head, (130, 144))

    # 符干：从符头右缘往上
    draw.rounded_rectangle((96, 80, 110, 200), radius=7, fill=white)
    draw.rounded_rectangle((196, 60, 210, 176), radius=7, fill=white)

    # 连梁：连接两根符干顶部（略微上斜）
    draw.polygon([(96, 80), (196, 60), (196, 92), (96, 112)], fill=white)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path)
    print(f"ICON : {out_path} ({out_path.stat().st_size:,} B, {size}x{size} 透明底)")


def main() -> None:
    parser = argparse.ArgumentParser(description="生成 music app 的测试 WAV 与图标")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--wav-only", action="store_true")
    group.add_argument("--icon-only", action="store_true")
    parser.add_argument("--out-dir", type=Path, default=STORAGE_DIR)
    args = parser.parse_args()

    if not args.icon_only:
        gen_wav(args.out_dir / "music-test48k.wav")
    if not args.wav_only:
        gen_icon(args.out_dir / "icon-music.png")


if __name__ == "__main__":
    main()

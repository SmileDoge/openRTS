"""The thunder of openRTS's thunderstorms (docs/weather.md, "Thunder"):
claps cut from three Freesound recordings into packs/thunder (the
openrts-thunder pack). Not run by the build: the recordings are not kept in
the repository, the claps are.

    python make_thunder.py <folder with the recordings> <packs/thunder>

The recordings (see packs/thunder/CREDITS.txt):
  321654__inspectorj__thunder-very-close-no-rain-c.wav  (CC BY 4.0)
  360328__inspectorj__thunder-very-close-rain-a.wav     (CC BY 4.0)
  844420__tsp-talk__extreme-thunder-impact-...wav       (CC0)

Each clap: a stretch of a recording, both channels mixed to one, filtered
for its distance (the air takes the highs first: near as recorded, middle
from 1.2 kHz down, far from 350..450 Hz down, their sharp start softened),
faded in and out, its loudest second brought to -16 dBFS RMS (no peak above
-1 dBFS), 16-bit mono WAV (near 44.1 kHz, the rest 22.05 kHz).
Needs numpy and scipy.
"""

import math
import os
import struct
import sys

import numpy as np
from scipy import signal


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise RuntimeError("not a WAV: " + path)
    pos = 12
    fmt = None
    frames = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            frames = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, bits = fmt
    if tag == 1 and bits == 16:
        values = np.frombuffer(frames, dtype="<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 24:
        raw = np.frombuffer(frames, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = ((raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)) << 8 >> 8).astype(np.float64) / 8388608.0
    elif tag == 3 and bits == 32:
        values = np.frombuffer(frames, dtype="<f4").astype(np.float64)
    else:
        raise RuntimeError("unsupported WAV format %s: %s" % (fmt, path))
    values = values[: len(values) // channels * channels].reshape(-1, channels)
    return rate, values.mean(axis=1)


def write_wav(path, rate, mono):
    pcm = np.clip(np.round(mono * 32767.0), -32768, 32767).astype("<i2").tobytes()
    header = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(pcm))
    open(path, "wb").write(header + pcm)


def fade(samples, rate, start_s, end_s, rising):
    """A raised cosine over [start_s, end_s] of the clip: up (rising) or down."""
    a = int(start_s * rate)
    b = min(len(samples), int(end_s * rate))
    if b <= a:
        return samples
    t = np.linspace(0.0, 1.0, b - a)
    shape = 0.5 - 0.5 * np.cos(math.pi * t)
    out = samples.copy()
    if rising:
        out[:a] = 0.0
        out[a:b] *= shape
    else:
        out[a:b] *= 1.0 - shape
        out[b:] = 0.0
    return out


def make(source, rate, start, end, lowpass, fade_in, fade_out_from, fade_out_to, out_rate):
    clip = source[int(start * rate):int(end * rate)]
    if lowpass:
        sos = signal.butter(4, lowpass, btype="low", fs=rate, output="sos")
        # Forwards and back: no phase shift, the start not smeared later.
        clip = signal.sosfiltfilt(sos, clip)
    if out_rate != rate:
        g = math.gcd(rate, out_rate)
        clip = signal.resample_poly(clip, out_rate // g, rate // g)
    clip = fade(clip, out_rate, 0.0, fade_in, True)
    clip = fade(clip, out_rate, fade_out_from - start, fade_out_to - start, False)
    # The loudest second to -16 dBFS RMS, no peak above -1 dBFS.
    window = out_rate
    power = np.convolve(clip * clip, np.ones(window) / window, mode="valid") if len(clip) > window else np.array([np.mean(clip * clip)])
    loudest = math.sqrt(max(float(power.max()), 1e-12))
    gain = 10 ** (-16 / 20) / loudest
    peak = float(np.max(np.abs(clip))) * gain
    if peak > 10 ** (-1 / 20):
        gain *= 10 ** (-1 / 20) / peak
    return clip * gain


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 64
    src, out = sys.argv[1], sys.argv[2]
    names = os.listdir(src)
    find = lambda prefix: os.path.join(src, next(n for n in names if n.startswith(prefix) and n.lower().endswith(".wav")))
    a_rate, a = read_wav(find("321654"))
    b_rate, b = read_wav(find("360328"))
    c_rate, c = read_wav(find("844420"))
    a_end = len(a) / a_rate - 0.02
    clips = {
        # Near: the whole very close strike; the other, its rain faded out
        # where it is mostly rain, the hiss above 7 kHz taken off.
        "thunder_near_1": make(a, a_rate, 0.0, a_end, None, 0.005, a_end - 0.35, a_end, 44100),
        "thunder_near_2": make(b, b_rate, 0.95, 7.6, 7000, 0.02, 4.5, 7.6, 44100),
        # Middle (1.5..5 km): the same strikes without their crack.
        "thunder_mid_1": make(a, a_rate, 0.0, a_end, 1200, 0.12, a_end - 0.5, a_end, 22050),
        "thunder_mid_2": make(b, b_rate, 0.95, 7.6, 1200, 0.12, 4.5, 7.6, 22050),
        # Far (5 km and more): rumble only, rising slowly.
        "thunder_far_1": make(c, c_rate, 0.55, 22.0, 450, 0.5, 12.0, 22.0, 22050),
        "thunder_far_2": make(c, c_rate, 5.0, 30.0, 450, 0.6, 18.0, 30.0, 22050),
        "thunder_far_3": make(a, a_rate, 0.0, a_end, 350, 0.35, a_end - 1.5, a_end, 22050),
    }
    os.makedirs(out, exist_ok=True)
    for name, clip in clips.items():
        rate = 44100 if "near" in name else 22050
        write_wav(os.path.join(out, name + ".wav"), rate, clip)
        print("%s: %.2f s, peak %.1f dBFS" % (name, len(clip) / rate, 20 * math.log10(max(float(np.max(np.abs(clip))), 1e-9))))
    return 0


if __name__ == "__main__":
    sys.exit(main())

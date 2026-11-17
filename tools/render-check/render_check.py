"""openRTS render check: fixed scenes through the viewer, one capture each
per setting, and what the viewer reports of them: the HDR frame's light in
linear light (the mean, median, percentiles and most, by region), the GPU's
passes, the bindings, the video memory. The captures are written as PNG to
look at. docs/experimental-effects.md, "The frame's encoding" (PBR stage 0),
"Physically based light" (stage 1) and "The sky's light" (stage 2).

    python render_check.py <viewer exe> <installation> <output folder> [scene ...] [--variants linear,pbr]

A variant is a set of viewer flags (VARIANTS); every scene runs in each.
"""

import os
import re
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
# The scenes' views (debug views with the root at E:\Games\MDD_Launcher\rts_main, the
# experimental path off: a variant turns it on); --root overrides the root.
VIEWS = os.path.join(HERE, "views")

# Standing, the simulation paused: the same picture every run.
TRAIN = ["--route", "Kuban_RTS", "--consist", "mdd VL80s-606", "--start", "on", "--speed", "0", "--paused"]

# Beside the locomotive, above and ahead of it, looking at its front: fixed in
# the car's own space (--capture-camera eye, target), so every run frames it alike
# (the "above" view drifts).
BESIDE = ["--capture-camera", "7", "5", "16", "0", "1.5", "0"]

# The eight scenes of PBR stage 0 and two of a low sun for stage 1 (docs/experimental-effects.md).
SCENES = {
    # 1. Day, an open stretch.
    "day_open": ["--view", os.path.join(VIEWS, "kuban_day_open.view"), "--time", "12:00"],
    # 2. Night, the station's lamps and the train's lights.
    "night_lamps": TRAIN + ["--time", "23:00"] + BESIDE,
    # 3. The locomotive's cab by day.
    "cab_day": TRAIN + ["--spawn", "cab", "--time", "12:00"],
    # 4. In a tunnel, looking out (Novokuznetsk).
    "tunnel": ["--view", os.path.join(VIEWS, "novokuznetsk_tunnel.view")],
    # 5. Shiny bodies (LoShine / HiShine) in the sun.
    "shiny_day": TRAIN + ["--time", "12:00"] + BESIDE,
    # 6. FullBright and night textures: lit windows at night (Novokuznetsk's works).
    "night_textures": ["--view", os.path.join(VIEWS, "novokuznetsk_works_night.view"), "--time", "23:00"],
    # 7. Transparent surfaces: the cab's glass and rain at dusk.
    "glass_rain": TRAIN + ["--spawn", "cab", "--time", "19:30", "--weather", "rain"],
    # 8. The sky towards the horizon (openRTS's sky; later its reflections).
    "sky_horizon": ["--view", os.path.join(VIEWS, "chernyshevsk_horizon.view")],
    # Stage 1: a low sun, where the reflections grow at grazing angles (Fresnel).
    "low_sun_open": ["--view", os.path.join(VIEWS, "kuban_day_open.view"), "--time", "19:00"],
    "low_sun_shiny": TRAIN + ["--time", "19:00"] + BESIDE,
}

VARIANTS = {
    "gamma": ["--experimental", "--hdr", "--hdr-encoding", "gamma"],
    # The linear frame with the light as before PBR (stage 0's).
    "linear": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "off"],
    # Physically based light with the old sky light (stage 1).
    "pbr1": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "off"],
    # Physically based light with the sky's light from its probe (stage 2).
    "pbr": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on"],
    # The same under RTS's dome (the probe holds the dome's layers).
    # The sky's light tuned (SceneEffectSettings::sky_light_*): weaker and stronger.
    "pbr_sky_dim": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on",
                    "--sky-light-strength", "0.5", "--sky-reflections", "0.5"],
    "pbr_sky_bright": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on",
                       "--sky-light-strength", "1.5", "--sky-reflections", "1.5"],
    # Auto exposure (PBR stage 3) on.
    "pbr_ae": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--auto-exposure", "on"],
    # Materials (PBR stage 4): a part of the models' material as the colour.
    "view_base": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--material-view", "base"],
    "view_roughness": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--material-view", "roughness"],
    "view_metallic": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--material-view", "metallic"],
    "view_normal": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--material-view", "normal"],
    "view_ao": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--material-view", "ao"],
    "pbr_dome": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "on", "--native-sky", "off"],
    "pbr1_dome": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--sky-light", "off", "--native-sky", "off"],
    "rts": ["--hdr"],
    "switch": ["--experimental", "--hdr", "--hdr-encoding", "linear", "--pbr", "on", "--capture-switch-path"],
}

COMMON = ["--no-sound", "--fps-limit", "30", "--no-discord", "--capture-at", "20"]


def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not m:
        return False
    w, h = int(m.group(1)), int(m.group(2))
    pixels = data[m.end():m.end() + w * h * 3]
    raw = b"".join(b"\x00" + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png_bytes = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png_bytes += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(png, "wb").write(png_bytes)
    return True


def run(viewer, root, out, scene, variant):
    ppm = os.path.join(out, "%s_%s.ppm" % (scene, variant))
    # `--window` first, then `--view <file>` or `--route <name>` (the viewer's main).
    # The auto exposure off unless the variant turns it on (on by default since 2026-11-17).
    args = [viewer, "--window"] + SCENES[scene] + ["--root", root] + ["--auto-exposure", "off"] + VARIANTS[variant] + COMMON + ["--capture", ppm]
    try:
        result = subprocess.run(args, cwd=os.path.dirname(viewer), capture_output=True, timeout=300)
        log = result.stdout.decode("utf-8", errors="replace") + result.stderr.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        log = "timed out"
    open(os.path.join(out, "%s_%s.log" % (scene, variant)), "w", encoding="utf-8").write(log)
    report = {"scene": scene, "variant": variant}
    m = re.search(r"hdr frame \(([^)]*)\):(.*)", log)
    if m:
        report["encoding"] = m.group(1)
        for region in re.finditer(r" (\w+) mean ([\d.]+) median ([\d.]+) p10 ([\d.]+) p90 ([\d.]+) p99 ([\d.]+) max ([\d.]+);", m.group(2)):
            report[region.group(1)] = tuple(float(region.group(k)) for k in range(2, 8))
    m = re.search(r"gpu frame ([\d.]+) ms: (.*)", log)
    if m:
        report["gpu"] = float(m.group(1))
        report["passes"] = m.group(2).strip()
    m = re.search(r"switches: (\d+) pipelines, (\d+) models, (\d+) bindings", log)
    if m:
        report["bindings"] = int(m.group(3))
    m = re.search(r"textures on the GPU: (\d+), (\d+) MB", log)
    if m:
        report["textures_mb"] = int(m.group(2))
    m = re.search(r"frame times: ([\d.]+) ms avg, ([\d.]+) ms worst", log)
    if m:
        report["frame_ms"] = (float(m.group(1)), float(m.group(2)))
    report["errors"] = len(re.findall(r"(?i)validation|wgpu.*error|panic", log))
    report["switched"] = len(re.findall(r"capture: the rendering path switched", log))
    if os.path.exists(ppm) and ppm_to_png(ppm, ppm[:-4] + ".png"):
        os.remove(ppm)
        report["png"] = ppm[:-4] + ".png"
    return report


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--variants")]
    variants = ["linear", "pbr"]
    for a in sys.argv[1:]:
        if a.startswith("--variants"):
            variants = a.split("=", 1)[1].split(",")
    if len(args) < 3:
        print(__doc__)
        return 64
    viewer, root, out = args[0], args[1], args[2]
    scenes = args[3:] or list(SCENES)
    os.makedirs(out, exist_ok=True)
    rows = []
    for scene in scenes:
        for variant in variants:
            r = run(viewer, root, out, scene, variant)
            rows.append(r)
            regions = " ".join("%s %.4f/%.4f" % (k, r[k][0], r[k][1]) for k in ("frame", "top", "middle", "bottom", "centre") if k in r)
            print("%-15s %-7s %s | mean/median %s | p99 %.3f max %.2f | gpu %s ms | bindings %s | textures %s MB | errors %d%s" % (
                scene, variant, r.get("encoding", "-"), regions, r["frame"][4] if "frame" in r else -1, r["frame"][5] if "frame" in r else -1,
                r.get("gpu", "-"), r.get("bindings", "-"), r.get("textures_mb", "-"), r["errors"],
                (", path switched %d" % r["switched"]) if r["switched"] else ""), flush=True)
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as f:
        for r in rows:
            f.write(repr(r) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

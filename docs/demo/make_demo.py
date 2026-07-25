"""Generate the animated hero graphic for vcv-osc.

A designed (not screen-recorded) animation — no running Rack needed. OSC
controllers on the left, the vcv-osc *OSC Controller* module in the middle
(living inside your patch), and what it drives on the right: parameters, and
cables & modules. Every wire is **bidirectional** — commands flow out, values
and change-events flow back (turn a fader on your phone → the knob moves; turn a
knob in Rack → a `/param/value` or `/event/*` comes back).

Visual family shared with the sibling osc-bridge project (same palette and
travelling-dot style); this script is adapted from its `docs/demo/make_demo.py`.

Output is an **APNG** (`.png` extension, written by Pillow with `save_all=True`).
GitHub renders APNG inline and auto-plays it — no video, no play button.

Run from the repo root:
    python3 docs/demo/make_demo.py
Output:
    docs/demo/vcv-osc-demo.png
"""
from __future__ import annotations
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# ── Canvas ──────────────────────────────────────────────────────────────
W, H = 960, 470

# ── Palette (Tokyo Night-ish — same family as osc-bridge) ───────────────
BG       = (24, 25, 38)
PANEL    = (32, 34, 52)
PANEL_HI = (40, 43, 64)
FG       = (192, 202, 245)
DIM      = (110, 118, 160)
WIRE     = (54, 58, 86)
CORE     = (122, 162, 247)   # vcv-osc module — blue
MCP      = (187, 154, 247)   # osc-bridge / passthrough — purple
PARAM    = (255, 158, 100)   # parameters — amber
STRUCT   = (158, 206, 106)   # cables & modules — green
WHITE    = (235, 240, 255)

# ── Fonts ───────────────────────────────────────────────────────────────
def font(size, bold=False):
    base = "/usr/share/fonts/truetype/dejavu/"
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    p = Path(base + name)
    return ImageFont.truetype(str(p), size) if p.exists() else ImageFont.load_default()

F_TITLE = font(30, bold=True)
F_SUB   = font(14)
F_BOX   = font(15, bold=True)
F_SMALL = font(11)
F_CORE  = font(20, bold=True)
F_FOOT  = font(13, bold=True)

# ── Animation ───────────────────────────────────────────────────────────
FPS = 14
N_FRAMES = 42                       # 3.0 s, seamless loop
FWD_DOTS = 3                        # commands: outward
RET_DOTS = 2                        # values / change-events: inward
LANE = 4.5                          # perpendicular offset between the two lanes

# ── Geometry ────────────────────────────────────────────────────────────
BOX_W_L, BOX_H_L = 224, 74
BOX_W_R, BOX_H_R = 232, 92
CORE_W, CORE_H   = 200, 150

LX = 32
CX = (W - CORE_W) // 2
RX = W - BOX_W_R - 44
CORE_Y = (H - CORE_H) // 2 + 6

# (label, detail, accent, y-center)
SOURCES = [
    ("TouchOSC · Open Stage Control", "surface auto-built via OSCQuery", STRUCT, 150),
    ("Max · Python · CLI",            "scripted & live control",          FG,     240),
    ("osc-bridge",                    "/vcv passthrough",                 MCP,    330),
]
TARGETS = [
    ("Parameters",       "set · get · watch — by name", PARAM,  188),
    ("Cables & modules", "patch · add/remove · presets", STRUCT, 312),
]

def src_box(i):
    _, _, _, cy = SOURCES[i]
    return (LX, cy - BOX_H_L // 2, LX + BOX_W_L, cy + BOX_H_L // 2)

def tgt_box(i):
    _, _, _, cy = TARGETS[i]
    return (RX, cy - BOX_H_R // 2, RX + BOX_W_R, cy + BOX_H_R // 2)

CORE_BOX = (CX, CORE_Y, CX + CORE_W, CORE_Y + CORE_H)

# ── Helpers ─────────────────────────────────────────────────────────────
def lerp(a, b, t):
    return a + (b - a) * t

def _mix(c1, c2, t):
    return tuple(int(lerp(c1[i], c2[i], t)) for i in range(3))

def rounded(d, box, fill, outline=None, width=1, radius=12):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)

def text_c(d, cx, cy, s, fnt, fill):
    bb = d.textbbox((0, 0), s, font=fnt)
    d.text((cx - (bb[2] - bb[0]) / 2, cy - (bb[3] - bb[1]) / 2), s, font=fnt, fill=fill)

def perp_offset(p0, p1, dd):
    """Shift the segment p0->p1 by `dd` along its perpendicular."""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    L = math.hypot(dx, dy) or 1.0
    px, py = -dy / L, dx / L
    return (p0[0] + px * dd, p0[1] + py * dd), (p1[0] + px * dd, p1[1] + py * dd)

def dot(d, p0, p1, t, color, r=3.2):
    x = lerp(p0[0], p1[0], t)
    y = lerp(p0[1], p1[1], t)
    d.ellipse([x - r - 3, y - r - 3, x + r + 3, y + r + 3], fill=_mix(color, BG, 0.80))
    d.ellipse([x - r, y - r, x + r, y + r], fill=color)

# ── Frame ───────────────────────────────────────────────────────────────
def make_frame(frame):
    phase = frame / N_FRAMES                       # 0..1, wraps seamlessly
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # Title + subtitle — a plain statement of what it does, no hype.
    text_c(d, W / 2, 34, "vcv-osc", F_TITLE, WHITE)
    text_c(d, W / 2, 62,
           "OSC control of a whole VCV Rack patch — every param, cable and module, by name",
           F_SUB, DIM)

    core_l = (CORE_BOX[0], CORE_Y + CORE_H / 2)
    core_r = (CORE_BOX[2], CORE_Y + CORE_H / 2)

    # ── Main wires (drawn once, down the middle) ────────────────────────
    legs = []  # (p_near_box, p_near_core, accent, is_input)
    for i, (_, _, accent, _) in enumerate(SOURCES):
        b = src_box(i)
        legs.append(((b[2], (b[1] + b[3]) / 2), core_l, accent, True))
    for i, (_, _, accent, _) in enumerate(TARGETS):
        b = tgt_box(i)
        legs.append(((b[0], (b[1] + b[3]) / 2), core_r, accent, False))
    for p_box, p_core, _, _ in legs:
        d.line([p_box, p_core], fill=WIRE, width=2)

    # ── Travelling packets ──────────────────────────────────────────────
    # Forward lane: commands flowing out (controller→module, module→target).
    # Return lane:  values / change-events flowing back (the other way).
    for p_box, p_core, accent, is_input in legs:
        if is_input:
            fwd_a, fwd_b = p_box, p_core            # source → core
            fwd_col = MCP if accent is MCP else CORE
        else:
            fwd_a, fwd_b = p_core, p_box            # core → target
            fwd_col = accent
        f0, f1 = perp_offset(fwd_a, fwd_b, LANE)
        for k in range(FWD_DOTS):
            t = (phase + k / FWD_DOTS) % 1.0
            dot(d, f0, f1, t, fwd_col, r=3.2)
        # return lane — opposite direction, dimmer + smaller
        r0, r1 = perp_offset(fwd_b, fwd_a, LANE)
        ret_col = _mix(accent if not is_input else FG, BG, 0.30)
        for k in range(RET_DOTS):
            t = (phase + k / RET_DOTS) % 1.0
            dot(d, r0, r1, t, ret_col, r=2.3)

    # ── Source boxes ────────────────────────────────────────────────────
    for i, (label, detail, accent, cy) in enumerate(SOURCES):
        b = src_box(i)
        rounded(d, b, PANEL, outline=PANEL_HI, width=1)
        d.text((b[0] + 16, b[1] + 13), label, font=F_BOX,
               fill=(MCP if accent is MCP else FG))
        d.text((b[0] + 16, b[1] + 39), detail, font=F_SMALL, fill=DIM)

    # ── Core — subtle pulse on the outline, seamless (period = N_FRAMES) ─
    pulse = 0.5 + 0.5 * math.sin(2 * math.pi * phase)
    rounded(d, CORE_BOX, PANEL_HI, outline=_mix(WIRE, CORE, 0.35 + 0.55 * pulse),
            width=3, radius=16)
    cx = CX + CORE_W / 2
    text_c(d, cx, CORE_Y + 34, "vcv-osc", F_CORE, CORE)
    text_c(d, cx, CORE_Y + 62, "OSC Controller module", F_SMALL, FG)
    text_c(d, cx, CORE_Y + 82, "inside your patch", F_SMALL, DIM)
    text_c(d, cx, CORE_Y + 108, "OSC 7770 · OSCQuery 7772", F_SMALL, DIM)

    # ── Target boxes ────────────────────────────────────────────────────
    for i, (label, detail, accent, cy) in enumerate(TARGETS):
        b = tgt_box(i)
        rounded(d, b, PANEL, outline=_mix(PANEL_HI, accent, 0.45), width=2)
        d.text((b[0] + 16, b[1] + 16), label, font=F_BOX, fill=accent)
        d.text((b[0] + 16, b[1] + 44), detail, font=F_SMALL, fill=DIM)
        d.text((b[0] + 16, b[1] + 62),
               "stable engine API" if accent is PARAM else "unofficial app API, isolated",
               font=F_SMALL, fill=_mix(accent, BG, 0.35))

    # Footer — plain keywords, no self-praise.
    text_c(d, W / 2, H - 26,
           "by name   ·   OSCQuery auto-surface   ·   bidirectional   ·   github.com/roomi-fields/vcv-osc",
           F_FOOT, _mix(FG, CORE, 0.4))

    return img

# ── Main ────────────────────────────────────────────────────────────────
def main():
    out = Path(__file__).parent / "vcv-osc-demo.png"
    rgb = [make_frame(i) for i in range(N_FRAMES)]

    # One shared 128-colour palette, dithering off: flat regions stay
    # byte-identical frame to frame, which compresses well and keeps the loop
    # perfectly clean (same approach as the osc-bridge demo).
    pal = rgb[N_FRAMES // 4].quantize(colors=128, method=Image.Quantize.MEDIANCUT)
    frames = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in rgb]
    durations = [int(1000 / FPS)] * N_FRAMES
    frames[0].save(
        out, save_all=True, append_images=frames[1:],
        duration=durations, loop=0, optimize=False,
    )
    kb = out.stat().st_size / 1024
    print(f"Generated: {out}  ({N_FRAMES} frames @ {FPS}fps, {N_FRAMES / FPS:.1f}s, {kb:.0f} KB)")

if __name__ == "__main__":
    main()

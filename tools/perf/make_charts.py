#!/usr/bin/env python3
"""Render docs/performance.md's charts from docs/perf/data.json (SCRUM-117).

Standard library only, so it runs anywhere the repo is checked out:

    python3 tools/perf/make_charts.py

Writes docs/perf/*.svg. Each SVG carries its own light and dark colours
(prefers-color-scheme), so it reads on both GitHub themes, and a <title> per
mark for a hover tooltip when opened directly. Every number drawn here is
also in a table in docs/performance.md.
"""
import json
import math
import os
import statistics

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "docs", "perf", "data.json")
OUT = os.path.join(ROOT, "docs", "perf")

# Validated categorical slots 1-3 (blue, orange, aqua), light and dark steps.
STYLE = """
<style>
  .surface { fill: #fcfcfb; }
  .ink     { fill: #0b0b0b; }
  .ink2    { fill: #52514e; }
  .grid    { stroke: #e4e3df; stroke-width: 1; }
  .axis    { stroke: #8a8984; stroke-width: 1; }
  .ref     { stroke: #52514e; stroke-width: 1.5; stroke-dasharray: 4 3; }
  .s1      { fill: #2a78d6; }
  .s2      { fill: #eb6834; }
  .s3      { fill: #1baf7a; }
  .whisker { stroke: #0b0b0b; stroke-width: 1.5; }
  text     { font-family: system-ui, -apple-system, "Segoe UI", sans-serif; font-size: 12px; }
  .title   { font-size: 14px; font-weight: 600; }
  @media (prefers-color-scheme: dark) {
    .surface { fill: #1a1a19; }
    .ink     { fill: #f0efec; }
    .ink2    { fill: #c3c2b7; }
    .grid    { stroke: #33332f; }
    .axis    { stroke: #6f6e69; }
    .ref     { stroke: #c3c2b7; }
    .s1      { fill: #3987e5; }
    .s2      { fill: #d95926; }
    .s3      { fill: #199e70; }
    .whisker { stroke: #f0efec; }
  }
</style>
"""


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


def svg(w, h, body, label):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}" role="img" aria-label="{esc(label)}">'
            f'{STYLE}<rect class="surface" width="{w}" height="{h}"/>'
            + "".join(body) + "</svg>\n")


def text(x, y, s, cls="ink", anchor="start", extra=""):
    return (f'<text class="{cls}" x="{x:.1f}" y="{y:.1f}" '
            f'text-anchor="{anchor}" {extra}>{esc(s)}</text>')


def rect(x, y, w, h, cls, tip):
    return (f'<rect class="{cls}" x="{x:.1f}" y="{y:.1f}" width="{max(w, 0):.1f}" '
            f'height="{h:.1f}" rx="2"><title>{esc(tip)}</title></rect>')


def legend(x, y, items):
    out, cx = [], x
    for cls, name in items:
        out.append(f'<rect class="{cls}" x="{cx}" y="{y - 9}" width="10" height="10" rx="2"/>')
        out.append(text(cx + 14, y, name, "ink2"))
        cx += 22 + 6.6 * len(name)
    return out


def frame_breakdown(d):
    win = d["frame_windows_tcg"]
    mean = lambda k: statistics.mean(a for a, _ in win[k])
    sim, wait = mean("sim"), mean("tic_wait")
    tcg = [("s1", "busy-wait for next tic", wait),
           ("s2", "game simulation", sim - wait),
           ("s3", "sound + render", mean("sound") + mean("render"))]
    k = d["frame_kvm_scrum87"]
    kvm = [("s1", "busy-wait for next tic", k["tic_wait_avg"]),
           ("s2", "game simulation", k["sim_avg"] - k["tic_wait_avg"]),
           ("s3", "sound + render", k["total_avg"] - k["sim_avg"])]

    W, left, right = 760, 210, 70
    xmax = 16.0
    sx = lambda v: left + (W - left - right) * v / xmax
    body = [text(16, 26, "Average frame time, split by where it goes (ms)", "ink title"),
            *legend(16, 52, [(c, n) for c, n, _ in tcg])]
    top = 76
    for g in range(0, 17, 2):
        body.append(f'<line class="grid" x1="{sx(g):.1f}" y1="{top}" x2="{sx(g):.1f}" y2="{top + 104}"/>')
        body.append(text(sx(g), top + 120, g, "ink2", "middle"))
    for i, (name, segs) in enumerate([("KVM, Sep 2026 (SCRUM-87)", kvm),
                                       ("TCG, no KVM (this report)", tcg)]):
        y = top + 12 + i * 48
        body.append(text(16, y + 18, name, "ink"))
        x = 0.0
        for cls, label, v in segs:
            x0, x1 = sx(x), sx(x + v)
            body.append(rect(x0 + (1 if x else 0), y, x1 - x0 - (2 if x else 1), 28, cls,
                             f"{name}: {label} {v:.2f} ms"))
            if x1 - x0 > 46:
                body.append(text((x0 + x1) / 2, y + 18, f"{v:.1f}", "surface", "middle",
                                 'style="font-weight:600"'))
            x += v
        body.append(text(sx(x) + 6, y + 18, f"{x:.1f} ms", "ink"))
    return svg(W, top + 132, body, "Average frame time split into busy-wait, simulation and sound plus render, KVM versus TCG")


def frame_max_hist(d):
    maxes = [m for _, m in d["frame_windows_tcg"]["total"]]
    bins = list(range(0, 62, 2))          # 0-2 ... 58-60, then overflow
    counts = [0] * len(bins)
    for m in maxes:
        i = min(m // 2, len(bins) - 1)
        counts[int(i)] += 1
    ms = sorted(maxes)
    p50, p95 = statistics.median(ms), ms[int(len(ms) * 0.95)]

    W, H, left, right, top, bot = 760, 300, 56, 24, 60, 50
    pw, ph = W - left - right, H - top - bot
    cmax = max(counts)
    ymax = int(math.ceil(cmax / 10.0)) * 10
    bw = pw / len(bins)
    sy = lambda c: top + ph - ph * c / ymax
    body = [text(16, 26, "Worst frame in each 1-second window, TCG (count of windows)", "ink title"),
            text(16, 44, f"{len(maxes)} windows of 35 frames; median {p50:g} ms, p95 {p95} ms, worst {ms[-1]} ms (level load)", "ink2")]
    for c in range(0, ymax + 1, 10):
        body.append(f'<line class="grid" x1="{left}" y1="{sy(c):.1f}" x2="{W - right}" y2="{sy(c):.1f}"/>')
        body.append(text(left - 6, sy(c) + 4, c, "ink2", "end"))
    for i, c in enumerate(counts):
        x = left + i * bw
        lo = bins[i]
        lab = f"{lo}-{lo + 2} ms" if i < len(bins) - 1 else f">= {lo} ms"
        if c:
            body.append(rect(x + 1, sy(c), bw - 2, top + ph - sy(c), "s1", f"{lab}: {c} windows"))
        if lo % 10 == 0:
            body.append(text(x, top + ph + 16, lo if i < len(bins) - 1 else f"{lo}+", "ink2", "middle"))
    xb = left + 15 * bw   # 30 ms = 33 fps budget
    body.append(f'<line class="ref" x1="{xb:.1f}" y1="{top - 6}" x2="{xb:.1f}" y2="{top + ph}"/>')
    body.append(text(xb + 4, top + 4, "30 ms budget (33 fps)", "ink2"))
    body.append(text(left + pw / 2, H - 12, "worst frame in the window (ms)", "ink2", "middle"))
    return svg(W, H, body, "Histogram of per-window worst frame time against the 30 ms budget")


def syscall_latency(d):
    runs = d["syscall_bench_cycles"]
    items = sorted(((statistics.median(v), min(v), max(v), k) for k, v in runs.items()))
    W, left, right, top = 760, 230, 80, 64
    lo, hi = 2.0, 5.0                     # log10: 100 .. 100k cycles
    sx = lambda v: left + (W - left - right) * (math.log10(v) - lo) / (hi - lo)
    rowh = 30
    H = top + rowh * len(items) + 40
    body = [text(16, 26, "Syscall round trip, median cycles over 6 runs (log scale)", "ink title"),
            text(16, 44, "Bar = median; whisker = fastest and slowest run. QEMU/TCG, rdtsc.", "ink2")]
    for p in range(2, 6):
        x = sx(10 ** p)
        body.append(f'<line class="grid" x1="{x:.1f}" y1="{top - 4}" x2="{x:.1f}" y2="{H - 34}"/>')
        body.append(text(x, H - 18, f"{10 ** p:,}", "ink2", "middle"))
    for i, (med, mn, mx, name) in enumerate(items):
        y = top + i * rowh
        body.append(text(16, y + 15, name, "ink"))
        body.append(rect(sx(100), y + 3, sx(med) - sx(100), 16, "s1",
                         f"{name}: median {med:,.0f}, range {mn:,}-{mx:,} cycles"))
        body.append(f'<line class="whisker" x1="{sx(mn):.1f}" y1="{y + 11}" x2="{sx(mx):.1f}" y2="{y + 11}"/>')
        body.append(text(sx(mx) + 6, y + 15, f"{med:,.0f}", "ink2"))
    return svg(W, H, body, "Syscall round-trip cost in cycles, median with range, log scale")


def memory(d):
    fb = d["shadow_fb_pages"]
    ctx = [(k, v) for k, v in d["pslist_pages"].items() if k != "shell (after launching doom)"]
    W, left, right, top = 760, 120, 110, 76
    xmax = 3000
    sx = lambda v: left + (W - left - right) * v / xmax
    rowh = 34
    H = top + rowh * len(ctx) + 40
    body = [text(16, 26, "Pages owned per running context (4 KiB pages)", "ink title"),
            *legend(16, 52, [("s1", "shadow framebuffer (3 MiB at 1024x768x32)"),
                             ("s2", "everything else: image, stack, heap")])]
    for g in range(0, 3001, 500):
        body.append(f'<line class="grid" x1="{sx(g):.1f}" y1="{top - 4}" x2="{sx(g):.1f}" y2="{H - 34}"/>')
        body.append(text(sx(g), H - 18, f"{g:,}", "ink2", "middle"))
    for i, (name, pages) in enumerate(ctx):
        y = top + i * rowh
        body.append(text(16, y + 16, name, "ink"))
        body.append(rect(sx(0), y + 2, sx(fb) - sx(0) - 1, 20, "s1", f"{name}: shadow framebuffer {fb} pages"))
        body.append(rect(sx(fb) + 1, y + 2, sx(pages) - sx(fb) - 1, 20, "s2",
                         f"{name}: other {pages - fb} pages"))
        body.append(text(sx(pages) + 6, y + 16, f"{pages:,} ({pages * 4 / 1024:.1f} MiB)", "ink2"))
    return svg(W, H, body, "Pages per context split into shadow framebuffer and the rest")


def main():
    d = json.load(open(DATA))
    for name, fn in [("frame-breakdown", frame_breakdown), ("frame-max-hist", frame_max_hist),
                     ("syscall-latency", syscall_latency), ("memory-per-context", memory)]:
        with open(os.path.join(OUT, name + ".svg"), "w") as f:
            f.write(fn(d))
        print("wrote docs/perf/%s.svg" % name)


if __name__ == "__main__":
    main()

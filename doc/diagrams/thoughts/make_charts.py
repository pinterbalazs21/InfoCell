"""Dependency-free SVG charts for doc/Thoughts.md."""
import math, os, sys

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

SURF = "#fcfcfb"; INK = "#0b0b0b"; INK2 = "#52514e"; GRID = "#e4e3df"
S1, S2, S3 = "#2a78d6", "#eb6834", "#1baf7a"
W, H = 760, 420
ML, MR, MT, MB = 78, 30, 56, 58
PW, PH = W - ML - MR, H - MT - MB
FONT = "font-family='Inter,Segoe UI,Helvetica,Arial,sans-serif'"


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Chart:
    def __init__(self, title, xlabel, ylabel, xr, yr, xlog=False, ylog=False):
        self.title, self.xlabel, self.ylabel = title, xlabel, ylabel
        self.xr, self.yr, self.xlog, self.ylog = xr, yr, xlog, ylog
        self.parts = []

    def _t(self, v, r, log):
        a, b = r
        if log:
            v, a, b = math.log10(v), math.log10(a), math.log10(b)
        return (v - a) / (b - a)

    def X(self, v):
        return ML + self._t(v, self.xr, self.xlog) * PW

    def Y(self, v):
        return MT + PH - self._t(v, self.yr, self.ylog) * PH

    def text(self, x, y, s, anchor="start", color=INK, size=12, weight="normal", dy=0):
        lines = s.split("\n")
        for i, ln in enumerate(lines):
            self.parts.append(
                f"<text x='{x:.1f}' y='{y + dy + i * (size + 3):.1f}' text-anchor='{anchor}' fill='{color}' "
                f"font-size='{size}' font-weight='{weight}' {FONT}>{esc(ln)}</text>")

    def label(self, xv, yv, s, dx=0, dy=0, anchor="start", color=INK):
        self.text(self.X(xv) + dx, self.Y(yv) + dy, s, anchor, color)

    def line(self, xs, ys, color, dash=None, width=2):
        pts = " ".join(f"{self.X(x):.1f},{self.Y(y):.1f}" for x, y in zip(xs, ys))
        d = f" stroke-dasharray='{dash}'" if dash else ""
        self.parts.append(f"<polyline points='{pts}' fill='none' stroke='{color}' stroke-width='{width}' stroke-linejoin='round' stroke-linecap='round'{d}/>")

    def dots(self, xs, ys, color, r=3.5):
        for x, y in zip(xs, ys):
            self.parts.append(f"<circle cx='{self.X(x):.1f}' cy='{self.Y(y):.1f}' r='{r}' fill='{color}' stroke='{SURF}' stroke-width='2'/>")

    def vline(self, xv, color=INK2, dash="3,4"):
        x = self.X(xv)
        self.parts.append(f"<line x1='{x:.1f}' x2='{x:.1f}' y1='{MT}' y2='{MT + PH}' stroke='{color}' stroke-dasharray='{dash}'/>")

    def bar(self, cx, w, yv, color):
        x0, y0, y1 = self.X(cx) - w / 2, self.Y(yv), self.Y(self.yr[0])
        h = y1 - y0
        r = min(4, h / 2)
        self.parts.append(
            f"<path d='M{x0:.1f},{y1:.1f} V{y0 + r:.1f} Q{x0:.1f},{y0:.1f} {x0 + r:.1f},{y0:.1f} H{x0 + w - r:.1f} "
            f"Q{x0 + w:.1f},{y0:.1f} {x0 + w:.1f},{y0 + r:.1f} V{y1:.1f} Z' fill='{color}'/>")

    def axes(self, xticks, yticks, xfmt=str, yfmt=str, xgrid=True):
        g = []
        for t in yticks:
            y = self.Y(t)
            g.append(f"<line x1='{ML}' x2='{ML + PW}' y1='{y:.1f}' y2='{y:.1f}' stroke='{GRID}'/>")
            g.append(f"<text x='{ML - 8}' y='{y + 4:.1f}' text-anchor='end' fill='{INK2}' font-size='11' {FONT}>{esc(yfmt(t))}</text>")
        for t in xticks:
            x = self.X(t) if not isinstance(t, tuple) else self.X(t[0])
            lab = xfmt(t) if not isinstance(t, tuple) else t[1]
            if xgrid:
                g.append(f"<line x1='{x:.1f}' x2='{x:.1f}' y1='{MT}' y2='{MT + PH}' stroke='{GRID}'/>")
            for i, ln in enumerate(lab.split("\n")):
                g.append(f"<text x='{x:.1f}' y='{MT + PH + 18 + i * 14}' text-anchor='middle' fill='{INK2}' font-size='11' {FONT}>{esc(ln)}</text>")
        g.append(f"<line x1='{ML}' x2='{ML + PW}' y1='{MT + PH}' y2='{MT + PH}' stroke='{INK2}' stroke-width='1'/>")
        self.parts = g + self.parts

    def save(self, name):
        head = [
            f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {W} {H}' width='{W}' height='{H}'>",
            f"<rect width='{W}' height='{H}' rx='8' fill='{SURF}'/>",
            f"<text x='{ML - 60}' y='28' fill='{INK}' font-size='15' font-weight='700' {FONT}>{esc(self.title)}</text>",
            f"<text x='{ML + PW / 2}' y='{H - 10}' text-anchor='middle' fill='{INK2}' font-size='12' {FONT}>{esc(self.xlabel)}</text>",
            f"<text transform='translate(16,{MT + PH / 2}) rotate(-90)' text-anchor='middle' fill='{INK2}' font-size='12' {FONT}>{esc(self.ylabel)}</text>",
        ]
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
            f.write("\n".join(head + self.parts + ["</svg>"]))


def pow10(t):
    e = int(round(math.log10(t)))
    sup = str.maketrans("-0123456789", "⁻⁰¹²³⁴⁵⁶⁷⁸⁹")
    return "1" if e == 0 else ("10" if e == 1 else "10" + str(e).translate(sup))


# 1) Program explosion
n = list(range(1, 21))
c = Chart("Move programs of length n vs. what they actually do", "program length n (steps)",
          "count (log scale)", (1, 20), (1, 1e13), ylog=True)
c.axes([1, 5, 10, 15, 20], [10 ** k for k in range(0, 14, 2)], yfmt=pow10)
c.line(n, [4 ** k for k in n], S2, dash="6,5")
c.line(n, [2 ** k for k in n], S2)
c.line(n, [(k + 1) ** 2 for k in n], S1, dash="6,5")
c.line(n, [k + 1 for k in n], S1)
c.label(20, 4 ** 20, "2D programs: 4ⁿ", dx=-4, dy=18, anchor="end")
c.label(20, 2 ** 20, "1D programs: 2ⁿ", dx=-4, dy=18, anchor="end")
c.label(20, 441, "2D distinct results: ~(n+1)²", dx=-4, dy=-8, anchor="end")
c.label(20, 21, "1D distinct results: n+1", dx=-4, dy=-8, anchor="end")
c.save("01_program_explosion.svg")

# 2) Scaling law (Kaplan et al. 2020)
Ns = [10 ** (6 + i * 0.1) for i in range(61)]
L = lambda N: (8.8e13 / N) ** 0.076
c = Chart("Neural scaling law: 100× more parameters → only ~30% lower loss",
          "model size N (parameters, log)", "test loss (nats/token, log)", (1e6, 1e12), (1.4, 6), xlog=True, ylog=True)
c.axes([10 ** k for k in range(6, 13)], [1.5, 2, 3, 4, 5, 6], xfmt=pow10, yfmt=lambda v: f"{v:g}")
c.line(Ns, [L(x) for x in Ns], S1)
c.dots([1e7, 1e9, 1e11], [L(1e7), L(1e9), L(1e11)], S1, r=5)
c.label(1e7, L(1e7), f"{L(1e7):.2f}", dx=8, dy=-8)
c.label(1e9, L(1e9), f"{L(1e9):.2f}", dx=8, dy=-8)
c.label(1e11, L(1e11), f"{L(1e11):.2f}", dx=8, dy=-8)
c.label(1e10, 4.6, "L(N) = (Nc / N)^0.076   (Kaplan et al., 2020)", anchor="middle", color=INK2)
c.save("02_scaling_law.svg")

# 3) MDL two-part code
xs = [i * 0.05 for i in range(201)]
LH = [8 * x + 5 for x in xs]
LDH = [300 * math.exp(-0.55 * x) + 10 for x in xs]
TOT = [a + b for a, b in zip(LH, LDH)]
i = min(range(len(TOT)), key=TOT.__getitem__)
c = Chart("MDL: the 'model size' of a symbolic solver (illustrative)", "rule-set complexity →",
          "bits", (0, 10), (0, 320))
c.axes([(0, "simple"), (5, ""), (10, "complex")], [0, 80, 160, 240, 320], xgrid=False)
c.line(xs, LH, S1)
c.line(xs, LDH, S2)
c.line(xs, TOT, S3, width=2.5)
c.dots([xs[i]], [TOT[i]], S3, r=6)
c.label(xs[i], TOT[i], "best rule-set (minimum total)", dx=0, dy=-14, anchor="middle")
c.label(10, LH[-1], "L(H): the rules", dx=-4, dy=18, anchor="end")
c.label(0.4, LDH[8], "L(D|H): exceptions / residual", dx=8, dy=0)
c.label(9, TOT[180], "total = L(H) + L(D|H)", dx=-4, dy=-16, anchor="end")
c.save("03_mdl.svg")

# 4) Neural vs symbolic (conceptual)
xs = [10 ** (i * 0.02) for i in range(201)]
c = Chart("Two different curves (conceptual, not measured)", "resources (parameters / search budget, log)",
          "error on the task (log)", (1, 1e4), (1e-4, 1.5), xlog=True, ylog=True)
c.axes([1, 10, 100, 1000, 10000], [1e-4, 1e-3, 1e-2, 1e-1, 1], xfmt=pow10, yfmt=pow10)
c.line(xs, [x ** -0.25 for x in xs], S1)
c.line([x for x in xs if x < 120] + [120, 120, 1e4], [x ** -0.02 for x in xs if x < 120] + [120 ** -0.02, 3e-4, 3e-4], S2)
c.vline(120)
c.label(5000, 5000 ** -0.25, "neural: smooth power law", dx=0, dy=-16, anchor="end")
c.label(130, 3e-4, "symbolic: the right rule found → ~0 error", dx=8, dy=-8)
c.label(100, 0.95, "flat: no correct rule yet", dx=-6, dy=-10, anchor="end")
c.save("04_neural_vs_symbolic.svg")

# 5) Max entropy of an ARC grid
s = list(range(1, 31))
bits = [k * k * math.log2(10) for k in s]
c = Chart("Maximum entropy of one ARC grid (uniformly random pixels)", "grid side length s (s×s grid)",
          "bits = s² · log₂(10)", (1, 30), (0, 3200))
c.axes([1, 5, 10, 15, 20, 25, 30], [0, 800, 1600, 2400, 3200])
c.line(s, bits, S1)
c.dots([10, 30], [bits[9], bits[29]], S1, r=5)
c.label(30, bits[29], "30×30 ≈ 2 990 bits ≈ 374 bytes", dx=-10, dy=4, anchor="end")
c.label(10, bits[9], "10×10 ≈ 332 bits", dx=-8, dy=-10, anchor="end")
c.save("05_arc_max_entropy.svg")

# 6) Two-part code for the copy3times task
raw_out = (6 + 6 + 9 + 18) * math.log2(10)   # all 4 output grids, pixel by pixel
rule = 16                                    # rough: pick 'repeat' tool + axis + count 3 among primitives
vals = [raw_out, rule, 0.5]
names = ["4 output grids\nstored raw", "the rule 'repeat 3× →'\n(paid once, estimate)", "each output\ngiven input + rule"]
c = Chart("Compression = understanding: 'line_simple_copy3times'", "",
          "bits needed", (0, 3), (0, 140))
c.axes([(0.5, names[0]), (1.5, names[1]), (2.5, names[2])], [0, 35, 70, 105, 140], xgrid=False)
for k, v in enumerate(vals):
    c.bar(k + 0.5, 90, max(v, 1), S1)
    c.label(k + 0.5, max(v, 1), "≈ 0 bits" if v < 1 else f"≈ {v:.0f} bits", dy=-8, anchor="middle")
c.save("06_conditional_bits.svg")
print("ok")

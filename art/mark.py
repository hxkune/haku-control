# SPDX-License-Identifier: GPL-3.0-only
"""The haku control mark: the h of the wordmark (a tall leaning spike, a pointed-arch shoulder with a thorn, spiked
feet) inside its orbit. Every shape is either a "blade" (a centreline with a width that swells and runs out to sharp
points) or an outline through smooth and sharp nodes, fitted to cubic Beziers, so the mark is clean at any size.

    python art/mark.py      writes ui/mark.svg (white, transparent) and art/mark.json (shapes for gen-icons.ps1)

The orbit passes in front of the letter below its long axis and behind it above; where it crosses, the shape
behind gets a gap (the same boolean steps in the SVG masks and in gen-icons.ps1). Small icon sizes use the mark
without the inner curl, made bolder by gen-icons.ps1."""
import json, math, os, sys
from bezfit import fit_loop

CX, CY, TILT = 128, 146, -16    # the orbit: centre and tilt
GAP = 7                         # gap where one shape passes over another
CURL = True                     # the inner curl of the wordmark (left out for small sizes)

ERR = 0.25   # curve fitting tolerance (viewBox units)

def outline(pts, cs):
    """a closed outline (points, corner indices) as fitted cubic Beziers"""
    out, idx = [], {}
    for i, q in enumerate(pts):
        if out and math.dist(out[-1], q) < 1e-3: idx[i] = len(out) - 1; continue
        idx[i] = len(out); out.append(q)
    while len(out) > 2 and math.dist(out[0], out[-1]) < 1e-3: out.pop()
    cs = sorted(set(min(idx[c], len(out) - 1) for c in cs))
    cv = fit_loop(out, cs, ERR)
    f = lambda q: f'{q[0]:.2f},{q[1]:.2f}'
    return 'M' + f(cv[0][0]) + ''.join(' C' + ' '.join(f(q) for q in c[1:]) for c in cv) + 'Z'


def catmull(pts, n=240):
    """centripetal Catmull-Rom through pts, n samples"""
    P = [pts[0]] + list(pts) + [pts[-1]]
    P[0] = (2 * pts[0][0] - pts[1][0], 2 * pts[0][1] - pts[1][1])
    P[-1] = (2 * pts[-1][0] - pts[-2][0], 2 * pts[-1][1] - pts[-2][1])
    segs = len(pts) - 1
    out = []
    for s in range(segs):
        p0, p1, p2, p3 = P[s], P[s + 1], P[s + 2], P[s + 3]
        def tj(ti, a, b): return ti + max(1e-6, math.dist(a, b)) ** 0.5
        t0 = 0; t1 = tj(t0, p0, p1); t2 = tj(t1, p1, p2); t3 = tj(t2, p2, p3)
        m = max(2, n // segs)
        for k in range(m if s < segs - 1 else m + 1):
            t = t1 + (t2 - t1) * k / m
            def lerp(a, b, ta, tb):
                if tb - ta < 1e-9: return a
                return tuple(((tb - t) * a[i] + (t - ta) * b[i]) / (tb - ta) for i in range(2))
            A1 = lerp(p0, p1, t0, t1); A2 = lerp(p1, p2, t1, t2); A3 = lerp(p2, p3, t2, t3)
            B1 = lerp(A1, A2, t0, t2); B2 = lerp(A2, A3, t1, t3)
            out.append(lerp(B1, B2, t1, t2))
    return out

def ellipse(cx, cy, rx, ry, rot, a0, a1, n=300):
    r = math.radians(rot); out = []
    for k in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * k / n)
        x, y = rx * math.cos(a), ry * math.sin(a)
        out.append((cx + x * math.cos(r) - y * math.sin(r), cy + x * math.sin(r) + y * math.cos(r)))
    return out

def profile(w, ti=.3, to=.3, ei=.8, eo=.8, w0=0, w1=0, peak=None):
    """width along the stroke (u = 0..1 by length): tapers in over ti, out over to; w0/w1 = blunt ends (fraction)"""
    def f(u):
        # sine easing: a sharp tip (exponent > 1: concave sides) and no kink where the taper ends
        a = 1 if u >= ti else w0 + (1 - w0) * math.sin(math.pi / 2 * u / ti) ** ei
        b = 1 if u <= 1 - to else w1 + (1 - w1) * math.sin(math.pi / 2 * (1 - u) / to) ** eo
        k = a * b
        if peak: k *= 1 + peak[1] * math.exp(-((u - peak[0]) / peak[2]) ** 2)
        return w * k
    return f

def blade(center, width, skew=0.0):
    """outline of a stroke along center with width(u); skew leans the width (pen angle feel)"""
    L = [0.0]
    for i in range(1, len(center)): L.append(L[-1] + math.dist(center[i - 1], center[i]))
    tot = L[-1] or 1
    left, right = [], []
    for i, p in enumerate(center):
        a = center[max(0, i - 1)]; b = center[min(len(center) - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]; d = math.hypot(dx, dy) or 1
        nx, ny = -dy / d, dx / d
        w = width(L[i] / tot) / 2
        wl, wr = w * (1 + skew), w * (1 - skew)
        left.append((p[0] + nx * wl, p[1] + ny * wl)); right.append((p[0] - nx * wr, p[1] - ny * wr))
    pts = left + right[::-1]
    return outline(pts, [0, len(left) - 1, len(left), len(pts) - 1])

def shape(nodes, n=48):
    """closed outline through nodes (x, y) or (x, y, 1) for a sharp corner; smooth (Catmull-Rom) in between"""
    cs = [i for i, q in enumerate(nodes) if len(q) > 2 and q[2]]
    P = [(q[0], q[1]) for q in nodes]
    out, corner = [], []
    for k in range(len(cs)):
        a, b = cs[k], cs[(k + 1) % len(cs)]
        run = [P[(a + i) % len(P)] for i in range(((b - a) % len(P) or len(P)) + 1)]
        seg = catmull(run, n * (len(run) - 1)) if len(run) > 2 else [run[0], run[1]]
        corner.append(len(out)); out += seg[:-1]
    return outline(out, corner)

def mark(bold=0.0):
    k = 1 + bold
    h = []
    # ascender + stem: a long leaning spike at the top, down through the orbit to a spiked foot
    h.append(shape([(72, 6, 1), (94, 56), (106, 110), (109, 160), (108, 208), (104, 230), (101, 252, 1),
                    (95, 228), (91, 180), (87, 118), (81, 62)]))
    # shoulder: a pointed arch out of the stem with a thorn at the top, down to a spiked foot
    h.append(shape([(106, 126, 1), (126, 106), (150, 84, 1), (170, 100), (183, 132), (187, 176), (190, 214), (196, 248, 1),
                    (175, 214), (171, 172), (166, 144), (155, 126), (143, 120, 1), (125, 132), (108, 152, 1)]))
    # orbit: one blade from a spike at the left, thick along the bottom left, thinning round the right and the top
    # to a spike at the top left; the part below its long axis passes in front of the letter, the rest behind it
    def w(u):
        base = 9 + 16 * math.exp(-((u - .24) / .2) ** 2)
        a = 1 if u >= .2 else math.sin(math.pi / 2 * u / .2) ** 1.45
        b = 1 if u <= .7 else math.sin(math.pi / 2 * (1 - u) / .3) ** 1.5
        return base * k * a * b
    O = blade(ellipse(CX, CY, 120, 56, TILT, 192, -142, 400), w)
    if CURL:   # the inner curl of the wordmark: a thin crescent inside the orbit at the bottom left
        O += ' ' + blade(ellipse(CX - 14, CY + 4, 88, 36, TILT, 205, 100, 200), profile(7 * k, ti=.45, to=.4, ei=1.4, eo=1.4))
    front, back = O, O
    return ' '.join(h), front, back

def svg(bold=0.0, fill='#fff', bg=None, gap=GAP):
    H, F, B = mark(bold)
    g = gap * (1 + bold * .6)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <defs>
    <path id="H" d="{H}"/><path id="F" d="{F}"/><path id="B" d="{B}"/>
    <mask id="mB" maskUnits="userSpaceOnUse" x="0" y="0" width="256" height="256"><rect width="256" height="256" fill="#fff"/><use href="#H" fill="#000" stroke="#000" stroke-width="{g * 2:.1f}" stroke-linejoin="round"/></mask>
    <clipPath id="cF"><rect x="-300" y="{CY - 1}" width="900" height="500" transform="rotate({TILT} {CX} {CY})"/></clipPath>
    <clipPath id="cB"><rect x="-300" y="{CY - 500}" width="900" height="500" transform="rotate({TILT} {CX} {CY})"/></clipPath>
    <mask id="mH" maskUnits="userSpaceOnUse" x="0" y="0" width="256" height="256"><rect width="256" height="256" fill="#fff"/><use href="#F" clip-path="url(#cF)" fill="#000" stroke="#000" stroke-width="{g * 2:.1f}" stroke-linejoin="round"/></mask>
  </defs>
  {f'<rect width="256" height="256" fill="{bg}"/>' if bg else ''}
  <g fill="{fill}" stroke="{fill}" stroke-width="{bold * 7:.1f}" stroke-linejoin="round">
  <g mask="url(#mB)"><use href="#B" clip-path="url(#cB)"/></g>
  <use href="#H" mask="url(#mH)"/>
  <use href="#F" clip-path="url(#cF)"/>
  </g>
</svg>'''

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    open(os.path.join(here, '..', 'ui', 'mark.svg'), 'w', newline='\n').write(svg())
    H, O, _ = mark()
    CURL = False
    _, Os, _ = mark()
    json.dump({'cx': CX, 'cy': CY, 'tilt': TILT, 'gap': GAP, 'h': H, 'orbit': O, 'orbit_small': Os},
              open(os.path.join(here, 'mark.json'), 'w'), indent=1)
    print('wrote ui/mark.svg, art/mark.json')

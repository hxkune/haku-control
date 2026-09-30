# SPDX-License-Identifier: GPL-3.0-only
"""Cubic Bezier fitting (Philip J. Schneider, "An Algorithm for Automatically Fitting Digitized Curves",
Graphics Gems, 1990): a run of points between two sharp corners becomes as few smooth curves as the error allows."""
import math

def sub(a, b): return (a[0] - b[0], a[1] - b[1])
def add(a, b): return (a[0] + b[0], a[1] + b[1])
def mul(a, s): return (a[0] * s, a[1] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1]
def norm(a):
    d = math.hypot(*a) or 1; return (a[0] / d, a[1] / d)
def bez(c, t):
    mt = 1 - t
    return add(add(mul(c[0], mt ** 3), mul(c[1], 3 * mt * mt * t)), add(mul(c[2], 3 * mt * t * t), mul(c[3], t ** 3)))
def bez1(c, t):
    mt = 1 - t
    return add(add(mul(sub(c[1], c[0]), 3 * mt * mt), mul(sub(c[2], c[1]), 6 * mt * t)), mul(sub(c[3], c[2]), 3 * t * t))
def bez2(c, t):
    return add(mul(add(sub(c[2], mul(c[1], 2)), c[0]), 6 * (1 - t)), mul(add(sub(c[3], mul(c[2], 2)), c[1]), 6 * t))

def chord(P):
    u = [0]
    for i in range(1, len(P)): u.append(u[-1] + math.dist(P[i], P[i - 1]))
    return [x / (u[-1] or 1) for x in u]

def gen_bez(P, u, t1, t2):
    A = [(mul(t1, 3 * (1 - x) ** 2 * x), mul(t2, 3 * (1 - x) * x * x)) for x in u]
    C = [[0, 0], [0, 0]]; X = [0, 0]
    for i, x in enumerate(u):
        C[0][0] += dot(A[i][0], A[i][0]); C[0][1] += dot(A[i][0], A[i][1]); C[1][1] += dot(A[i][1], A[i][1])
        tmp = sub(P[i], bez((P[0], P[0], P[-1], P[-1]), x))
        X[0] += dot(A[i][0], tmp); X[1] += dot(A[i][1], tmp)
    C[1][0] = C[0][1]
    det = C[0][0] * C[1][1] - C[0][1] * C[1][0]
    a1 = (X[0] * C[1][1] - X[1] * C[0][1]) / det if det else 0
    a2 = (C[0][0] * X[1] - C[1][0] * X[0]) / det if det else 0
    seg = math.dist(P[0], P[-1]); eps = 1e-6 * seg
    if a1 < eps or a2 < eps: a1 = a2 = seg / 3
    return (P[0], add(P[0], mul(t1, a1)), add(P[-1], mul(t2, a2)), P[-1])

def reparam(c, P, u):
    out = []
    for p, x in zip(P, u):
        d = sub(bez(c, x), p); d1 = bez1(c, x); d2 = bez2(c, x)
        num = dot(d, d1); den = dot(d1, d1) + dot(d, d2)
        out.append(x - num / den if den else x)
    return out

def max_err(c, P, u):
    best = 0; idx = len(P) // 2
    for i, (p, x) in enumerate(zip(P, u)):
        e = math.dist(bez(c, x), p) ** 2
        if e > best: best, idx = e, i
    return best, idx

def fit(P, t1, t2, err):
    if len(P) == 2:
        d = math.dist(P[0], P[1]) / 3
        return [(P[0], add(P[0], mul(t1, d)), add(P[1], mul(t2, d)), P[1])]
    u = chord(P); c = gen_bez(P, u, t1, t2); e, s = max_err(c, P, u)
    if e < err: return [c]
    if e < err * 4:
        for _ in range(20):
            u = reparam(c, P, u); c = gen_bez(P, u, t1, t2); e, s = max_err(c, P, u)
            if e < err: return [c]
    s = max(1, min(len(P) - 2, s))
    tc = norm(sub(P[s - 1], P[s + 1]))
    return fit(P[:s + 1], t1, tc, err) + fit(P[s:], mul(tc, -1), t2, err)

def fit_loop(p, cs, err=0.9):
    n = len(p)
    if not cs: cs = [0, n // 2]
    cs = sorted(cs)
    curves = []
    for k in range(len(cs)):
        a, b = cs[k], cs[(k + 1) % len(cs)]
        run = [p[(a + i) % n] for i in range(((b - a) % n or n) + 1)]
        if len(run) < 2: continue
        t1 = norm(sub(run[min(3, len(run) - 1)], run[0])); t2 = norm(sub(run[max(0, len(run) - 4)], run[-1]))
        if len(run) < 4: t1 = norm(sub(run[-1], run[0])); t2 = mul(t1, -1)
        curves += fit(run, t1, t2, err * err)
    return curves

def svg_path(curves, ox=0, oy=0):
    f = lambda p: f'{p[0] + ox:.1f},{p[1] + oy:.1f}'
    s = 'M' + f(curves[0][0])
    for c in curves: s += ' C' + ' '.join(f(q) for q in c[1:])
    return s + 'Z'


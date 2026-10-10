"""A model of the background, fitted to an integrated table, and the table
filtered by it: a prototype (docs/backstop.md).

The background under the reflections is scatter from air and from what holds
the sample -- water, nylon -- smooth with angle, and the reflections whose
background does not follow it are in the backstop shadow (low) or its flare
(high). The model, per pixel and image:

    B = R(s) . G(phi) . P . Omega . Q

R(s), s = 1/d, and G(phi), the rotation, are cubic B-splines -- G periodic over
a full turn -- penalised for roughness so that R's smoothness carries it
through the innermost resolutions, where nearly every reflection is the
backstop's. P, the polarisation, Omega, the pixel's solid angle, and Q, the
sensor's efficiency, are computed from the experiment list, not fitted. In log
space the model is linear; it is fitted by iteratively reweighted least squares
with Tukey's biweight, each reflection weighted by its background's counting
variance -- 1 / (mean x pixels) in log space -- and an intrinsic spread
estimated from the residuals, so that the shadow and the flare do not pull it.

Each reflection's z is how many standard deviations its log background lies
from the model's. Beyond --z-max, it is flagged: its integrated flags cleared
and excluded-for-scaling set in the filtered table, so that neither mxi_scale
nor dials.scale takes it. background.expected and background.z are added to the
table for every reflection. Shells whose z spread far beyond 1 are reported, and
with --reject-shells left out whole.

    mxeq background-model integrated.expt integrated.refl -o filtered.refl
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field

import numpy as np
import scipy.sparse as sp
from scipy.interpolate import BSpline

from . import refl

INTEGRATED_SUM = 1 << 8
INTEGRATED_PRF = 1 << 9
EXCLUDED_FOR_SCALING = 1 << 24


def _basis(x, lo, hi, intervals, periodic=False):
    """A cubic B-spline basis over [lo, hi] at x, sparse; and its second
    difference penalty matrix."""
    k = 3
    if periodic:
        period = hi - lo
        h = period / intervals
        t = lo + h * np.arange(-k, intervals + k + 1)
        xm = lo + np.mod(x - lo, period)
        full = BSpline.design_matrix(xm, t, k)  # intervals + k columns
        fold = sp.csr_matrix(
            (
                np.ones(intervals + k),
                (np.arange(intervals + k), np.arange(intervals + k) % intervals),
            ),
            shape=(intervals + k, intervals),
        )
        b = (full @ fold).tocsr()
        n = intervals
        d = np.zeros((n, n))  # circular second differences
        for i in range(n):
            d[i, i] += 1.0
            d[i, (i + 1) % n] -= 2.0
            d[i, (i + 2) % n] += 1.0
        return b, d
    t = np.r_[[lo] * k, np.linspace(lo, hi, intervals + 1), [hi] * k]
    xc = np.clip(x, lo, hi)
    b = BSpline.design_matrix(xc, t, k).tocsr()
    n = intervals + k
    d = np.diff(np.eye(n), n=2, axis=0)
    return b, d


@dataclass
class Geometry:
    """What the experiment list says that the model computes rather than fits."""

    origin: np.ndarray
    fast: np.ndarray
    slow: np.ndarray
    pixel: tuple[float, float]
    direction: np.ndarray  # the beam, towards the source
    normal: np.ndarray  # the polarisation plane's normal
    fraction: float
    mu: float
    thickness: float
    phi_start: float  # degrees, at the scan's first image's start, z = 0
    phi_width: float  # degrees an image


def geometry(expt_path: str) -> Geometry:
    from mxeq import rflx

    e = rflx.experiments_dict(expt_path)
    b, p = e["beam"][0], e["detector"][0]["panels"][0]
    osc = e["scan"][0]["properties"]["oscillation"] if e.get("scan") else [0.0, 0.0]
    width = osc[1] - osc[0] if len(osc) > 1 else 0.0
    return Geometry(
        np.array(p["origin"], float),
        np.array(p["fast_axis"], float) / np.linalg.norm(p["fast_axis"]),
        np.array(p["slow_axis"], float) / np.linalg.norm(p["slow_axis"]),
        tuple(p["pixel_size"]),
        np.array(b["direction"], float) / np.linalg.norm(b["direction"]),
        np.array(b.get("polarization_normal", [0.0, 1.0, 0.0]), float),
        float(b.get("polarization_fraction", 0.999)),
        float(p.get("mu", 0.0)),
        float(p.get("thickness", 0.0)),
        float(osc[0]),
        width,
    )


def known_factors(g: Geometry, x_px, y_px):
    """The polarisation, the solid angle (relative) and the sensor's efficiency
    at each pixel position, and the azimuth there."""
    pos = (
        g.origin
        + np.outer(x_px * g.pixel[0], g.fast)
        + np.outer(y_px * g.pixel[1], g.slow)
    )
    r = np.linalg.norm(pos, axis=1)
    u = pos / r[:, None]
    e1 = np.cross(g.normal, g.direction)
    e1 /= np.linalg.norm(e1)
    e2 = np.cross(g.direction, e1)
    p = g.fraction * (1.0 - (u @ e1) ** 2) + (1.0 - g.fraction) * (1.0 - (u @ e2) ** 2)
    n = np.cross(g.fast, g.slow)
    cos_a = np.abs(u @ n)
    omega = cos_a / r**2
    omega = omega / np.median(omega)
    if g.mu > 0 and g.thickness > 0:
        q = 1.0 - np.exp(-g.mu * g.thickness / np.maximum(cos_a, 1e-6))
        q = q / np.median(q)
    else:
        q = np.ones_like(r)
    return p, omega, q


@dataclass
class Fit:
    expected: np.ndarray  # counts a pixel, every row (NaN where nothing to say)
    z: np.ndarray  # every row
    used: np.ndarray  # the rows the fit could use
    tau: float  # the intrinsic spread, in log space
    s_curve: tuple[np.ndarray, np.ndarray]  # s, R(s)
    phi_curve: tuple[np.ndarray, np.ndarray]  # phi, G(phi)
    iterations: int
    # Per row, for drawing: s, phi, the computed factors P . Omega . Q, R(s), G(phi).
    s: np.ndarray = field(default_factory=lambda: np.zeros(0))
    phi: np.ndarray = field(default_factory=lambda: np.zeros(0))
    known: np.ndarray = field(default_factory=lambda: np.zeros(0))
    r_row: np.ndarray = field(default_factory=lambda: np.zeros(0))
    g_row: np.ndarray = field(default_factory=lambda: np.zeros(0))
    flagged_low: np.ndarray = field(default_factory=lambda: np.zeros(0, bool))
    flagged_high: np.ndarray = field(default_factory=lambda: np.zeros(0, bool))
    flagged_shell: np.ndarray = field(
        default_factory=lambda: np.zeros(0, bool)
    )  # by --reject-shells
    high_removed: bool = False  # whether the too-high were taken out, --reject-high


def fit(
    g: Geometry,
    d,
    bg,
    n_pix,
    x_px,
    y_px,
    phi_deg,
    knots_s: int = 30,
    phi_spacing: float = 10.0,
    smoothness: float = 100.0,
    iterations: int = 10,
) -> Fit:
    rows = len(d)
    s = np.where(d > 0, 1.0 / d, np.nan)
    p, omega, q = known_factors(g, x_px, y_px)
    known = p * omega * q
    used = np.isfinite(s) & np.isfinite(bg) & (bg > 0) & (n_pix >= 10) & (known > 0)
    s_lo, s_hi = np.nanmin(s[used]), np.nanmax(s[used])
    phi_lo, phi_hi = np.nanmin(phi_deg), np.nanmax(phi_deg)
    span = phi_hi - phi_lo
    periodic = span >= 355.0
    if periodic:
        phi_lo, phi_hi = phi_lo, phi_lo + 360.0
    intervals_phi = max(1, int(round((phi_hi - phi_lo) / phi_spacing)))
    br, dr = _basis(s[used], s_lo, s_hi, knots_s)
    bgm, dg = _basis(phi_deg[used], phi_lo, phi_hi, intervals_phi, periodic)
    bgm = bgm[:, :-1]  # the constant is R's: G loses one degree
    dg = dg[:, :-1]
    x = sp.hstack([br, bgm]).tocsr()
    y = np.log(bg[used]) - np.log(known[used])
    var_count = 1.0 / (bg[used] * n_pix[used])
    nr, ng = br.shape[1], bgm.shape[1]
    penalty = np.zeros((nr + ng, nr + ng))
    tau = 0.05
    robust = np.ones(len(y))
    beta = np.zeros(nr + ng)
    for _ in range(iterations):
        w = robust / (var_count + tau**2)
        lam = smoothness * np.median(w)
        penalty[:nr, :nr] = lam * dr.T @ dr
        penalty[nr:, nr:] = lam * dg.T @ dg
        xw = x.multiply(w[:, None]).tocsr()
        a = (x.T @ xw).toarray() + penalty
        beta = np.linalg.solve(a + 1e-9 * np.eye(len(a)), xw.T @ y)
        resid = y - x @ beta
        mad = 1.4826 * np.median(np.abs(resid - np.median(resid)))
        tau = float(np.sqrt(max(mad**2 - np.median(var_count), 1e-6)))
        zz = resid / np.sqrt(var_count + tau**2)
        u = zz / 4.685
        robust = np.where(np.abs(u) < 1.0, (1.0 - u**2) ** 2, 0.0)

    # Every row: the model where the row has a resolution and a position.
    have = np.isfinite(s) & (known > 0)
    br_all, _ = _basis(np.where(have, s, s_lo), s_lo, s_hi, knots_s)
    bg_all, _ = _basis(
        np.where(np.isfinite(phi_deg), phi_deg, phi_lo),
        phi_lo,
        phi_hi,
        intervals_phi,
        periodic,
    )
    log_r = br_all @ beta[:nr]
    log_g = bg_all[:, :-1] @ beta[nr:]
    log_model = log_r + log_g
    expected = np.where(have, np.exp(log_model) * known, np.nan)
    z = np.full(rows, np.nan)
    with np.errstate(divide="ignore", invalid="ignore"):
        valid = have & (n_pix > 0) & np.isfinite(bg)
        zero = valid & (bg <= 0)
        positive = valid & (bg > 0)
        z[positive] = (np.log(bg[positive]) - np.log(expected[positive])) / np.sqrt(
            1.0 / (bg[positive] * n_pix[positive]) + tau**2
        )
        z[zero] = -np.inf
    grid_s = np.linspace(s_lo, s_hi, 200)
    rs, _ = _basis(grid_s, s_lo, s_hi, knots_s)
    grid_phi = np.linspace(phi_lo, phi_hi, 200, endpoint=not periodic)
    gs, _ = _basis(grid_phi, phi_lo, phi_hi, intervals_phi, periodic)
    return Fit(
        expected,
        z,
        used,
        tau,
        (grid_s, np.exp(rs @ beta[:nr])),
        (grid_phi, np.exp(gs[:, :-1] @ beta[nr:])),
        iterations,
        s=s,
        phi=phi_deg,
        known=known,
        r_row=np.exp(log_r),
        g_row=np.exp(log_g),
    )


@dataclass
class Result:
    table: refl.ReflectionTable
    fit: Fit
    report: str


def run(
    expt_path: str,
    refl_path: str,
    z_max: float = 5.0,
    reject_shells: float | None = None,
    shells: int = 12,
    annotate_only: bool = False,
    reject_high: bool = False,
    reject_inner: float | None = None,
    inner_width: float = 0.005,
    inner_min: int = 20,
    **options,
) -> Result:
    g = geometry(expt_path)
    t = refl.load(refl_path)
    c = t.columns
    for name in ("background.mean", "num_pixels.background", "d", "xyzcal.px", "flags"):
        if name not in c:
            raise ValueError(f"{refl_path} has no {name}: is it integrated?")
    bg = np.asarray(c["background.mean"], float).ravel()
    n_pix = np.asarray(c["num_pixels.background"], float).ravel()
    d = np.asarray(c["d"], float).ravel()
    px = np.asarray(c["xyzcal.px"], float).reshape(-1, 3)
    if "xyzcal.mm" in c:
        phi = np.degrees(np.asarray(c["xyzcal.mm"], float).reshape(-1, 3)[:, 2])
    else:
        phi = g.phi_start + px[:, 2] * g.phi_width
    f = fit(g, d, bg, n_pix, px[:, 0], px[:, 1], phi, **options)
    low = np.isfinite(f.z) & (f.z < -z_max) | np.isneginf(f.z)
    high = np.isfinite(f.z) & (f.z > z_max)

    # The resolution shells: the z's robust spread, and whether to leave out.
    s = np.where(d > 0, 1.0 / d, np.nan)
    have = np.isfinite(f.z) | np.isneginf(f.z)
    edges = np.quantile(s[np.isfinite(f.z)], np.linspace(0, 1, shells + 1))
    shell_rows = []
    whole = np.zeros(len(d), bool)
    lines = [
        f"{t.nrows} reflections, {int(f.used.sum())} with a background the fit could use; intrinsic "
        f"spread {100 * f.tau:.1f} per cent",
        f"beyond |z| {z_max:g}: {int(low.sum())} with a background too low, {int(high.sum())} too high"
        " -- the backstop shadow among the low and its flare among the high, but not only;"
        + (
            " both taken out"
            if reject_high
            else " the too-low taken out, the too-high kept (--reject-high)"
        ),
        "",
        f"  {'d (A)':>15} {'n':>8} {'obs/model':>9} {'z spread':>8} {'low %':>7} {'high %':>7}",
    ]
    for k in range(shells):
        sel = (
            have
            & (s >= edges[k])
            & ((s <= edges[k + 1]) if k == shells - 1 else (s < edges[k + 1]))
        )
        if not sel.any():
            continue
        zs = f.z[sel & np.isfinite(f.z)]
        spread = 1.4826 * np.median(np.abs(zs - np.median(zs))) if len(zs) else np.nan
        ratio = np.nanmedian(bg[sel] / f.expected[sel])
        dd = d[sel]
        mark = ""
        if reject_shells is not None and spread > reject_shells:
            whole |= sel
            mark = "  left out"
        lines.append(
            f"  {dd.max():6.2f} -{dd.min():7.2f} {sel.sum():8d} {ratio:9.3f} {spread:8.2f} "
            f"{100 * low[sel].mean():6.2f}% {100 * high[sel].mean():6.2f}%{mark}"
        )
        shell_rows.append((dd.max(), dd.min(), spread))
    g_lo, g_hi = float(np.min(f.phi_curve[1])), float(np.max(f.phi_curve[1]))
    lines += [
        "",
        f"  G(phi), the rotation, from {g_lo:.3f} to {g_hi:.3f} of its mean over "
        f"{f.phi_curve[0][0]:.1f} to {f.phi_curve[0][-1]:.1f} degrees",
        "  z spread is 1.4826 times the median absolute deviation of z: 1 where the model",
        "  and counting explain the backgrounds, more where they do not -- a shell far",
        "  beyond 1 is one whose backgrounds cannot be trusted.",
    ]
    # Only the too-low by default: on ferritin, against their equivalents, those
    # below z -5 were 7 to 16 per cent low -- attenuated -- where those above 5,
    # the flare, the module edges, the rings, were 1 to 1.5 per cent: unusual in
    # background, nearly right in intensity. --reject-high takes them too.
    # The innermost, judged whole: beside the backstop every background is the
    # flare's or the shadow's, the model's normal there their average, and z
    # cannot tell the unharmed from the attenuated -- one attenuated to the
    # model's level passes. So, outward from the lowest resolution in fine
    # shells of equal width in 1/d (merged until each holds inner_min), each
    # shell goes whole while its z spread exceeds reject_inner; the first that
    # does not stops it. The data choose --d-max.
    inner = np.zeros(len(d), bool)
    inner_limit = None
    if reject_inner is not None:
        finite_s = s[np.isfinite(s)]
        lo = float(np.min(finite_s))
        zz = np.where(np.isneginf(f.z), -100.0, f.z)
        start = lo
        while start < float(np.max(finite_s)):
            stop = start + inner_width
            sel = (s >= start) & (s < stop) & np.isfinite(zz)
            while sel.sum() < inner_min and stop < float(np.max(finite_s)):
                stop += inner_width
                sel = (s >= start) & (s < stop) & np.isfinite(zz)
            zs = zz[sel]
            spread = 1.4826 * np.median(np.abs(zs - np.median(zs))) if len(zs) else 0.0
            if spread <= reject_inner:
                break
            inner |= np.isfinite(s) & (s >= start) & (s < stop)
            inner_limit = (1.0 / stop, spread)
            start = stop
    whole = whole | inner
    flagged = low | (high if reject_high else np.zeros_like(high)) | whole
    if not annotate_only:
        flags = np.asarray(c["flags"]).astype(np.int64).copy()
        flags[flagged] = (
            flags[flagged] & ~(INTEGRATED_SUM | INTEGRATED_PRF)
        ) | EXCLUDED_FOR_SCALING
        c["flags"] = flags.astype(np.asarray(c["flags"]).dtype)
    c["background.expected"] = np.nan_to_num(f.expected, nan=-1.0)
    t.types["background.expected"] = "double"
    c["background.z"] = np.where(np.isneginf(f.z), -1e30, np.nan_to_num(f.z, nan=0.0))
    t.types["background.z"] = "double"
    f.flagged_low, f.flagged_high = low, high
    f.flagged_shell = whole & ~(low | high)
    f.high_removed = reject_high
    if reject_inner is not None:
        lines += [
            "",
            (
                f"  the innermost left out to d {inner_limit[0]:.2f} A, {int(inner.sum())} reflections, "
                f"where the background's z spread exceeded {reject_inner:g}"
                if inner_limit
                else f"  the innermost kept: its first shell's z spread is within {reject_inner:g}"
            ),
        ]
    # Where the too-high cluster in resolution, in fine bins: a ring too narrow
    # for R's spline -- ice, at 3.90, 3.67, 3.44, 2.67, 2.25 A and so on -- piles up.
    lines += [
        "",
        "  where the too-high cluster in resolution, the fine bins richest in them:",
    ]
    fine = np.linspace(np.nanmin(s[have]), np.nanmax(s[have]), 201)
    total, _ = np.histogram(s[have], fine)
    hits, _ = np.histogram(s[high], fine)
    share = np.where(total >= 20, hits / np.maximum(total, 1), 0.0)
    for k in np.argsort(-share)[:8]:
        if hits[k] == 0:
            break
        lines.append(
            f"    d {1 / fine[k + 1]:6.3f} - {1 / fine[k]:6.3f} A: {hits[k]:6d} of {total[k]:7d}, "
            f"{100 * share[k]:5.1f}%"
        )
    shells_note = (
        f", {int((whole & ~(low | high)).sum())} of them by whole shells"
        if whole.any()
        else ""
    )
    lines.insert(
        2,
        (
            f"{int(flagged.sum())} flagged{shells_note}; --annotate-only, so the table's flags are as they were"
            if annotate_only
            else f"{int(flagged.sum())} reflections' integrated flags cleared in the filtered table{shells_note}"
        ),
    )
    return Result(t, f, "\n".join(lines))

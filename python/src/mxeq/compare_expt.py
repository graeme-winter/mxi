"""Two experiment lists' models side by side: what differs, and by how much.

For making mxi_import agree with dials.import across a population of data
sets: run both on the same master and compare. Each model is compared on what
processing uses -- the beam's wavelength and direction; each panel's origin,
axes, pixel and image size, trusted range, sensor; the goniometer's axes and
angles; the scan's images and angles -- and a line is printed for each, "same"
within tolerances small enough that a difference means something, or the
difference.

    mxeq compare-expt imported.expt dials_imported.expt
"""

from __future__ import annotations

import json

import numpy as np

TOLERANCE = {
    "length": 1e-3,  # mm
    "angle": 1e-4,  # degrees
    "relative": 1e-6,
}


def _angle(a, b) -> float:
    a = np.asarray(a, float)
    b = np.asarray(b, float)
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    if na == 0 or nb == 0:
        return float("nan")
    return float(np.degrees(np.arccos(np.clip(np.dot(a, b) / (na * nb), -1.0, 1.0))))


class Comparison:
    def __init__(self):
        self.lines: list[str] = []
        self.differences = 0

    def same(self, what: str, a, b, kind: str, unit: str = ""):
        """Compare two numbers or vectors of a kind: length (mm apart), angle
        (degrees between them), relative, or exact."""
        if kind == "exact":
            ok = a == b
            text = "same" if ok else f"{a!r} against {b!r}"
        elif kind == "angle-between":
            d = _angle(a, b)
            ok = d <= TOLERANCE["angle"]
            text = (
                "same"
                if ok
                else f"{d:.4g} deg apart: {np.round(a, 6).tolist()} against {np.round(b, 6).tolist()}"
            )
        elif kind == "length":
            d = float(np.linalg.norm(np.asarray(a, float) - np.asarray(b, float)))
            ok = d <= TOLERANCE["length"]
            text = (
                "same"
                if ok
                else f"{d:.4g} mm apart: {np.round(a, 4).tolist()} against {np.round(b, 4).tolist()}"
            )
        elif kind == "angle":
            d = (
                float(np.max(np.abs(np.asarray(a, float) - np.asarray(b, float))))
                if np.size(a)
                else 0.0
            )
            ok = d <= TOLERANCE["angle"]
            text = "same" if ok else f"differ by up to {d:.4g} deg"
        else:  # relative
            a_, b_ = np.asarray(a, float), np.asarray(b, float)
            scale = np.maximum(np.abs(b_), 1e-30)
            d = float(np.max(np.abs(a_ - b_) / scale)) if a_.size else 0.0
            ok = d <= TOLERANCE["relative"]
            if ok:
                text = "same"
            elif a_.size > 3:
                # A long array summarised: its first values, and the worst.
                text = (
                    f"first {a_.ravel()[0]:.6g} against {b_.ravel()[0]:.6g}, "
                    f"{d:.3g} relative at worst{', ' + unit if unit else ''}"
                )
            else:
                text = (
                    f"{np.round(a_, 6).tolist()} against {np.round(b_, 6).tolist()} "
                    f"({d:.3g} relative{', ' + unit if unit else ''})"
                )
        if not ok:
            self.differences += 1
        self.lines.append(f"  {what:<34} {text}")

    def say(self, line: str):
        self.lines.append(line)


def _model(doc: dict, kind: str, i: int):
    """Experiment i's model of a kind, through its own index -- several sweeps
    each have their own."""
    experiments = doc.get("experiment", [])
    index = experiments[i].get(kind, 0) if i < len(experiments) else 0
    return doc[kind][index]


def compare(
    a: dict, b: dict, label_a: str = "first", label_b: str = "second", i: int = 0
) -> Comparison:
    c = Comparison()
    c.say(f"{label_a} against {label_b}")
    ba, bb = _model(a, "beam", i), _model(b, "beam", i)
    c.say("beam")
    c.same("wavelength", ba["wavelength"], bb["wavelength"], "relative", "A")
    c.same("direction", ba["direction"], bb["direction"], "angle-between")
    c.same(
        "polarization fraction",
        ba.get("polarization_fraction"),
        bb.get("polarization_fraction"),
        "relative",
    )
    pa, pb = _model(a, "detector", i)["panels"], _model(b, "detector", i)["panels"]
    c.say("detector")
    c.same("panels", len(pa), len(pb), "exact")
    for k, (x, y) in enumerate(zip(pa, pb)):
        c.say(f" panel {k}")
        c.same("origin", x["origin"], y["origin"], "length")
        c.same("fast axis", x["fast_axis"], y["fast_axis"], "angle-between")
        c.same("slow axis", x["slow_axis"], y["slow_axis"], "angle-between")
        c.same("image size", list(x["image_size"]), list(y["image_size"]), "exact")
        c.same(
            "raw image offset",
            list(x.get("raw_image_offset", [0, 0])),
            list(y.get("raw_image_offset", [0, 0])),
            "exact",
        )
        c.same("pixel size", x["pixel_size"], y["pixel_size"], "relative", "mm")
        c.same("trusted range", x["trusted_range"], y["trusted_range"], "relative")
        c.same("thickness", x["thickness"], y["thickness"], "relative", "mm")
        c.same("material", x["material"], y["material"], "exact")
        c.same("mu", x["mu"], y["mu"], "relative", "1/mm")
        c.same(
            "px to mm",
            x.get("px_mm_strategy", {}).get("type"),
            y.get("px_mm_strategy", {}).get("type"),
            "exact",
        )
    ga, gb = _model(a, "goniometer", i), _model(b, "goniometer", i)
    c.say("goniometer")
    axes_a = ga.get("axes") or [ga.get("rotation_axis")]
    axes_b = gb.get("axes") or [gb.get("rotation_axis")]
    c.same("axes", len(axes_a), len(axes_b), "exact")
    for k, (x, y) in enumerate(zip(axes_a, axes_b)):
        name = (ga.get("names") or ["axis"] * len(axes_a))[k]
        c.same(f"axis {k} ({name})", x, y, "angle-between")
    c.same("names", ga.get("names"), gb.get("names"), "exact")
    c.same("scan axis", ga.get("scan_axis"), gb.get("scan_axis"), "exact")
    c.same("angles", ga.get("angles", []), gb.get("angles", []), "angle")
    sa, sb = _model(a, "scan", i), _model(b, "scan", i)
    c.say("scan")
    c.same("image range", list(sa["image_range"]), list(sb["image_range"]), "exact")
    oa = np.asarray(sa["properties"]["oscillation"], float)
    ob = np.asarray(sb["properties"]["oscillation"], float)
    c.same("images", len(oa), len(ob), "exact")
    if len(oa) == len(ob) and len(oa):
        c.same("each image's start angle", oa, ob, "angle")
        if len(oa) > 1:
            c.same("oscillation width", oa[1] - oa[0], ob[1] - ob[0], "angle")
    ea = sa["properties"].get("exposure_time", [])
    eb = sb["properties"].get("exposure_time", [])
    if len(ea) == len(eb) and len(ea):
        c.same("exposure time", ea, eb, "relative", "s")
    ia, ib = _model(a, "imageset", i), _model(b, "imageset", i)
    c.say("image set")
    import os

    c.same(
        "template's file name",
        os.path.basename(ia.get("template", "")),
        os.path.basename(ib.get("template", "")),
        "exact",
    )
    c.say(f"{c.differences} difference{'s' if c.differences != 1 else ''}")
    return c


def compare_files(path_a: str, path_b: str) -> Comparison:
    """Experiment by experiment when both lists hold the same number -- several
    sweeps, each compared with its counterpart -- otherwise the first of each."""
    from mxeq import rflx

    a, b = rflx.experiments_dict(path_a), rflx.experiments_dict(path_b)
    na, nb = len(a.get("experiment", [])), len(b.get("experiment", []))
    if na != nb or na <= 1:
        c = compare(a, b, path_a, path_b)
        if na != nb:
            c.lines.insert(
                1, f"  {na} experiments against {nb}: the first of each compared"
            )
            c.differences += 1
        return c
    total = Comparison()
    for i in range(na):
        part = compare(a, b, f"{path_a} experiment {i}", f"{path_b} experiment {i}", i)
        total.lines += part.lines + [""]
        total.differences += part.differences
    total.say(
        f"{total.differences} difference{'s' if total.differences != 1 else ''} over {na} experiments"
    )
    return total

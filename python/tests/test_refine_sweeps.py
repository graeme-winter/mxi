"""mxi_refine on several sweeps unlike each other: each judged and modelled by
its own measure.

Graeme's four small-molecule sweeps, three of 350 degrees and one of 120 at
another goniometer setting: the short sweep started with larger residuals, and
outliers rejected against one spread pooled over all four left it 474 of some
3400 spots, its detector distance then free to slide 0.4 mm with its cell; and
the first sweep's 35 control points, given to it too, left it so free that one
minimiser could not settle it, and stopped the others early. Now each sweep's
outliers are judged by its own spread, as dials.refine's are, and each takes
its own number of control points, one per 10 degrees of its own scan.

The refined insulin fixture split into two sweeps: one of 75 images made noisy
-- 1 px on its observed positions -- must keep a share of its spots like the
clean one's; and one told its images are 0.5 degrees wide, a 75 degree scan,
takes its own 8 control points beside the other's 5. Needs MXI_REFINE,
MXI_POSTREFINE_EXPT and MXI_POSTREFINE_REFL.
"""

import copy
import json
import os
import re
import subprocess

import numpy as np
import pytest

from mxeq import refl

BINARY = os.environ.get("MXI_REFINE")
EXPT = os.environ.get("MXI_POSTREFINE_EXPT")
REFL = os.environ.get("MXI_POSTREFINE_REFL")
needs = pytest.mark.skipif(
    not (BINARY and EXPT and REFL),
    reason="set MXI_REFINE, MXI_POSTREFINE_EXPT, MXI_POSTREFINE_REFL",
)
SPLIT = 150


def halves(tmp_path, noise=0.0, wide_second=False, split=SPLIT):
    e = json.load(open(EXPT))
    n = e["scan"][0]["image_range"][1]
    out = copy.deepcopy(e)
    for k in ("beam", "detector", "goniometer", "scan", "imageset"):
        out[k] = [copy.deepcopy(e[k][0]), copy.deepcopy(e[k][0])]
    c0 = copy.deepcopy(e["crystal"][0])
    c0.pop("A_at_scan_points", None)
    out["crystal"] = [copy.deepcopy(c0), copy.deepcopy(c0)]
    out.pop("profile", None)
    out["experiment"] = []
    for i, (lo, hi) in enumerate(((0, split), (split, n))):
        s = out["scan"][i]
        s["image_range"] = [lo + 1, hi]
        for key, values in s["properties"].items():
            if isinstance(values, list) and len(values) == n:
                s["properties"][key] = values[lo:hi]
        if wide_second and i == 1:
            start = s["properties"]["oscillation"][0]
            s["properties"]["oscillation"] = [start + 0.5 * j for j in range(hi - lo)]
        out["imageset"][i]["single_file_indices"] = list(range(lo, hi))
        out["experiment"].append(
            {
                "__id__": "Experiment",
                "identifier": f"half-{i}",
                "beam": i,
                "detector": i,
                "goniometer": i,
                "scan": i,
                "imageset": i,
                "crystal": i,
            }
        )
    json.dump(out, open(tmp_path / "two.expt", "w"))
    t = refl.load(REFL)
    xyz = np.asarray(t.columns["xyzobs.px.value"], float).reshape(-1, 3).copy()
    second = xyz[:, 2] >= split
    t.columns["id"] = second.astype(np.int32)
    t.types["id"] = "int"
    t.identifiers = {0: "half-0", 1: "half-1"}
    if noise > 0:
        rng = np.random.default_rng(7)
        xyz[second, :2] += rng.normal(0.0, noise, size=(int(second.sum()), 2))
        t.columns["xyzobs.px.value"] = xyz
    refl.write(str(tmp_path / "two.refl"), t)


def refine(tmp_path, *extra):
    r = subprocess.run(
        [
            BINARY,
            "two.expt",
            "two.refl",
            "--analytic",
            "--output-expt",
            "refined.expt",
            "--output-refl",
            "refined.refl",
            *extra,
        ],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )
    assert r.returncode == 0, r.stderr + r.stdout[-1500:]
    return r.stdout


@needs
def test_a_sweep_that_starts_worse_keeps_its_share_of_spots(tmp_path):
    # A clean sweep of 225 images beside a noisy one of 75, 1 px on its
    # observed positions: pooled, the clean sweep set the spread, and the noisy
    # one kept 49 per cent of its spots to the clean one's 91; now 92 and 90.
    halves(tmp_path, noise=1.0, split=225)
    refine(tmp_path)
    c = refl.load(str(tmp_path / "refined.refl")).columns
    ids = np.asarray(c["id"]).ravel()
    used = (np.asarray(c["flags"]).ravel().astype(np.int64) & (1 << 3)) != 0
    kept = [used[ids == i].mean() for i in (0, 1)]
    # Pooled, the clean sweep's spread condemned most of the noisy one's.
    assert kept[1] > 0.7 * kept[0], kept


@needs
def test_each_sweep_takes_its_own_control_points(tmp_path):
    halves(tmp_path, wide_second=True)
    out = refine(tmp_path)
    m = re.search(r"Scan-varying, ([0-9, ]+) control points: (\d+) parameters", out)
    assert m, out[-1500:]
    assert m.group(1).strip() == "5, 8"
    models = json.load(open(tmp_path / "refined.expt"))
    points = [len(c.get("A_at_scan_points", [])) for c in models["crystal"]]
    # Written as samples at each image boundary, a sweep's own.
    assert points == [SPLIT + 1, 300 - SPLIT + 1]

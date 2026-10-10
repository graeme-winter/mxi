"""mxi_index and mxi_refine on several sweeps: indexed with one matrix, then
each sweep's crystal refined apart from it, as DIALS's protocol has it.

The refined 300 image insulin sweep's spots and models, the crystal taken away
so that the list is as imported, split at image 150 into two sweeps. Indexed:
one basis for both -- their orientations within a tenth of a degree, their
cells within a tenth of a per cent -- yet a crystal each, the last cycle
refining them apart; with --shared-crystal one throughout. Refined: a crystal
each by default, the residual no worse than one crystal shared gives. Needs
MXI_INDEX, MXI_REFINE, MXI_POSTREFINE_EXPT and MXI_POSTREFINE_REFL.
"""

import copy
import json
import os
import re
import subprocess

import numpy as np
import pytest

from mxeq import refl

INDEX = os.environ.get("MXI_INDEX")
REFINE = os.environ.get("MXI_REFINE")
EXPT = os.environ.get("MXI_POSTREFINE_EXPT")
REFL = os.environ.get("MXI_POSTREFINE_REFL")
needs = pytest.mark.skipif(
    not (INDEX and REFINE and EXPT and REFL),
    reason="set MXI_INDEX, MXI_REFINE, MXI_POSTREFINE_EXPT, MXI_POSTREFINE_REFL",
)
SPLIT = 150


def imported_halves(tmp_path):
    e = json.load(open(EXPT))
    n = e["scan"][0]["image_range"][1]
    out = copy.deepcopy(e)
    for k in ("beam", "detector", "goniometer", "scan", "imageset"):
        out[k] = [copy.deepcopy(e[k][0]), copy.deepcopy(e[k][0])]
    out["crystal"] = []
    out.pop("profile", None)
    out["experiment"] = []
    for i, (lo, hi) in enumerate(((0, SPLIT), (SPLIT, n))):
        s = out["scan"][i]
        s["image_range"] = [lo + 1, hi]
        for key, values in s["properties"].items():
            if isinstance(values, list) and len(values) == n:
                s["properties"][key] = values[lo:hi]
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
            }
        )
    json.dump(out, open(tmp_path / "two.expt", "w"))
    t = refl.load(REFL)
    z = np.asarray(t.columns["xyzobs.px.value"], float).reshape(-1, 3)[:, 2]
    t.columns["id"] = (z >= SPLIT).astype(np.int32)
    t.types["id"] = "int"
    t.identifiers = {0: "half-0", 1: "half-1"}
    refl.write(str(tmp_path / "two.refl"), t)


def run(tmp_path, *words):
    r = subprocess.run(list(words), capture_output=True, text=True, cwd=tmp_path)
    assert r.returncode == 0, r.stderr + r.stdout[-1500:]
    return r.stdout


def matrices(path):
    e = json.load(open(path))
    out = []
    for c in e["crystal"]:
        real = np.array([c["real_space_a"], c["real_space_b"], c["real_space_c"]])
        out.append(np.linalg.inv(real))
    return e, out


def rmsd(text):
    m = re.search(
        r"RMSD over those \d+: ([0-9.]+) px, ([0-9.]+) px, ([0-9.]+) images", text
    )
    assert m, text[-800:]
    return np.array([float(v) for v in m.groups()])


@needs
def test_indexed_in_one_basis_then_each_sweeps_crystal_refined_apart(tmp_path):
    imported_halves(tmp_path)
    out = run(
        tmp_path,
        INDEX,
        "two.expt",
        "two.refl",
        "--output-expt",
        "indexed.expt",
        "--output-refl",
        "indexed.refl",
    )
    assert "each sweep's crystal apart" in out
    m = re.search(r"Indexed (\d+) of (\d+)", out)
    assert int(m.group(1)) >= 0.95 * int(m.group(2))
    e, (a, b) = matrices(tmp_path / "indexed.expt")
    assert [x["crystal"] for x in e["experiment"]] == [0, 1]
    r = b @ np.linalg.inv(a)
    angle = np.degrees(np.arccos(np.clip((np.trace(r) - 1.0) / 2.0, -1.0, 1.0)))
    assert angle < 0.1, angle
    lengths = [np.linalg.norm(np.linalg.inv(x), axis=1) for x in (a, b)]
    assert np.allclose(lengths[0], lengths[1], rtol=1e-3)

    apart = rmsd(
        run(
            tmp_path,
            REFINE,
            "indexed.expt",
            "indexed.refl",
            "--output-expt",
            "apart.expt",
            "--output-refl",
            "apart.refl",
        )
    )
    e, crystals = matrices(tmp_path / "apart.expt")
    assert len(crystals) == 2

    (tmp_path / "shared").mkdir()
    shared_dir = tmp_path / "shared"
    for f in ("two.expt", "two.refl"):
        (shared_dir / f).write_bytes((tmp_path / f).read_bytes())
    out = run(
        shared_dir,
        INDEX,
        "two.expt",
        "two.refl",
        "--shared-crystal",
        "--output-expt",
        "indexed.expt",
        "--output-refl",
        "indexed.refl",
    )
    assert "each sweep's crystal apart" not in out
    e, crystals = matrices(shared_dir / "indexed.expt")
    assert len(crystals) == 1
    shared = rmsd(
        run(
            shared_dir,
            REFINE,
            "indexed.expt",
            "indexed.refl",
            "--shared-crystal",
            "--output-expt",
            "refined.expt",
            "--output-refl",
            "refined.refl",
        )
    )
    assert np.sum(apart**2) <= np.sum(shared**2) * 1.001, (apart, shared)

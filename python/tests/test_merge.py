"""mxi merge: a merged MTZ from scaled data, as dials.merge writes one (26
November). Run with MXI_SYMMETRY_DATA naming an integrated.rflx (insulin's,
say): scaled, merged, and the file read back."""

import os
import subprocess

import numpy as np
import pytest

gemmi = pytest.importorskip("gemmi")

BUILD = os.environ.get("MXI_BUILD")
MXI = os.path.join(BUILD, "mxi") if BUILD else None
DATA = os.environ.get("MXI_SYMMETRY_DATA")

pytestmark = pytest.mark.skipif(
    not (MXI and DATA), reason="set MXI_BUILD and MXI_SYMMETRY_DATA"
)


def run(*args, cwd):
    r = subprocess.run([MXI, *args], capture_output=True, text=True, cwd=cwd)
    assert r.returncode == 0, r.stderr[-2000:]
    return r.stdout


def test_the_merged_mtz_is_dials_merges_shape_and_self_consistent(tmp_path):
    run("scale", DATA, "-o", "scaled.rflx", cwd=tmp_path)
    run("merge", "scaled.rflx", "-o", "merged.mtz", cwd=tmp_path)
    m = gemmi.read_mtz_file(str(tmp_path / "merged.mtz"))
    labels = [c.label for c in m.columns]
    assert labels == [
        "H",
        "K",
        "L",
        "FreeR_flag",
        "IMEAN",
        "SIGIMEAN",
        "N",
        "F",
        "SIGF",
        "I(+)",
        "SIGI(+)",
        "I(-)",
        "SIGI(-)",
        "N(+)",
        "N(-)",
        "F(+)",
        "SIGF(+)",
        "F(-)",
        "SIGF(-)",
        "DANO",
        "SIGDANO",
    ]
    d = {c.label: np.array(c, copy=False) for c in m.columns}
    both = np.isfinite(d["I(+)"]) & np.isfinite(d["I(-)"])
    assert both.any()
    assert np.all(d["N"][both] == d["N(+)"][both] + d["N(-)"][both])
    w1, w2 = 1 / d["SIGI(+)"][both] ** 2, 1 / d["SIGI(-)"][both] ** 2
    mean = (w1 * d["I(+)"][both] + w2 * d["I(-)"][both]) / (w1 + w2)
    assert np.allclose(mean, d["IMEAN"][both], rtol=1e-4, atol=1e-3)
    strong = d["IMEAN"] / d["SIGIMEAN"] > 10
    ratio = np.nanmedian(d["F"][strong] ** 2 / d["IMEAN"][strong])
    assert 0.99 < ratio < 1.0, ratio
    assert np.nanmin(d["F"]) > 0
    free = np.mean(d["FreeR_flag"] == 0)
    assert 0.03 < free < 0.07, free
    dano = np.isfinite(d["DANO"])
    assert np.allclose(d["DANO"][dano], (d["F(+)"] - d["F(-)"])[dano], atol=1e-3)


def test_each_crystal_merged_alone(tmp_path):
    run("scale", DATA, "-o", "scaled.rflx", cwd=tmp_path)
    out = run("merge", "scaled.rflx", "--each", "-o", "per.mtz", cwd=tmp_path)
    assert "per_0.mtz" in out
    assert (tmp_path / "per_0.mtz").exists()


def _summary(text):
    """The summary's lines, Overall to Total unique; a Suggested column (mxi
    scale's, where it suggests a cut) dropped."""
    lines = text.splitlines()
    start = next(i for i, l in enumerate(lines) if "Summary of merging statistics" in l)
    end = next(
        i for i in range(start, len(lines)) if lines[i].startswith("Total unique")
    )
    block = [l for l in lines[start + 1 : end + 1] if l.strip()]
    suggested = "Suggested" in block[0]
    out = []
    for line in block:
        if suggested:
            head, _, last = line.rstrip().rpartition(" ")
            # A row with four values ends with the suggested one.
            if len(line[44:].split()) == 4:
                line = head
        out.append(line.rstrip())
    return out


def test_the_merge_reproduces_scalings_merging_statistics(tmp_path):
    """Graeme, 26 November: since we are merging it should at least reproduce
    the merging statistics -- the summary, Overall, Low and High, as mxi
    scale's from the same observations, line for line."""
    scaled = run("scale", DATA, "-o", "scaled.rflx", cwd=tmp_path)
    merged = run("merge", "scaled.rflx", "-o", "merged.mtz", cwd=tmp_path)
    summary = _summary(scaled)
    assert len(summary) > 15
    assert _summary(merged) == summary


def test_the_summary_describes_what_was_merged_within_the_limits(tmp_path):
    """Graeme, 27 November: with --d-min 1.3 the summary went to 1.14 A --
    every row scaling had -- while the file stopped at 1.3. The summary is
    of what was merged: its limits within those asked for, its totals the
    counts the line above it gives."""
    import re

    run("scale", DATA, "-o", "scaled.rflx", cwd=tmp_path)
    out = run(
        "merge",
        "scaled.rflx",
        "--d-min",
        "2.0",
        "--d-max",
        "20",
        "-o",
        "merged.mtz",
        cwd=tmp_path,
    )
    head = re.search(
        r"(\d+) observations merged to (\d+) reflections, ([\d.]+) to ([\d.]+) A",
        out,
    )
    observations, unique = int(head.group(1)), int(head.group(2))
    high = float(re.search(r"^High resolution limit\s+([\d.]+)", out, re.M).group(1))
    low = float(re.search(r"^Low resolution limit\s+([\d.]+)", out, re.M).group(1))
    assert high >= 2.0 and low <= 20.0, (high, low)
    total = int(re.search(r"^Total observations\s+(\d+)", out, re.M).group(1))
    total_unique = int(re.search(r"^Total unique\s+(\d+)", out, re.M).group(1))
    assert (total, total_unique) == (observations, unique)

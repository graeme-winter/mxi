"""Hot pixels (mxi find, 16 November): a pixel signal on a quarter of the
images or more, after Graeme's DIALS tool -- said always; with --mask-hot,
written to the image set's untrusted regions, so integration leaves it out.

Planted (mxeq.fixtures.nxmx --hot-pixels, 120 images): one pixel bright on
every image, one on every other (twinkling), one on the first tenth only."""

import json
import os
import subprocess
import sys

import pytest

from mxeq import rflx

BUILD = os.path.dirname(os.environ.get("MXI_FIND", "")) or os.environ.get(
    "MXI_BUILD", ""
)
FIND = os.path.join(BUILD, "mxi_find")

pytestmark = pytest.mark.skipif(not os.path.exists(FIND), reason="mxi is not built")

FRAMES = 120


@pytest.fixture
def series(tmp_path):
    subprocess.run(
        [
            sys.executable,
            "-m",
            "mxeq.fixtures.nxmx",
            str(tmp_path),
            str(FRAMES),
            "--hot-pixels",
        ],
        check=True,
        capture_output=True,
    )
    from mxeq.fixtures import nxmx

    nxmx.write_expt(str(tmp_path / "one.expt"), FRAMES)
    doc = json.load(open(tmp_path / "one.expt"))
    doc["experiment"][0]["imageset"] = 0
    doc["imageset"] = [
        {"__id__": "ImageSequence", "template": str(tmp_path / "series.nxs")}
    ]
    json.dump(doc, open(tmp_path / "l.expt", "w"))
    return tmp_path


def find(where, *extra):
    return subprocess.run(
        [
            FIND,
            str(where / "l.expt"),
            "-o",
            str(where / "strong.rflx"),
            "-j",
            "2",
            *extra,
        ],
        capture_output=True,
        text=True,
        cwd=where,
    )


def test_the_hot_are_said_and_not_masked_unasked(series):
    run = find(series)
    assert run.returncode == 0, run.stderr
    said = [l for l in run.stdout.splitlines() if l.startswith("Hot pixels")]
    assert said == [
        "Hot pixels: 2 on >= 25% of the 120 images (--hot-fraction): "
        "--mask-hot to exclude and mask them"
    ]
    assert "the most:" not in run.stdout  # with --verbose only
    sets = rflx.experiments_dict(str(series / "strong.rflx"))["imageset"]
    assert "untrusted" not in sets[0]


def test_verbose_lists_each_slow_then_fast(series):
    # Slow, then fast: a detector's own mask is m[slow, fast] -- Graeme marks
    # them there (18 November). Most first; the tenth-of-the-images one not.
    run = find(series, "--verbose")
    assert run.returncode == 0, run.stderr
    lines = run.stdout.splitlines()
    at = lines.index("     slow    fast  images")
    assert [l.split() for l in lines[at + 1 : at + 3]] == [
        ["4", "4", "100.0%"],
        ["250", "379", "50.0%"],
    ]
    assert "  40      44" not in run.stdout


def test_masked_they_are_the_image_sets_untrusted_pixels(series):
    run = find(series, "--mask-hot")
    assert run.returncode == 0, run.stderr
    assert "Hot pixels: 2 on >= 25% of the 120 images (--hot-fraction): masked" in (
        run.stdout
    )
    assert "Hot pixels masked: 2, in image set 0's untrusted regions" in run.stdout
    sets = rflx.experiments_dict(str(series / "strong.rflx"))["imageset"]
    regions = sets[0]["untrusted"]
    assert regions == [{"panel": 0, "pixels": [[4, 4], [379, 250]]}]
    # A second run on the masked file reads them as masked: no hot pixels
    # left to find, those two pixels no longer signal.
    again = subprocess.run(
        [
            FIND,
            str(series / "strong.rflx"),
            "-o",
            str(series / "again.rflx"),
            "-j",
            "2",
        ],
        capture_output=True,
        text=True,
        cwd=series,
    )
    assert again.returncode == 0, again.stderr
    assert "Untrusted regions: 2 single pixels, 2 pixels" in again.stdout
    assert "Hot pixels: none on >= 25% of the 120 images" in again.stdout


def test_a_lower_fraction_takes_the_occasional_one_too(series):
    run = find(series, "--hot-fraction", "0.1")
    assert run.returncode == 0, run.stderr
    assert "Hot pixels: 3 on >= 10% of the 120 images" in run.stdout


def test_a_short_series_is_not_judged(tmp_path):
    # Of 30 images a planted spot, 3 images wide, is a tenth of them: too few
    # to tell a hot pixel from a reflection.
    subprocess.run(
        [
            sys.executable,
            "-m",
            "mxeq.fixtures.nxmx",
            str(tmp_path),
            "30",
            "--hot-pixels",
        ],
        check=True,
        capture_output=True,
    )
    run = subprocess.run(
        [FIND, str(tmp_path / "series.nxs"), "-o", str(tmp_path / "s.refl")],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )
    assert run.returncode == 0, run.stderr
    assert "Hot pixels: not judged, from 30 images (100 at least)" in run.stdout


def test_mask_hot_needs_somewhere_to_write_them(series):
    run = subprocess.run(
        [
            FIND,
            str(series / "series.nxs"),
            "-o",
            str(series / "strong.refl"),
            "--mask-hot",
        ],
        capture_output=True,
        text=True,
        cwd=series,
    )
    assert run.returncode == 2
    assert "--mask-hot writes hot pixels into the experiment list's" in run.stderr


def test_mxi_untrusted_hot_says_them_and_nothing_else(series):
    # The hot pixels, every one, slow then fast, and the mask: not spot
    # finding's tables (Graeme, 18 November). --hot-list as JSON, for a
    # detector's own pixel mask.
    mxi = os.path.join(BUILD, "mxi")
    subprocess.run(
        [mxi, "convert", str(series / "l.expt"), "-o", str(series / "in.rflx")],
        check=True,
        capture_output=True,
        cwd=series,
    )
    run = subprocess.run(
        [
            mxi,
            "untrusted",
            str(series / "in.rflx"),
            "--hot",
            "-j",
            "2",
            "-o",
            str(series / "masked.rflx"),
            "--hot-list",
            str(series / "hot.json"),
        ],
        capture_output=True,
        text=True,
        cwd=series,
    )
    assert run.returncode == 0, run.stderr
    out = run.stdout
    assert "Hot pixels: 2 on >= 25% of the 120 images (--hot-fraction)" in out
    lines = out.splitlines()
    at = lines.index("     slow    fast  images")
    assert [l.split() for l in lines[at + 1 : at + 3]] == [
        ["4", "4", "100.0%"],
        ["250", "379", "50.0%"],
    ]
    for not_asked in (
        "Histogram",
        "Spots by resolution",
        "Extracted",
        "reflections to",
    ):
        assert not_asked not in out
    assert "image set 0, series.nxs: 2 single pixels" in out
    assert json.loads((series / "hot.json").read_text()) == [[4, 4], [250, 379]]
    sets = rflx.experiments_dict(str(series / "masked.rflx"))["imageset"]
    assert sets[0]["untrusted"] == [{"panel": 0, "pixels": [[4, 4], [379, 250]]}]
    assert not list(series.glob("*mxi_untrusted_hot*"))  # the scratch, removed
    # Spot finding's own lines went to its log.
    assert "Histogram of spots per image" in (series / "mxi_find.log").read_text()
    both = subprocess.run(
        [mxi, "untrusted", str(series / "in.rflx"), "--hot", "--clear"],
        capture_output=True,
        text=True,
        cwd=series,
    )
    assert both.returncode == 2 and "one of --set, --clear and --hot" in both.stderr

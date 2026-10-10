""".rflx, mxi's own files: dxtbx-h5's layout (docs/rflx.md), through
mxi_convert to and from the DIALS pair.

Each direction checked against something independent of mxi's C++: mxeq's
reader (mxeq.rflx), written from the specification with h5py alone, and
dxtbx-h5's own decoder where it is installed. A real DIALS table through .rflx
and back, every column the same; an experiment list from mxi_import through
and back to the same JSON, integers and floats apart; mxi_find's spots,
shoeboxes and all, through and back with every column's bytes the same;
and a file
dxtbx-h5 wrote, read by mxi to the JSON it was written from. Needs
MXI_CONVERT; and MXI_IMPORT, MXI_FIND and hdf5plugin for the planted data.
"""

import json
import os
import pathlib
import subprocess

import numpy as np
import pytest

from mxeq import refl, rflx

h5py = pytest.importorskip("h5py")
CONVERT = os.environ.get("MXI_CONVERT")
IMPORT = os.environ.get("MXI_IMPORT")
FIND = os.environ.get("MXI_FIND")
DATA = pathlib.Path(__file__).parent / "data"
needs = pytest.mark.skipif(not CONVERT, reason="set MXI_CONVERT")


def convert(directory, *words):
    r = subprocess.run(
        [CONVERT, *map(str, words)], capture_output=True, text=True, cwd=directory
    )
    assert r.returncode == 0, r.stderr + r.stdout
    return r.stdout


def strictly_equal(a, b, path=""):
    """Equal as JSON, and of the same types: 0 is not 0.0 here."""
    assert type(a) is type(b), f"{path}: {type(a).__name__} against {type(b).__name__}"
    if isinstance(a, dict):
        assert list(sorted(a)) == list(sorted(b)), path
        for k in a:
            strictly_equal(a[k], b[k], f"{path}.{k}")
    elif isinstance(a, list):
        assert len(a) == len(b), path
        for i, (x, y) in enumerate(zip(a, b)):
            strictly_equal(x, y, f"{path}[{i}]")
    else:
        assert a == b, f"{path}: {a!r} against {b!r}"


def same_table(a, b):
    assert a.nrows == b.nrows and a.identifiers == b.identifiers
    assert set(a.columns) == set(b.columns)
    for name in b.columns:
        x, y = np.asarray(a.columns[name]), np.asarray(b.columns[name])
        assert a.types[name] == b.types[name], name
        assert np.array_equal(x, y, equal_nan=np.issubdtype(y.dtype, np.floating)), name


@needs
def test_a_dials_table_goes_through_and_back(tmp_path):
    (tmp_path / "strong.refl").write_bytes((DATA / "dials_strong.refl").read_bytes())
    convert(tmp_path, "strong.refl")
    assert rflx.is_hdf5(str(tmp_path / "strong.rflx"))
    original = refl.load(str(tmp_path / "strong.refl"))
    same_table(refl.load(str(tmp_path / "strong.rflx")), original)
    (tmp_path / "strong.refl").unlink()
    convert(tmp_path, "strong.rflx")
    same_table(refl.load(str(tmp_path / "strong.refl")), original)


def imported(tmp_path):
    import test_import

    test_import.plant(tmp_path / "master.nxs")
    r = subprocess.run(
        [IMPORT, "master.nxs", "-o", "imported.expt"],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )
    assert r.returncode == 0, r.stderr
    return json.load(open(tmp_path / "imported.expt"))


@needs
@pytest.mark.skipif(not IMPORT, reason="set MXI_IMPORT")
def test_an_experiment_list_goes_through_and_back_types_and_all(tmp_path):
    original = imported(tmp_path)
    convert(tmp_path, "imported.expt", "-o", "imported.rflx")
    strictly_equal(rflx.experiments_dict(str(tmp_path / "imported.rflx")), original)
    try:
        from dxtbx_h5 import encoding
    except ImportError:
        encoding = None
    if encoding is not None:  # dxtbx-h5's own decoder agrees
        with h5py.File(tmp_path / "imported.rflx", "r") as f:
            strictly_equal(encoding.decode_group(f["experiments"]), original)
    (tmp_path / "imported.expt").unlink()
    convert(tmp_path, "imported.rflx")
    strictly_equal(json.load(open(tmp_path / "imported.expt")), original)


@needs
@pytest.mark.skipif(not FIND, reason="set MXI_FIND")
def test_shoeboxes_go_through_and_back_byte_for_byte(tmp_path):
    pytest.importorskip("hdf5plugin")
    import test_pixel_mask

    master = test_pixel_mask.series(tmp_path, mask_hot=False)
    r = subprocess.run(
        [FIND, str(master), "-o", "strong.refl"],
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )
    assert r.returncode == 0, r.stderr
    original = (tmp_path / "strong.refl").read_bytes()
    assert b"Shoebox<>" in original
    convert(tmp_path, "strong.refl")
    (tmp_path / "strong.refl").unlink()
    convert(tmp_path, "strong.rflx")
    # The same table, every column's type and very bytes, shoeboxes included.
    # Not the same file: mxi_find writes with the spot finder's own writer,
    # which orders the columns differently from the one mxi_convert uses, and
    # a msgpack map's order means nothing.
    import msgpack

    a = msgpack.unpackb(original, raw=False, strict_map_key=False)
    b = msgpack.unpackb(
        (tmp_path / "strong.refl").read_bytes(), raw=False, strict_map_key=False
    )
    assert a[:2] == b[:2]
    assert a[2] == b[2]


@needs
@pytest.mark.skipif(not IMPORT, reason="set MXI_IMPORT")
def test_a_file_dxtbx_h5_wrote_is_read_to_the_json_it_was_written_from(tmp_path):
    encoding = pytest.importorskip("dxtbx_h5.encoding")
    original = imported(tmp_path)
    with h5py.File(tmp_path / "theirs.rflx", "w") as f:
        f.attrs["format"] = "dxtbx_h5"
        f.attrs["format_version"] = 1
        encoding.encode_dict(f.create_group("experiments"), original)
    convert(tmp_path, "theirs.rflx")
    strictly_equal(json.load(open(tmp_path / "theirs.expt")), original)


SCALE = os.environ.get("MXI_SCALE")
SCALE_EXPT = os.environ.get("MXI_SCALE_EXPT")
SCALE_REFL = os.environ.get("MXI_SCALE_REFL")


def without_identifiers(path):
    d = json.load(open(path))
    d.pop("history", None)
    for e in d.get("experiment", []):
        e.pop("identifier", None)
    return d


@needs
@pytest.mark.skipif(
    not (SCALE and SCALE_EXPT and SCALE_REFL),
    reason="set MXI_SCALE, MXI_SCALE_EXPT, MXI_SCALE_REFL",
)
def test_a_program_given_one_rflx_writes_what_it_writes_given_the_pair(tmp_path):
    # One .rflx standing for both (rflx::as_pair), read by read_experiments and
    # read_reflections as the pair is: the same output.
    convert(tmp_path, SCALE_EXPT, SCALE_REFL, "-o", "in.rflx")
    for name, inputs in (("pair", [SCALE_EXPT, SCALE_REFL]), ("rflx", ["in.rflx"])):
        r = subprocess.run(
            [SCALE, *inputs, "-o", f"{name}.refl", "--output-expt", f"{name}.expt"],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr
    assert (tmp_path / "pair.refl").read_bytes() == (
        tmp_path / "rflx.refl"
    ).read_bytes()
    assert without_identifiers(tmp_path / "pair.expt") == without_identifiers(
        tmp_path / "rflx.expt"
    )


@needs
@pytest.mark.skipif(not FIND, reason="set MXI_FIND")
def test_mxi_find_takes_a_rflx_experiment_list_not_for_a_master(tmp_path):
    # HDF5 both, a .rflx has /experiments where a master has /entry. The
    # planted series of mxeq.fixtures.nxmx, with the experiment list it writes.
    pytest.importorskip("hdf5plugin")
    import sys

    subprocess.run(
        [sys.executable, "-m", "mxeq.fixtures.nxmx", str(tmp_path / "s"), "6"],
        check=True,
        capture_output=True,
    )
    convert(tmp_path, "s/series.expt", "-o", "series.rflx")
    for name, given in (("expt", "s/series.expt"), ("rflx", "series.rflx")):
        # Its list names no images, so the master is named as well -- and
        # the .rflx must still be taken as the list, not as a second master.
        r = subprocess.run(
            [FIND, given, "-x", "s/series.nxs", "-o", f"{name}.refl"],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr
    assert (tmp_path / "expt.refl").read_bytes() == (
        tmp_path / "rflx.refl"
    ).read_bytes()


# Writing (docs/rflx.md): nothing named, one .rflx named for the step; -o a
# .rflx, that; --output-expt, --output-refl or -o a .refl, the pair, as always.

INTEGRATE = os.environ.get("MXI_INTEGRATE")
REFINED_EXPT = os.environ.get("MXI_POSTREFINE_EXPT")
REFINED_REFL = os.environ.get("MXI_POSTREFINE_REFL")


def split(tmp_path, name):
    """A .rflx's halves, through mxi_convert, for comparing with the pair."""
    convert(
        tmp_path,
        name,
        "--output-expt",
        name + ".x.expt",
        "--output-refl",
        name + ".x.refl",
    )
    return name + ".x.expt", name + ".x.refl"


def same_pair(tmp_path, a, b):
    import msgpack

    (ea, ra), (eb, rb) = a, b
    assert without_identifiers(tmp_path / ea) == without_identifiers(tmp_path / eb)
    ta = msgpack.unpackb((tmp_path / ra).read_bytes(), raw=False, strict_map_key=False)
    tb = msgpack.unpackb((tmp_path / rb).read_bytes(), raw=False, strict_map_key=False)
    assert ta[2] == tb[2]


@needs
@pytest.mark.skipif(
    not (SCALE and SCALE_EXPT and SCALE_REFL),
    reason="set MXI_SCALE, MXI_SCALE_EXPT, MXI_SCALE_REFL",
)
def test_a_program_writes_a_rflx_unless_told_the_pair(tmp_path):
    def scale(*words):
        r = subprocess.run(
            [SCALE, SCALE_EXPT, SCALE_REFL, *words],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr

    scale()
    assert (tmp_path / "scaled.rflx").exists()
    assert (
        not (tmp_path / "scaled.expt").exists()
        and not (tmp_path / "scaled.refl").exists()
    )
    scale("-o", "named.rflx")
    assert (tmp_path / "named.rflx").exists()
    scale("-o", "old.refl")  # -o a .refl: the pair, as mxi_scale always wrote it
    assert (tmp_path / "old.refl").exists() and (tmp_path / "scaled.expt").exists()
    same_pair(tmp_path, ("scaled.expt", "old.refl"), split(tmp_path, "scaled.rflx"))
    same_pair(tmp_path, ("scaled.expt", "old.refl"), split(tmp_path, "named.rflx"))


@needs
@pytest.mark.skipif(not IMPORT, reason="set MXI_IMPORT")
def test_mxi_import_writes_the_experiments_alone(tmp_path):
    original = imported(tmp_path)  # -o imported.expt: JSON, as always
    r = subprocess.run(
        [IMPORT, "master.nxs"], capture_output=True, text=True, cwd=tmp_path
    )
    assert r.returncode == 0, r.stderr
    # Two imports, so two fresh identifiers: compared without them.
    ours = rflx.experiments_dict(str(tmp_path / "imported.rflx"))
    for d in (ours, original):
        d.pop("history", None)
        for e in d["experiment"]:
            e.pop("identifier", None)
    strictly_equal(ours, original)
    with h5py.File(tmp_path / "imported.rflx", "r") as f:
        assert "reflections" not in f


@needs
@pytest.mark.skipif(not FIND, reason="set MXI_FIND")
def test_mxi_find_writes_the_experiments_with_the_spots(tmp_path):
    pytest.importorskip("hdf5plugin")
    import sys

    subprocess.run(
        [sys.executable, "-m", "mxeq.fixtures.nxmx", str(tmp_path / "s"), "6"],
        check=True,
        capture_output=True,
    )
    for words in (["-o", "strong.refl"], []):
        r = subprocess.run(
            [FIND, "s/series.expt", "-x", "s/series.nxs", *words],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr
    assert not (tmp_path / "strong.rflx.refl").exists()  # the table beside it, removed
    strictly_equal(
        rflx.experiments_dict(str(tmp_path / "strong.rflx")),
        json.load(open(tmp_path / "s/series.expt")),
    )
    same_table(
        refl.load(str(tmp_path / "strong.rflx")),
        refl.load(str(tmp_path / "strong.refl")),
    )


@needs
@pytest.mark.skipif(
    not (INTEGRATE and REFINED_EXPT and REFINED_REFL),
    reason="set MXI_INTEGRATE, MXI_POSTREFINE_EXPT, MXI_POSTREFINE_REFL",
)
def test_mxi_integrate_writes_a_rflx_of_what_it_writes_as_the_pair(tmp_path):
    # Its runs -- one sweep or several, post-refined or not -- write the pair;
    # for a .rflx, beside it, joined into it at the end and removed.
    for words in (["-o", "pair.refl", "--output-expt", "pair.expt"], []):
        r = subprocess.run(
            [INTEGRATE, REFINED_EXPT, REFINED_REFL, "--threads", "2", *words],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr[-2000:]
    assert (
        not (tmp_path / "integrated.rflx.expt").exists()
        and not (tmp_path / "integrated.rflx.refl").exists()
    )
    same_pair(tmp_path, ("pair.expt", "pair.refl"), split(tmp_path, "integrated.rflx"))

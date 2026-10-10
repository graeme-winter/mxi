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

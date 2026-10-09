"""Reflection tables written as HDF5, as DIALS writes them from 2025 on.

Graeme's DIALS (3.dev, 2025) wrote integrated.refl as HDF5 -- each data set a
group /dials/processing/group_N, its experiment ids and identifiers attributes,
each column a dataset -- and mxi_symmetry read it as msgpack: 'expected an
array'. Both readers now tell the two apart by HDF5's signature.

The scaling fixture's table, msgpack, written again in DIALS's HDF5 layout --
as one group, and its rows split over two -- must load in mxeq to the same
columns, types and identifiers, and must scale in mxi_scale to the very same
bytes. Needs h5py; and for the second, MXI_SCALE, MXI_SCALE_EXPT and
MXI_SCALE_REFL.
"""

import os
import subprocess

import numpy as np
import pytest

from mxeq import refl

h5py = pytest.importorskip("h5py")
BINARY = os.environ.get("MXI_SCALE")
EXPT = os.environ.get("MXI_SCALE_EXPT")
REFL = os.environ.get("MXI_SCALE_REFL")


def as_hdf5(table, path, groups=1):
    """The table in DIALS's HDF5 layout, its rows split over `groups` groups."""
    edges = np.linspace(0, table.nrows, groups + 1).astype(int)
    with h5py.File(path, "w") as f:
        f.attrs["file_type"] = "dials_processed_data"
        f.attrs["file_version"] = 1
        for g in range(groups):
            group = f.create_group(f"dials/processing/group_{g}")
            ids = sorted(table.identifiers)
            group.attrs["experiment_ids"] = np.array(ids, dtype=np.int64)
            group.attrs["identifiers"] = [table.identifiers[i] for i in ids]
            for name, column in table.columns.items():
                array = np.asarray(column)
                kind = table.types[name]
                if kind == "bool":
                    array = array.astype(bool)
                elif kind == "std::size_t":
                    array = array.astype(np.uint64)
                elif kind in ("int", "int6", "cctbx::miller::index<>"):
                    array = array.astype(np.int32)
                group.create_dataset(name, data=array[edges[g] : edges[g + 1]])


@pytest.mark.skipif(not REFL, reason="set MXI_SCALE_REFL")
@pytest.mark.parametrize("groups", [1, 2])
def test_mxeq_reads_it_as_the_msgpack_table(tmp_path, groups):
    original = refl.load(REFL)
    as_hdf5(original, tmp_path / "t.refl", groups)
    read = refl.load(str(tmp_path / "t.refl"))
    assert read.nrows == original.nrows
    assert read.identifiers == original.identifiers
    for name, column in original.columns.items():
        assert read.types[name] == original.types[name], name
        a, b = np.asarray(read.columns[name]), np.asarray(column)
        # NaN where a column has no value is equal to NaN.
        assert np.array_equal(a, b, equal_nan=np.issubdtype(b.dtype, np.floating)), name


@pytest.mark.skipif(
    not (BINARY and EXPT and REFL),
    reason="set MXI_SCALE, MXI_SCALE_EXPT, MXI_SCALE_REFL",
)
@pytest.mark.parametrize("groups", [1, 2])
def test_mxi_scales_it_to_the_same_bytes(tmp_path, groups):
    original = refl.load(REFL)
    as_hdf5(original, tmp_path / "t.refl", groups)
    outputs = []
    for name, table in (("msgpack", REFL), ("hdf5", str(tmp_path / "t.refl"))):
        r = subprocess.run(
            [
                BINARY,
                EXPT,
                table,
                "-o",
                f"{name}.refl",
                "--output-expt",
                f"{name}.expt",
            ],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr
        outputs.append((tmp_path / f"{name}.refl").read_bytes())
    assert outputs[0] == outputs[1]

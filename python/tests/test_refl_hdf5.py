"""Reflection tables written as HDF5: DIALS's format (dials/dials#3255, its
default from 2025) and dxtbx-h5's, which shares its columns.

Graeme's DIALS wrote integrated.refl as HDF5 and mxi_symmetry read it as
msgpack: 'expected an array'. Both readers now tell the two apart by HDF5's
signature, and read either layout: DIALS's, its tables the groups under the
LAST group of /dials -- /dials/processing by default, but the writer takes the
name -- in the order written; and dxtbx-h5's, /reflections/N, where a column's
dials_type is preferred to inference. Columns mxi has no type for -- float32,
uint8, strings -- are left out. Shoeboxes, a group of concatenated arrays, DIALS
compresses with LZ4, which mxi reads without HDF5's plugin.

The scaling fixture's table, written again in each layout -- one group and two,
a second level not named processing, dials_type given, a float32 and a string
column beside the rest -- loads in mxeq to the same columns, types and
identifiers, and scales in mxi_scale to the very same bytes; the refined
fixture, its shoeboxes in DIALS's LZ4, integrates to the very same bytes too.
Needs h5py and hdf5plugin; and the programs and fixtures named below.
"""

import os
import struct
import subprocess

import msgpack
import numpy as np
import pytest

from mxeq import refl

h5py = pytest.importorskip("h5py")
hdf5plugin = pytest.importorskip("hdf5plugin")
SCALE = os.environ.get("MXI_SCALE")
SCALE_EXPT = os.environ.get("MXI_SCALE_EXPT")
SCALE_REFL = os.environ.get("MXI_SCALE_REFL")
INTEGRATE = os.environ.get("MXI_INTEGRATE")
REFINED_EXPT = os.environ.get("MXI_POSTREFINE_EXPT")
REFINED_REFL = os.environ.get("MXI_POSTREFINE_REFL")

DIALS_TYPE = {
    "bool": "bool",
    "int": "int",
    "std::size_t": "size_t",
    "double": "double",
    "vec2<double>": "vec2_double",
    "vec3<double>": "vec3_double",
    "mat3<double>": "mat3_double",
    "cctbx::miller::index<>": "miller_index",
    "int6": "int6",
}


def shoeboxes_of(path):
    """A msgpack table's shoeboxes: (panel, bbox, data, mask, background) each,
    from the record layout DIALS and mxi write, version 1 or 2."""
    doc = msgpack.unpackb(open(path, "rb").read(), raw=False, strict_map_key=False)
    _, (n, blob) = doc[2]["data"]["shoebox"]
    out, at = [], 0
    for _ in range(n):
        panel = struct.unpack_from("<I", blob, at)[0]
        at += 4
        bbox = struct.unpack_from("<6i", blob, at)
        at += 24
        version = blob[at]
        at += 1
        size = (bbox[1] - bbox[0]) * (bbox[3] - bbox[2]) * (bbox[5] - bbox[4])
        data = np.frombuffer(blob, "<f4", size, at)
        at += 4 * size
        if version == 1:
            mask = np.frombuffer(blob, "<i4", size, at).astype(np.uint8)
            at += 4 * size
        else:
            mask = np.frombuffer(blob, "u1", size, at)
            at += size
        background = np.frombuffer(blob, "<f4", size, at)
        at += 4 * size
        out.append((panel, bbox, data, mask, background))
    return out


def write_table(
    path, table, layout="dials", groups=1, process="processing", boxes=None
):
    """`table` in DIALS's HDF5 layout or dxtbx-h5's, its rows split over
    `groups` table groups; a float32 and a string column added, to be left out;
    shoeboxes in DIALS's LZ4 if given."""
    edges = np.linspace(0, table.nrows, groups + 1).astype(int)
    with h5py.File(path, "w", track_order=True) as f:
        if layout == "dials":
            f.attrs["file_type"] = "dials_processed_data"
            f.attrs["file_version"] = 1
            parent = f.create_group("dials", track_order=True).create_group(
                process, track_order=True
            )
            name_of = lambda g: f"group_{g}"
        else:
            f.attrs["format"] = "dxtbx_h5"
            parent = f.create_group("reflections", track_order=True)
            name_of = str
        for g in range(groups):
            lo, hi = edges[g], edges[g + 1]
            group = parent.create_group(name_of(g), track_order=True)
            ids = sorted(table.identifiers)
            group.attrs["experiment_ids"] = np.array(ids, dtype=np.uint64)
            group.attrs["identifiers"] = [table.identifiers[i] for i in ids]
            for name, column in table.columns.items():
                kind = table.types[name]
                array = np.asarray(column)
                if kind == "bool":
                    array = array.astype(bool)
                elif kind == "std::size_t":
                    array = array.astype(np.uint64)
                elif kind in ("int", "int6", "cctbx::miller::index<>"):
                    array = array.astype(np.int32)
                d = group.create_dataset(name, data=array[lo:hi])
                if layout != "dials":
                    d.attrs["dials_type"] = DIALS_TYPE[kind]
            group.create_dataset("a_float32_column", data=np.zeros(hi - lo, np.float32))
            group.create_dataset("a_string_column", data=[b"x"] * (hi - lo))
            if boxes is not None:
                part = boxes[lo:hi]
                lz4 = hdf5plugin.LZ4()
                sb = group.create_group("shoebox")
                sb.create_dataset(
                    "bbox",
                    data=np.array([b[1] for b in part], np.int32),
                    compression=lz4,
                )
                sb.create_dataset(
                    "panel",
                    data=np.array([b[0] for b in part], np.uint64),
                    compression=lz4,
                )
                for k, name in (
                    (2, "shoebox_data"),
                    (3, "shoebox_mask"),
                    (4, "shoebox_background"),
                ):
                    sb.create_dataset(
                        name, data=np.concatenate([b[k] for b in part]), compression=lz4
                    )


VARIANTS = [
    ("dials", 1, "processing"),
    ("dials", 2, "integrate"),
    ("dxtbx_h5", 1, None),
    ("dxtbx_h5", 2, None),
]


@pytest.mark.skipif(not SCALE_REFL, reason="set MXI_SCALE_REFL")
@pytest.mark.parametrize("layout, groups, process", VARIANTS)
def test_mxeq_reads_it_as_the_msgpack_table(tmp_path, layout, groups, process):
    original = refl.load(SCALE_REFL)
    write_table(tmp_path / "t.refl", original, layout, groups, process or "processing")
    read = refl.load(str(tmp_path / "t.refl"))
    assert read.nrows == original.nrows
    assert read.identifiers == original.identifiers
    assert (
        "a_float32_column" not in read.columns and "a_string_column" not in read.columns
    )
    for name, column in original.columns.items():
        assert read.types[name] == original.types[name], name
        a, b = np.asarray(read.columns[name]), np.asarray(column)
        # NaN where a column has no value is equal to NaN.
        assert np.array_equal(a, b, equal_nan=np.issubdtype(b.dtype, np.floating)), name


@pytest.mark.skipif(
    not (SCALE and SCALE_EXPT and SCALE_REFL),
    reason="set MXI_SCALE, MXI_SCALE_EXPT, MXI_SCALE_REFL",
)
@pytest.mark.parametrize("layout, groups, process", VARIANTS)
def test_mxi_scales_it_to_the_same_bytes(tmp_path, layout, groups, process):
    write_table(
        tmp_path / "t.refl",
        refl.load(SCALE_REFL),
        layout,
        groups,
        process or "processing",
    )
    outputs = []
    for name, table in (("msgpack", SCALE_REFL), ("hdf5", str(tmp_path / "t.refl"))):
        r = subprocess.run(
            [
                SCALE,
                SCALE_EXPT,
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


@pytest.mark.skipif(
    not (INTEGRATE and REFINED_EXPT and REFINED_REFL),
    reason="set MXI_INTEGRATE, MXI_POSTREFINE_EXPT, MXI_POSTREFINE_REFL",
)
def test_shoeboxes_in_dials_lz4_integrate_to_the_same_bytes(tmp_path):
    # The profile model is estimated from the strong spots' shoeboxes: read from
    # LZ4 chunks with no HDF5 plugin, they must be the msgpack table's, bit for bit.
    table = refl.load(REFINED_REFL)
    write_table(
        tmp_path / "t.refl",
        table,
        "dials",
        2,
        "processing",
        boxes=shoeboxes_of(REFINED_REFL),
    )
    outputs = []
    for name, source in (("msgpack", REFINED_REFL), ("hdf5", str(tmp_path / "t.refl"))):
        r = subprocess.run(
            [
                INTEGRATE,
                REFINED_EXPT,
                source,
                "--threads",
                "4",
                "-o",
                f"{name}.refl",
                "--output-expt",
                f"{name}.expt",
            ],
            capture_output=True,
            text=True,
            cwd=tmp_path,
        )
        assert r.returncode == 0, r.stderr[-1500:]
        outputs.append((tmp_path / f"{name}.refl").read_bytes())
    assert outputs[0] == outputs[1]

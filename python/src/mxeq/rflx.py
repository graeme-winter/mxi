""".rflx: one HDF5 file holding an experiment list, reflection tables or both,
in dxtbx-h5's layout (github.com/graeme-winter/dxtbx-h5, docs/HANDOVER.md;
mxi's docs/rflx.md). Read here from the specification with h5py alone -- not
through dxtbx-h5's own reader, so that agreeing with mxi's C++ is a check on
both.

``experiments_dict`` gives the experiment list's JSON dictionary from an .expt
or a .rflx alike; ``load_table`` the reflections, for ``refl.load``.
"""

from __future__ import annotations

import json

import numpy as np

HDF5_SIGNATURE = b"\x89HDF\r\n\x1a\n"
RESERVED = {"__type__", "__keys__", "__len__"}


def is_hdf5(path: str) -> bool:
    with open(path, "rb") as f:
        return f.read(8) == HDF5_SIGNATURE


def _escape(key: str) -> str:
    """HANDOVER 2.2: % first, then /, then a leading underscore; the empty key."""
    if key == "":
        return "%00"
    out = key.replace("%", "%25").replace("/", "%2F")
    return "%5F" + out[1:] if out.startswith("_") else out


def _unescape(name: str) -> str:
    if name == "%00":
        return ""
    if name.startswith("%5F"):
        name = "_" + name[3:]
    return name.replace("%2F", "/").replace("%25", "%")


def _text(value) -> str:
    return value.decode("utf-8") if isinstance(value, bytes) else str(value)


def _leaf(value, null: bool):
    """A value's type from its dtype and dataspace alone (HANDOVER 2.2)."""
    import h5py

    if null or isinstance(value, h5py.Empty):
        return None
    if isinstance(value, np.ndarray):
        if value.dtype.kind in "OSU":
            return (
                np.vectorize(_text, otypes=[object])(value).tolist()
                if value.size
                else []
            )
        return value.tolist()
    if isinstance(value, (bytes, str)):
        return _text(value)
    if isinstance(value, np.generic):
        return value.item()
    return value


def _member(group, name: str):
    import h5py

    if name in group.attrs:
        a = group.attrs.get_id(name)
        return _leaf(
            group.attrs[name], a.get_space().get_simple_extent_type() == h5py.h5s.NULL
        )
    obj = group[name]
    if isinstance(obj, h5py.Group):
        return _decode(obj)
    null = obj.id.get_space().get_simple_extent_type() == h5py.h5s.NULL
    return _leaf(None if null else obj[()], null)


def _decode(group):
    kind = _text(group.attrs.get("__type__", "dict"))
    if kind == "list":
        return [_member(group, str(i)) for i in range(int(group.attrs["__len__"]))]
    if "__keys__" in group.attrs:
        return {
            _text(k): _member(group, _escape(_text(k))) for k in group.attrs["__keys__"]
        }
    names = sorted(set(group.keys()) | {n for n in group.attrs if n not in RESERVED})
    return {_unescape(n): _member(group, n) for n in names}


def experiments_dict(path: str) -> dict:
    """The experiment list's JSON dictionary, from an .expt or a .rflx."""
    if not is_hdf5(path):
        with open(path) as f:
            return json.load(f)
    import h5py

    with h5py.File(path, "r") as f:
        if "experiments" not in f:
            raise ValueError(f"{path} holds no experiment list")
        return _decode(f["experiments"])


_TAGS = {
    "bool": "bool",
    "int": "int",
    "size_t": "std::size_t",
    "double": "double",
    "vec2_double": "vec2<double>",
    "vec3_double": "vec3<double>",
    "mat3_double": "mat3<double>",
    "miller_index": "cctbx::miller::index<>",
    "int6": "int6",
    "tiny_int_2": "tiny<int,2>",
}


def _column_type(dataset) -> str | None:
    """By dials_type where given, else by dtype and shape (HANDOVER 3.3);
    None for a type mxi's tables have no place for."""
    tag = dataset.attrs.get("dials_type")
    if tag is not None:
        return _TAGS.get(_text(tag))
    shape, dtype = dataset.shape, dataset.dtype
    width = int(np.prod(shape[1:])) if len(shape) > 1 else 1
    if dtype == np.bool_:
        return "bool"
    if dtype == np.float64:
        return {
            1: "double",
            2: "vec2<double>",
            3: "vec3<double>",
            9: "mat3<double>",
        }.get(width)
    if dtype.kind == "i" and width == 3 and len(shape) == 2:
        return "cctbx::miller::index<>"
    if dtype.kind == "i" and width == 6:
        return "int6"
    if dtype == np.uint64 and width == 1:
        return "std::size_t"
    if dtype == np.int32 and width == 1:
        return "int"
    return None


def load_table(path: str):
    """The tables under /reflections, joined in order; shoeboxes left aside,
    which mxeq has no use for."""
    import h5py

    from mxeq.refl import ReflectionTable, ReflFormatError

    columns: dict[str, list] = {}
    types: dict[str, str] = {}
    identifiers: dict[int, str] = {}
    nrows = 0
    with h5py.File(path, "r") as f:
        if "reflections" not in f:
            raise ReflFormatError(f"{path} holds no reflections")
        group = f["reflections"]
        names = sorted(
            (n for n in group if isinstance(group[n], h5py.Group)),
            key=lambda n: (len(n), n),
        )
        for name in names:
            table = group[name]
            for i, identifier in zip(
                table.attrs.get("experiment_ids", []),
                table.attrs.get("identifiers", []),
            ):
                identifiers[int(i)] = _text(identifier)
            rows = int(table.attrs["nrows"]) if "nrows" in table.attrs else None
            for key, dataset in table.items():
                if not isinstance(dataset, h5py.Dataset):
                    continue
                kind = _column_type(dataset)
                if kind is None:
                    continue
                array = dataset[()]
                if array.ndim == 3:
                    array = array.reshape(array.shape[0], -1)
                rows = len(array) if rows is None else rows
                types[key] = kind
                columns.setdefault(key, []).append(array)
            nrows += rows or 0
    out = ReflectionTable(nrows=nrows, identifiers=identifiers)
    for key, parts in columns.items():
        out.columns[key] = np.concatenate(parts)
        out.types[key] = types[key]
    out.validate()
    return out

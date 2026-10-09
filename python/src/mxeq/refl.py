"""Read and write DIALS reflection tables (``.refl``) without cctbx.

A ``.refl`` file is a msgpack document.  At the top level it is a three-element
array: the tag ``dials::af::reflection_table``, a format version, and a map
holding the row count, the experiment identifiers, and the columns.  Each
column is a two-element array of a C++ type name and a payload, and the payload
is itself a pair of an element count and a ``bin`` holding the values packed
little-endian, with the components of a compound type adjacent rather than in
separate arrays.

The element count inside the payload is the number of *rows*, not the number of
scalars, so a ``vec3<double>`` column of 13766 rows reports 13766 and carries
330384 bytes.  It is checked against the blob length rather than trusted,
because the two disagreeing is the signature of a truncated file.

Nothing here knows about cctbx.  That is the point: the checker has to run in a
container with no DIALS build in it, against files written by a DIALS build
somewhere else.

STATUS
------
Validated against ``strong.refl`` and ``metal.refl`` written by DIALS 3.x and by
``mxi_find`` (13766 rows, ten columns including a shoebox).  The
first version of this reader assumed a two-element top level and a bare ``bin``
payload, and was wrong on both counts; see ``CLAUDE.md``.  Element widths --
``int`` 4 bytes, ``std::size_t`` 8, ``double`` 8, ``vec3<double>`` 24, ``int6``
24 -- were confirmed against that file.

Both the two- and three-element top level are accepted, and a payload is
accepted either wrapped in its count or bare, because neither cost anything and
a reader that only understands the one file it was tested on is not much of a
reader.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field

import msgpack
import numpy as np

TAG = "dials::af::reflection_table"

#: The format version DIALS writes, and the one :func:`dumps` writes back.
FORMAT_VERSION = 2

#: The tag accepted for the map key holding the experiment identifiers.  DIALS
#: has spelled this more than one way; both are read, the first is written.
IDENTIFIER_KEYS = ("identifiers", "experiment_identifiers")

# C++ type name -> (numpy scalar dtype, components per row).  A column of
# ``vec3<double>`` is 3N doubles, not N of anything, so the shape is imposed
# here and not inferred from the byte count alone -- inferring it would silently
# accept a truncated payload.
NUMERIC_TYPES = {
    "int": ("<i4", 1),
    "std::size_t": ("<u8", 1),
    "double": ("<f8", 1),
    "bool": ("|u1", 1),
    "vec2<double>": ("<f8", 2),
    "vec3<double>": ("<f8", 3),
    "mat3<double>": ("<f8", 9),
    "int6": ("<i4", 6),
    "cctbx::miller::index<>": ("<i4", 3),
    "tiny<int,2>": ("<i4", 2),
}

#: Types carried through without being decoded.  A shoebox is a nested record
#: with its own variable-length pixel arrays, and no equivalence check here
#: looks inside one; decoding it would be work spent to produce something
#: nothing reads.
OPAQUE_TYPES = ("Shoebox<>",)


class ReflFormatError(Exception):
    """The file is msgpack, but not a reflection table this can read."""


@dataclass
class ReflectionTable:
    """Columns as numpy arrays, plus the row count and the identifiers.

    ``nrows`` is read from the file rather than derived from the columns, and
    :meth:`validate` compares the two.  A file whose header and payload
    disagree is a corrupt file, and saying so is more useful than quietly
    believing one of them.
    """

    nrows: int
    columns: dict[str, np.ndarray] = field(default_factory=dict)
    identifiers: dict[int, str] = field(default_factory=dict)
    types: dict[str, str] = field(default_factory=dict)
    opaque: dict[str, str] = field(default_factory=dict)
    #: The format version read from the file, or None if the document had no
    #: version element. Reported by `mxeq inspect`, not acted on.
    version: int | None = None

    def __len__(self) -> int:
        return self.nrows

    def __contains__(self, key: str) -> bool:
        return key in self.columns

    def __getitem__(self, key: str) -> np.ndarray:
        try:
            return self.columns[key]
        except KeyError:
            raise KeyError(
                f"no column {key!r}; have {', '.join(sorted(self.columns))}"
            ) from None

    def validate(self) -> None:
        for name, values in self.columns.items():
            if len(values) != self.nrows:
                raise ReflFormatError(
                    f"column {name!r} has {len(values)} rows, header says {self.nrows}"
                )

    def select(self, mask: np.ndarray) -> ReflectionTable:
        """A new table holding the selected rows.  Opaque columns are dropped."""
        mask = np.asarray(mask)
        if mask.dtype == bool:
            n = int(mask.sum())
        else:
            n = len(mask)
        return ReflectionTable(
            nrows=n,
            columns={k: v[mask] for k, v in self.columns.items()},
            identifiers=dict(self.identifiers),
            types=dict(self.types),
        )


def _unwrap(name: str, payload: object, nrows: int) -> object:
    """Strip the (count, blob) pair DIALS wraps each column payload in.

    The count is the number of rows and not the number of scalars, so it is
    compared against the row count rather than against the blob length divided
    by anything. A count that disagrees means a truncated or mis-assembled
    file, which is worth an error rather than a silently short column.
    """
    if isinstance(payload, (list, tuple)) and len(payload) == 2:
        count, inner = payload
        if isinstance(count, int) and isinstance(
            inner, (bytes, bytearray, list, tuple)
        ):
            if count != nrows:
                raise ReflFormatError(
                    f"column {name!r}: payload declares {count} elements, "
                    f"the table declares {nrows} rows"
                )
            return inner
    return payload


def _decode_column(name: str, type_name: str, payload: object) -> np.ndarray:
    if type_name == "std::string":
        if not isinstance(payload, (list, tuple)):
            raise ReflFormatError(f"column {name!r}: string payload is not an array")
        return np.array(
            [p.decode() if isinstance(p, bytes) else p for p in payload], dtype=object
        )

    dtype, width = NUMERIC_TYPES[type_name]
    if not isinstance(payload, (bytes, bytearray)):
        raise ReflFormatError(
            f"column {name!r} of type {type_name}: payload is "
            f"{type(payload).__name__}, expected a binary blob"
        )
    values = np.frombuffer(bytes(payload), dtype=dtype)
    if width == 1:
        out = values
    else:
        if len(values) % width:
            raise ReflFormatError(
                f"column {name!r} of type {type_name}: {len(values)} scalars "
                f"is not a multiple of {width}"
            )
        out = values.reshape(-1, width)
    if type_name == "bool":
        out = out.astype(bool)
    return out


def loads(raw: bytes) -> ReflectionTable:
    """Decode a reflection table from the bytes of a ``.refl`` file."""
    doc = msgpack.unpackb(raw, raw=True, strict_map_key=False)
    return _from_document(doc)


def load(path: str) -> ReflectionTable:
    with open(path, "rb") as f:
        return loads(f.read())


def _text(value: object) -> object:
    return value.decode() if isinstance(value, bytes) else value


def _unraw(obj: object) -> object:
    """Recursively turn msgpack ``raw`` bytes back into ``str`` for map keys."""
    if isinstance(obj, dict):
        return {_text(k): _unraw(v) for k, v in obj.items()}
    return obj


def _from_document(doc: object) -> ReflectionTable:
    if not isinstance(doc, (list, tuple)) or len(doc) not in (2, 3):
        raise ReflFormatError(
            "not a reflection table: the document is not an array of two or "
            "three elements. Run `mxeq inspect` on it."
        )
    tag = _text(doc[0])
    if tag != TAG:
        raise ReflFormatError(f"unexpected tag {tag!r}, expected {TAG!r}")

    # Three elements is [tag, version, body]; two is [tag, body]. The version
    # is kept rather than checked: a reader that refuses an unknown version
    # cannot report what it found, and reporting is the whole job here.
    version = int(doc[1]) if len(doc) == 3 and isinstance(doc[1], int) else None
    body = _unraw(doc[-1])
    if not isinstance(body, dict):
        raise ReflFormatError("the reflection table body is not a map")

    if "nrows" not in body:
        raise ReflFormatError(
            f"no 'nrows' in the table body; keys are {', '.join(map(str, body))}"
        )
    nrows = int(body["nrows"])

    identifiers: dict[int, str] = {}
    for key in IDENTIFIER_KEYS:
        if key in body and isinstance(body[key], dict):
            identifiers = {int(k): _text(v) for k, v in body[key].items()}
            break

    data = body.get("data")
    if not isinstance(data, dict):
        raise ReflFormatError("no 'data' map in the table body")

    table = ReflectionTable(nrows=nrows, identifiers=identifiers, version=version)
    for raw_name, column in data.items():
        name = str(_text(raw_name))
        if not isinstance(column, (list, tuple)) or len(column) != 2:
            raise ReflFormatError(f"column {name!r} is not a [type, payload] pair")
        type_name = str(_text(column[0]))
        if type_name in OPAQUE_TYPES:
            table.opaque[name] = type_name
            continue
        if type_name not in NUMERIC_TYPES and type_name != "std::string":
            print(
                f"mxeq: skipping column {name!r}: unknown type {type_name!r}",
                file=sys.stderr,
            )
            table.opaque[name] = type_name
            continue
        table.columns[name] = _decode_column(
            name, type_name, _unwrap(name, column[1], nrows)
        )
        table.types[name] = type_name

    table.validate()
    return table


def _encode_column(values: np.ndarray, type_name: str) -> object:
    if type_name == "std::string":
        return [str(v) for v in values]
    dtype, _ = NUMERIC_TYPES[type_name]
    if type_name == "bool":
        return np.asarray(values, dtype=bool).astype("|u1").tobytes()
    return np.ascontiguousarray(values, dtype=dtype).tobytes()


def dumps(table: ReflectionTable) -> bytes:
    """Encode a reflection table.  Used for fixtures, and to round-trip the reader."""
    table.validate()
    data = {}
    for name, values in table.columns.items():
        type_name = table.types.get(name)
        if type_name is None:
            raise ReflFormatError(f"column {name!r} has no recorded C++ type")
        data[name] = [
            type_name,
            [table.nrows, _encode_column(values, type_name)],
        ]
    body = {
        IDENTIFIER_KEYS[0]: {int(k): v for k, v in table.identifiers.items()},
        "nrows": table.nrows,
        "data": data,
    }
    return msgpack.packb(
        [TAG, table.version or FORMAT_VERSION, body], use_bin_type=True
    )


def write(path: str, table: ReflectionTable) -> None:
    with open(path, "wb") as f:
        f.write(dumps(table))


def structure(raw: bytes, max_items: int = 40) -> list[str]:
    """Describe an unknown msgpack document, assuming nothing about its keys.

    This is what to reach for when :func:`loads` refuses a real file, so it
    must not raise -- and the first version did, on the first real file it was
    given, because it decoded every ``bin`` as UTF-8 to see whether it was
    printable text.  A column of packed doubles is not valid UTF-8.  An escape
    hatch that fails on binary data is no escape hatch at all.
    """
    lines: list[str] = []

    def printable(obj: bytes) -> str | None:
        """The value as text, or None if it is not short printable text."""
        if len(obj) >= 60:
            return None
        try:
            text = obj.decode("utf-8")
        except UnicodeDecodeError:
            return None
        return text if text.isprintable() else None

    def walk(obj: object, indent: int, label: str) -> None:
        pad = "  " * indent
        if isinstance(obj, dict):
            lines.append(f"{pad}{label}map, {len(obj)} keys")
            for i, (k, v) in enumerate(obj.items()):
                if i >= max_items:
                    lines.append(f"{pad}  ... {len(obj) - max_items} more")
                    break
                key = k
                if isinstance(k, (bytes, bytearray)):
                    key = printable(bytes(k)) or f"<{len(k)} bytes>"
                walk(v, indent + 1, f"{key!r}: ")
        elif isinstance(obj, (list, tuple)):
            lines.append(f"{pad}{label}array, {len(obj)} items")
            for i, v in enumerate(obj):
                if i >= max_items:
                    lines.append(f"{pad}  ... {len(obj) - max_items} more")
                    break
                walk(v, indent + 1, f"[{i}] ")
        elif isinstance(obj, (bytes, bytearray)):
            text = printable(bytes(obj))
            if text is None:
                lines.append(f"{pad}{label}binary, {len(obj)} bytes")
            else:
                lines.append(f"{pad}{label}{text!r}")
        else:
            lines.append(f"{pad}{label}{obj!r}")

    try:
        doc = msgpack.unpackb(raw, raw=True, strict_map_key=False)
    except Exception as exc:  # noqa: BLE001 - the exception *is* the report
        return [f"not decodable as msgpack: {exc}"]
    try:
        walk(doc, 0, "")
    except Exception as exc:  # noqa: BLE001 - never let the escape hatch fail
        lines.append(f"... description stopped: {type(exc).__name__}: {exc}")
    return lines


def has_prediction(table, rows=None):
    """Which rows carry a real predicted position.

    Not ``xyzcal.px != 0``. A reflection that was never predicted has the
    column allocated and never written, and what is in it is whatever was in
    the memory: on a real DIALS indexed.refl those rows read

        xyzcal.px = [1.5e-320, 5.2e-310, 0.0]

    which are denormals, and which compare unequal to zero. Differencing real
    predictions against them reported offsets of fifteen hundred pixels and a
    median of 1.5e-320 -- a number whose only meaning is that it came from
    memory nobody wrote.

    A prediction exists where the reflection is indexed. That is the question
    actually being asked, and it does not depend on what uninitialised memory
    happened to hold.
    """
    predicted = np.any(table["miller_index"] != 0, axis=1)
    if "xyzcal.px" in table:
        values = np.abs(table["xyzcal.px"])
        # Two ways a row can carry no prediction, and both occur in the wild.
        # Left as written by whoever allocated the column: exactly zero, which
        # is what this package writes and what a failed prediction leaves
        # behind. Or never written at all: whatever was in the memory, which on
        # a real DIALS file reads as denormals around 1e-320.
        #
        # A genuine prediction at exactly the detector origin is possible in
        # principle and is not worth the ambiguity; it is one pixel out of four
        # million and it is not on the detector face in any real geometry.
        written = np.any(values != 0, axis=1)
        sane = ~np.any((values > 0) & (values < 1e-30), axis=1)
        predicted = predicted & written & sane
    return predicted if rows is None else predicted[rows]

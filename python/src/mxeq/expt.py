"""Read DIALS experiment lists (``.expt``) without cctbx.

An ``.expt`` file is JSON: a list of experiments, each referring by integer
index into shared lists of beams, detectors, crystals, scans and goniometers.
What is needed here is the crystal (cell and orientation, static or per scan
point), the scan, and enough of the detector to say whether two models put the
beam in the same place.

Unit cells and orientations are derived from the serialised real-space vectors
rather than from any stored cell parameters, because the vectors are what the
refinement actually produced and the two can disagree in a file someone has
edited.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field

import numpy as np


class ExptFormatError(Exception):
    pass


@dataclass
class Crystal:
    a: np.ndarray  # real space a, b, c as rows of a 3x3
    hall: str | None = None
    #: Setting matrices at each scan point, as (n, 3, 3), for a scan-varying
    #: model.  Empty for a static one.
    a_at_scan_points: np.ndarray = field(default_factory=lambda: np.empty((0, 3, 3)))

    @property
    def scan_varying(self) -> bool:
        return len(self.a_at_scan_points) > 0

    @property
    def cell(self) -> tuple[float, float, float, float, float, float]:
        return cell_from_real_space(self.a)

    @property
    def volume(self) -> float:
        return abs(float(np.linalg.det(self.a)))

    @property
    def setting(self) -> np.ndarray:
        """The A matrix, with a*, b*, c* as its columns.

        The real-space matrix has a, b, c as its *rows*, so that their product
        is the identity by the definition of the reciprocal basis -- and the
        setting matrix is therefore the plain inverse, not the inverse
        transpose. Getting this wrong transposes every resolution calculation
        for a triclinic cell and for nothing else, which is the worst possible
        way for it to be wrong.
        """
        return np.linalg.inv(self.a)

    def cells_at_scan_points(self) -> np.ndarray:
        """(n, 6) cell parameters over the scan; empty if the model is static."""
        if not self.scan_varying:
            return np.empty((0, 6))
        return np.array(
            [cell_from_real_space(np.linalg.inv(A)) for A in self.a_at_scan_points]
        )


@dataclass
class Scan:
    image_range: tuple[int, int]
    oscillation: tuple[float, float]
    batch_offset: int = 0
    #: Worst departure from a constant oscillation width, as a fraction of the
    #: width. Round-off alone gives about 1e-14; anything larger is a scan that
    #: does not have one width, and reporting it as though it did would be a
    #: fiction.
    max_width_deviation: float = 0.0

    @property
    def num_images(self) -> int:
        return self.image_range[1] - self.image_range[0] + 1


@dataclass
class Detector:
    """Only the parts two models can be meaningfully compared on."""

    num_panels: int
    image_size: tuple[int, int]
    pixel_size: tuple[float, float]
    origins: np.ndarray  # (n, 3)
    fast_axes: np.ndarray
    slow_axes: np.ndarray

    def normal_distance(self) -> float:
        """Perpendicular distance from the sample to panel 0's plane."""
        normal = np.cross(self.fast_axes[0], self.slow_axes[0])
        normal /= np.linalg.norm(normal)
        return abs(float(np.dot(self.origins[0], normal)))


@dataclass
class Beam:
    wavelength: float
    direction: np.ndarray


@dataclass
class Experiment:
    identifier: str = ""
    crystal: Crystal | None = None
    scan: Scan | None = None
    detector: Detector | None = None
    beam: Beam | None = None


@dataclass
class ExperimentList:
    experiments: list[Experiment] = field(default_factory=list)

    def __len__(self) -> int:
        return len(self.experiments)

    def __getitem__(self, i: int) -> Experiment:
        return self.experiments[i]

    def __iter__(self):
        return iter(self.experiments)


def cell_from_real_space(a: np.ndarray) -> tuple[float, ...]:
    """Cell parameters from a 3x3 whose rows are the real-space basis vectors."""
    lengths = [float(np.linalg.norm(v)) for v in a]
    angles = []
    for i, j in ((1, 2), (0, 2), (0, 1)):
        cos = float(np.dot(a[i], a[j]) / (lengths[i] * lengths[j]))
        angles.append(math.degrees(math.acos(max(-1.0, min(1.0, cos)))))
    return (*lengths, *angles)


def _crystal(d: dict) -> Crystal:
    try:
        a = np.array(
            [d["real_space_a"], d["real_space_b"], d["real_space_c"]], dtype=float
        )
    except KeyError as exc:
        raise ExptFormatError(f"crystal has no {exc.args[0]}") from None
    scan_points = d.get("A_at_scan_points") or []
    return Crystal(
        a=a,
        hall=d.get("space_group_hall_symbol"),
        a_at_scan_points=(
            np.array([np.array(A, dtype=float).reshape(3, 3) for A in scan_points])
            if scan_points
            else np.empty((0, 3, 3))
        ),
    )


def _detector(d: dict) -> Detector:
    panels = d["panels"] if isinstance(d, dict) else d
    if not panels:
        raise ExptFormatError("detector has no panels")
    return Detector(
        num_panels=len(panels),
        image_size=tuple(panels[0]["image_size"]),
        pixel_size=tuple(panels[0]["pixel_size"]),
        origins=np.array([p["origin"] for p in panels], dtype=float),
        fast_axes=np.array([p["fast_axis"] for p in panels], dtype=float),
        slow_axes=np.array([p["slow_axis"] for p in panels], dtype=float),
    )


def _scan(d: dict) -> Scan:
    """Read a scan, accepting both the old and current oscillation forms.

    Current DIALS writes ``properties.oscillation`` as a per-image array of
    start angles; older files carry a top-level ``oscillation`` of
    ``[start, width]``. Reading only the old key against a current file yields
    ``(0.0, 0.0)`` for both sides of a comparison, which then agree perfectly
    and say nothing -- a silent pass, which is worse than a failure.

    The width is taken from the endpoints rather than the first two elements.
    The array is built by repeated addition, so adjacent differences are a
    subtraction of nearby doubles; spanning the scan divides that error by the
    number of images.
    """
    image_range = tuple(d["image_range"])
    oscillation = d.get("oscillation")
    deviation = 0.0

    if oscillation is None:
        values = (d.get("properties") or {}).get("oscillation") or []
        if len(values) >= 2:
            width = (values[-1] - values[0]) / (len(values) - 1)
            if width:
                gaps = [values[i] - values[i - 1] for i in range(1, len(values))]
                deviation = max(abs(g - width) for g in gaps) / abs(width)
            oscillation = (values[0], width)
        elif len(values) == 1:
            oscillation = (values[0], 0.0)
        else:
            oscillation = (0.0, 0.0)

    return Scan(
        image_range=image_range,
        oscillation=tuple(oscillation),
        batch_offset=int(d.get("batch_offset", 0)),
        max_width_deviation=deviation,
    )


def loads(text: str) -> ExperimentList:
    doc = json.loads(text)
    if not isinstance(doc, dict) or "experiment" not in doc:
        raise ExptFormatError(
            "not an experiment list: no 'experiment' key at the top level"
        )

    crystals = [_crystal(c) for c in doc.get("crystal", [])]
    scans = [_scan(s) for s in doc.get("scan", [])]
    detectors = [_detector(x) for x in doc.get("detector", [])]
    beams = [
        Beam(
            wavelength=float(b["wavelength"]),
            direction=np.array(b["direction"], dtype=float),
        )
        for b in doc.get("beam", [])
    ]

    def pick(pool: list, index: object):
        # A missing model is serialised as -1 or as a null, not as an absent
        # key, so both have to mean "no model" rather than "index -1".
        if index is None or not isinstance(index, int) or index < 0:
            return None
        return pool[index] if index < len(pool) else None

    out = ExperimentList()
    for e in doc["experiment"]:
        out.experiments.append(
            Experiment(
                identifier=e.get("identifier", ""),
                crystal=pick(crystals, e.get("crystal")),
                scan=pick(scans, e.get("scan")),
                detector=pick(detectors, e.get("detector")),
                beam=pick(beams, e.get("beam")),
            )
        )
    return out


def load(path: str) -> ExperimentList:
    """An .expt, or a .rflx's experiment list (mxeq.rflx)."""
    from mxeq import rflx

    return loads(json.dumps(rflx.experiments_dict(path)))

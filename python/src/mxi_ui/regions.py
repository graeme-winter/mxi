"""The detector's untrusted regions, as the viewer draws them: each image
set's, in mxi's form (docs/mask.md) -- {"panel": 0, "circle": [x, y, r]},
{"panel": 0, "rectangle": [x0, y0, x1, y1]}, {"panel": 0, "pixels":
[[x, y], ...]} (hot pixels, from mxi find --mask-hot), {"panel": 0, "polygon":
[[x, y], ...]} -- in pixels as DIALS counts them, pixel i spanning i to
i + 1, which are the viewer's own coordinates. Made from what is drawn; undone
a step at a time, each image set its own; hit by a point, to select one. No
tkinter here: saved by mxi_tools.save_untrusted, through mxi untrusted.
"""

import copy
import math


def rectangle(x0, y0, x1, y1):
    """Dragged corner to corner, either way: None where it has no area."""
    a, b = sorted((x0, x1)), sorted((y0, y1))
    if a[1] - a[0] < 0.5 or b[1] - b[0] < 0.5:
        return None
    return {
        "panel": 0,
        "rectangle": [round(a[0], 1), round(b[0], 1), round(a[1], 1), round(b[1], 1)],
    }


def circle(cx, cy, x, y):
    """Dragged from its centre to its edge: None where it has no size."""
    r = math.hypot(x - cx, y - cy)
    if r < 0.5:
        return None
    return {"panel": 0, "circle": [round(cx, 1), round(cy, 1), round(r, 1)]}


def polygon(points):
    """Its vertices, as clicked: None where fewer than three remain once
    repeats -- a double-click's second click -- are dropped."""
    kept = []
    for x, y in points:
        if not kept or math.hypot(x - kept[-1][0], y - kept[-1][1]) >= 0.5:
            kept.append([round(x, 1), round(y, 1)])
    if (
        len(kept) > 1
        and math.hypot(kept[0][0] - kept[-1][0], kept[0][1] - kept[-1][1]) < 0.5
    ):
        kept.pop()
    return {"panel": 0, "polygon": kept} if len(kept) >= 3 else None


def contains(region, x, y):
    """Whether point (x, y) is inside the region."""
    if "rectangle" in region:
        x0, y0, x1, y1 = region["rectangle"]
        return x0 <= x <= x1 and y0 <= y <= y1
    if "circle" in region:
        cx, cy, r = region["circle"]
        return math.hypot(x - cx, y - cy) <= r
    if "polygon" in region:
        inside = False
        pts = region["polygon"]
        for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]):
            if (ay > y) != (by > y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
                inside = not inside
        return inside
    if "pixels" in region:
        # Hot pixels (mxi find --mask-hot): within two pixels of one, as
        # clicking one exactly, zoomed out, would be too fiddly.
        return any(
            px - 2 <= x <= px + 3 and py - 2 <= y <= py + 3
            for px, py in region["pixels"]
        )
    return False


class Regions:
    """Each image set's regions, and each one's history for Undo: every
    change -- a region added or removed, Clear, Same as previous -- first
    keeps the list as it was."""

    def __init__(self, initial=None):
        self.by_imageset = {
            k: copy.deepcopy(list(v)) for k, v in (initial or {}).items()
        }
        self.history = {}
        self.changed = False

    def of(self, imageset):
        return self.by_imageset.setdefault(imageset, [])

    def _keep(self, imageset):
        self.history.setdefault(imageset, []).append(copy.deepcopy(self.of(imageset)))
        self.changed = True

    def add(self, imageset, region):
        if region is None:
            return False
        self._keep(imageset)
        self.of(imageset).append(region)
        return True

    def remove(self, imageset, index):
        if not 0 <= index < len(self.of(imageset)):
            return False
        self._keep(imageset)
        del self.of(imageset)[index]
        return True

    def clear(self, imageset):
        if not self.of(imageset):
            return False
        self._keep(imageset)
        self.by_imageset[imageset] = []
        return True

    def copy_from(self, imageset, other):
        """Same as another image set's: for sweeps that share their shadows."""
        if other == imageset or other not in self.by_imageset:
            return False
        self._keep(imageset)
        self.by_imageset[imageset] = copy.deepcopy(self.of(other))
        return True

    def undo(self, imageset):
        stack = self.history.get(imageset)
        if not stack:
            return False
        self.by_imageset[imageset] = stack.pop()
        return True

    def at(self, imageset, x, y):
        """The index of the last-drawn region holding point (x, y), or None."""
        for i in range(len(self.of(imageset)) - 1, -1, -1):
            if contains(self.of(imageset)[i], x, y):
                return i
        return None

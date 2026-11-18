"""The untrusted regions' logic (mxi_ui/regions.py), with no window: each
shape from what is drawn, which point it holds, and each image set's
regions changed and undone."""

from mxi_ui import regions as r


def test_shapes_from_what_is_drawn():
    assert r.rectangle(30, 40, 10, 20) == {
        "panel": 0,
        "rectangle": [10, 20, 30, 40],
    }, "either way"
    assert r.rectangle(10, 20, 10.2, 40) is None, "no width"
    assert r.circle(100, 100, 103, 104) == {"panel": 0, "circle": [100, 100, 5.0]}
    assert r.circle(100, 100, 100.1, 100) is None
    tri = r.polygon([(0, 0), (10, 0), (10, 0.1), (0, 10), (0.1, 0)])
    assert tri == {
        "panel": 0,
        "polygon": [[0, 0], [10, 0], [0, 10]],
    }, "a double-click's repeat, and a last vertex on the first, dropped"
    assert r.polygon([(0, 0), (5, 5)]) is None


def test_which_point_a_shape_holds():
    rect, circ = r.rectangle(10, 10, 20, 30), r.circle(0, 0, 3, 4)
    tri = r.polygon([(0, 0), (10, 0), (0, 10)])
    assert r.contains(rect, 15, 25) and not r.contains(rect, 25, 15)
    assert r.contains(circ, 3, 3) and not r.contains(circ, 4, 4)
    assert r.contains(tri, 2, 2) and not r.contains(tri, 8, 8)
    hot = {"panel": 0, "pixels": [[10, 10], [50, 60]]}
    # Within two pixels of one: clicking it exactly, zoomed out, too fiddly.
    assert r.contains(hot, 10.5, 10.5) and r.contains(hot, 52, 62)
    assert not r.contains(hot, 30, 30) and not r.contains(hot, 14, 10)


def test_each_image_sets_regions_changed_and_undone():
    regs = r.Regions({0: [r.circle(5, 5, 8, 9)], 1: []})
    assert not regs.changed
    assert regs.add(1, r.rectangle(0, 0, 10, 10)) and regs.changed
    assert not regs.add(1, None), "nothing drawn, nothing kept"
    assert regs.copy_from(1, 0) and regs.of(1) == regs.of(0)
    assert regs.of(1) is not regs.of(0), "a copy, not the same list"
    assert regs.undo(1) and len(regs.of(1)) == 1 and "rectangle" in regs.of(1)[0]
    assert regs.clear(1) and regs.of(1) == [] and regs.undo(1) and len(regs.of(1)) == 1
    assert (
        regs.remove(0, 0) and regs.of(0) == [] and regs.undo(0) and len(regs.of(0)) == 1
    )
    assert not regs.undo(0), "image set 0's history spent; 1's its own"
    regs.add(0, r.rectangle(0, 0, 20, 20))
    assert regs.at(0, 6, 6) == 1, "the last drawn of two holding it"
    assert regs.at(0, 50, 50) is None

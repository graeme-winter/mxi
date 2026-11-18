"""mxi ui's image viewer on a planted series, where there is a display: the
markers drawn are those mxi_tools.Markers puts on each image, each on its
spot at any zoom and pan; the images stepped through; the readout; an indexed
file's indexed spots apart; the main window's View images."""

import os
import subprocess
import sys
import time

import pytest

tkinter = pytest.importorskip("tkinter")

from test_ui_window import _can_open_a_window  # noqa: E402

BUILD = os.path.dirname(os.environ.get("MXI_FIND", "")) or os.environ.get(
    "MXI_BUILD", ""
)
MXI = os.path.join(BUILD, "mxi")

pytestmark = [
    pytest.mark.skipif(not _can_open_a_window(), reason="no display for a window"),
    pytest.mark.skipif(not os.path.exists(MXI), reason="mxi is not built"),
]


def settle(root, n=10):
    for _ in range(n):
        root.update()
        time.sleep(0.01)


@pytest.fixture
def strong(tmp_path):
    from test_verify import indexed

    indexed(tmp_path)  # the planted series, a whole geometry, its spots found
    return tmp_path


def open_viewer(path):
    from mxi_ui.viewer import Viewer

    root = tkinter.Tk()
    root.withdraw()
    v = Viewer(root, str(path))
    v.top.geometry("800x700")
    # No Fit: opened as a person opens it, fitted by the viewer itself --
    # pressing Fit here had hidden that it was not (Graeme's grey window).
    settle(root, 20)
    return root, v


def drawn_boxes(v, tag):
    return sorted(
        tuple(round(c, 6) for c in v.canvas.coords(i))
        for i in v.canvas.find_withtag(tag)
    )


def wanted_boxes(v, key):
    """Each spot's box on the canvas, as the viewer should draw it: its pixel
    edges a pixel outside, at least 2 MIN_RADIUS about its middle, any part on
    the canvas."""
    from mxi_ui.viewer import MIN_RADIUS, _at_least

    _yx, _own, boxes = v.layers[0][key].on_boxed(v._image() - 1)
    w, h = v._canvas_size()
    out = []
    for bx0, bx1, by0, by1 in boxes:
        x0, y0 = v.view.to_canvas(bx0, by0)
        x1, y1 = v.view.to_canvas(bx1, by1)
        x0, x1 = _at_least(x0 - 1, x1 + 1, 2 * MIN_RADIUS)
        y0, y1 = _at_least(y0 - 1, y1 + 1, 2 * MIN_RADIUS)
        if x1 >= 0 and x0 <= w and y1 >= 0 and y0 <= h:
            out.append(tuple(round(c, 6) for c in (x0, y0, x1, y1)))
    return sorted(out)


def test_each_spot_is_drawn_as_its_box_at_any_zoom(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.image.set(3)
        v.show()
        settle(root)
        for action in (None, "in", "in", "pan", "out"):
            if action == "in":
                v.view.zoom(1.25, 400, 300)
            elif action == "out":
                v.view.zoom(0.8, 400, 300)
            elif action == "pan":
                v.view.pan(37, -21)
            v.draw()
            settle(root, 3)
            drawn, wanted = drawn_boxes(v, "unindexed"), wanted_boxes(v, "unindexed")
            assert drawn == wanted and drawn, action
            # And round the spot in the picture: its brightest pixel inside.
            x0, y0, x1, y1 = drawn[len(drawn) // 2]
            (dx0, dy0), (dx1, dy1) = v.view.to_detector(x0, y0), v.view.to_detector(
                x1, y1
            )
            inside = v.frame[
                max(0, int(dy0)) : int(dy1) + 1, max(0, int(dx0)) : int(dx1) + 1
            ]
            around = v.frame[
                max(0, int(dy0) - 4) : int(dy1) + 5, max(0, int(dx0) - 4) : int(dx1) + 5
            ]
            assert inside.max() == around.max() > 0, action
    finally:
        root.destroy()


def test_the_images_step_within_the_sweep(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        assert (v.first, v.last) == (1, 6) and v._image() == 1
        v.step(-1)
        assert v._image() == 1
        for _ in range(10):
            v.step(1)
        assert v._image() == 6
    finally:
        root.destroy()


def test_the_readout_under_the_cursor(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        cx, cy = v.view.to_canvas(100.5, 80.5)

        class Event:
            x, y = cx, cy

        v._hover(Event)
        text = v.readout.get()
        assert (
            "x    100." in text
            and "y     80." in text
            and "counts" in text
            and " Å" in text
        )
    finally:
        root.destroy()


def test_an_indexed_files_spots_are_their_own_layer(strong):
    run = subprocess.run(
        [
            MXI,
            "convert",
            str(strong / "indexed.expt"),
            str(strong / "indexed.refl"),
            "-o",
            str(strong / "indexed.rflx"),
        ],
        capture_output=True,
        text=True,
        cwd=strong,
    )
    assert run.returncode == 0, run.stderr
    root, v = open_viewer(strong / "indexed.rflx")
    try:
        layers = v.layers[0]
        assert layers.get("indexed") is not None and layers.get("unindexed") is None
        boxes = [c.cget("text") for c in v.toggles.winfo_children()]
        assert boxes == [f"Indexed spots ({len(layers['indexed'])})"]
        v.image.set(3)
        v.show()
        settle(root)
        assert v.canvas.find_withtag("indexed") and not v.canvas.find_withtag(
            "unindexed"
        )
        v.showing["indexed"].set(False)
        v.draw()
        assert not v.canvas.find_withtag("indexed"), "a layer hidden"
    finally:
        root.destroy()


def test_view_images_opens_the_steps_file_or_what_it_reads(strong):
    from mxi_ui.window import MainWindow

    root = tkinter.Tk()
    root.withdraw()
    w = MainWindow(root, str(strong))
    try:
        w.tree.selection_set("find")
        root.update()
        assert w.images_file().endswith("strong.rflx")
        viewer = w.view_images()
        assert viewer is not None and "Find spots" in viewer.top.title()
        w.tree.selection_set("index")
        root.update()
        assert w.images_file().endswith("strong.rflx"), "not run: what it reads"
    finally:
        root.destroy()


def test_view_images_with_nothing_to_view_says_so(tmp_path, monkeypatch):
    from tkinter import messagebox

    from mxi_ui.window import MainWindow

    said = []
    monkeypatch.setattr(messagebox, "showinfo", lambda *a: said.append(a))
    root = tkinter.Tk()
    root.withdraw()
    w = MainWindow(root, str(tmp_path))
    try:
        w.tree.selection_set("find")
        root.update()
        assert w.images_file() is None and w.view_images() is None
        assert said and "run Import first" in said[0][1]
    finally:
        root.destroy()


def test_contrast_and_colours_recolour_without_reducing_again(strong, monkeypatch):
    from mxi_ui import render

    root, v = open_viewer(strong / "strong.rflx")
    try:
        calls = []
        real = render.reduce
        monkeypatch.setattr(render, "reduce", lambda *a: calls.append(1) or real(*a))
        v.ceiling.set(v.ceiling.get() / 2)
        v.draw()
        v.scheme.set("heat")
        v.draw()
        assert calls == [], "the reduction kept"
        v.view.zoom(1.25, 100, 100)
        v.draw()
        assert calls == [1], "a new view reduced afresh"
    finally:
        root.destroy()


def test_zoom_without_a_wheel(strong):
    # Graeme has no wheel: buttons and keys zoom, a level each.
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.top.deiconify()
        settle(root)
        start = v.view.scale
        w, h = v._canvas_size()
        middle = v.view.to_detector(w / 2, h / 2)
        buttons = {
            b.cget("text"): b
            for b in v.top.winfo_children()[0].winfo_children()
            if b.winfo_class() == "TButton"
        }
        buttons["+"].invoke()
        assert v.view.scale < start
        assert all(
            abs(a - b) < 1e-6 for a, b in zip(v.view.to_canvas(*middle), (w / 2, h / 2))
        ), "about the middle"
        buttons["−"].invoke()
        assert v.view.scale == start
        v.top.focus_force()
        settle(root)
        v.top.event_generate("<KeyPress-plus>")
        v.top.event_generate("<KeyPress-equal>")
        settle(root)
        assert v.view.scale < start
        v.top.event_generate("<KeyPress-0>")
        settle(root)
        assert v.view.scale == start, "0 fits"
    finally:
        root.destroy()


def test_a_trackpads_burst_of_scrolls_zooms_a_level_a_tenth_of_a_second(strong):
    from mxi_ui.viewer import WHEEL_INTERVAL

    root, v = open_viewer(strong / "strong.rflx")
    try:
        start = v.view.scale

        class E:
            x, y = 100, 100

        taken = [
            v._wheel(E, 1, now=t)
            for t in (10.0, 10.01, 10.02, 10.05, 10.0 + WHEEL_INTERVAL + 0.01)
        ]
        assert taken == [True, False, False, False, True]
        levels = 0
        s = start
        from mxi_ui import render

        while s > v.view.scale + 1e-9:
            s = render.next_level(s, inwards=True)
            levels += 1
        assert levels == 2, "two levels, not five"
    finally:
        root.destroy()


def test_typing_in_a_box_keeps_the_zoom(strong):
    # 0 was bound to Fit on the whole window: typing image 10 in the image box
    # (a slider since) fitted the view. The Sum box now, typed 10 and -.
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.top.deiconify()
        v.zoom(1)
        v.zoom(1)
        zoomed = v.view.scale
        v.sum_box.focus_force()
        settle(root)
        v.sum_box.event_generate("<KeyPress-1>")
        v.sum_box.event_generate("<KeyPress-0>")
        v.sum_box.event_generate("<KeyPress-minus>")
        settle(root)
        assert v.view.scale == zoomed, "a box's keys are the box's"
        v.canvas.focus_force()
        settle(root)
        v.canvas.event_generate("<KeyPress-0>")
        settle(root)
        assert v.view.scale != zoomed, "on the image, 0 fits"
    finally:
        root.destroy()


def test_a_box_is_its_spots_size(strong):
    from mxi_ui.viewer import MIN_RADIUS

    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.image.set(3)
        v.show()
        _yx, _own, boxes = v.layers[0]["unindexed"].on_boxed(2)
        width = float(boxes[0][1] - boxes[0][0])
        for _ in range(4):
            settle(root, 2)
            x0, _y0, x1, _y1 = v.canvas.coords(v.canvas.find_withtag("unindexed")[0])
            assert abs((x1 - x0) - max(2 * MIN_RADIUS, width / v.view.scale + 2)) < 1e-6
            v.zoom(1)
        assert (x1 - x0) > 8 * MIN_RADIUS, "zoomed in, the spot's own size"
        assert v.canvas.type(v.canvas.find_withtag("unindexed")[0]) == "rectangle"
    finally:
        root.destroy()


def test_a_layer_without_boxes_is_drawn_as_circles(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.image.set(3)
        v.layers[0]["unindexed"].box = None  # as predictions without boxes
        v.show()
        settle(root)
        items = v.canvas.find_withtag("unindexed")
        assert items and {v.canvas.type(i) for i in items} == {"oval"}
    finally:
        root.destroy()


def test_dragging_with_the_right_button_zooms(strong):
    from mxi_ui.viewer import DRAG_LEVEL

    root, v = open_viewer(strong / "strong.rflx")
    try:
        for sequence in (
            "<ButtonPress-2>",
            "<ButtonPress-3>",
            "<Control-ButtonPress-1>",
            "<B2-Motion>",
            "<B3-Motion>",
            "<Control-B1-Motion>",
        ):
            assert v.canvas.bind(sequence), sequence
        start = v.view.scale
        point = v.view.to_detector(200, 300)  # where the drag begins

        class E:
            def __init__(self, y):
                self.x, self.y = 200, y

        v._zoom_press(E(300))
        v._zoom_drag(E(300 - 2 * DRAG_LEVEL - 5))  # up two levels and a bit
        from mxi_ui import render

        assert v.view.scale == render.next_level(render.next_level(start, True), True)
        assert all(
            abs(a - b) < 1e-6 for a, b in zip(v.view.to_canvas(*point), (200, 300))
        )
        v._zoom_drag(E(300 + DRAG_LEVEL))  # back down past where it began
        assert v.view.scale > render.next_level(start, True)
    finally:
        root.destroy()


def test_a_trackpads_scroll_its_vertical_part(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        start = v.view.scale

        class E:
            x, y = 100, 100
            delta = (3 << 16) | 5  # dx 3, dy 5, packed as Tk 9 packs them

        v._touchpad(E)
        assert v.view.scale < start, "dy up: in"
        v._last_wheel = -1e9

        class Down(E):
            delta = (0 << 16) | (0x10000 - 4)  # dy -4

        v._touchpad(Down)
        assert v.view.scale == start
    finally:
        root.destroy()


def test_the_image_is_fitted_when_the_window_opens(strong):
    # No Fit pressed: fitted once at the canvas's first size, a pixel, and
    # never again, the window had opened grey.
    from mxi_ui import render
    from mxi_ui.viewer import Viewer

    root = tkinter.Tk()
    root.withdraw()
    v = Viewer(root, str(strong / "strong.rflx"))
    try:
        v.top.geometry("800x700")
        settle(root, 20)
        w, h = v._canvas_size()
        fitted = render.View((v.view.width, v.view.height), (w, h)).scale
        assert w > 100 and v.view.scale == fitted
        x0, y0, x1, y1 = v.canvas.bbox("image")
        assert x1 - x0 > w / 3 and y1 - y0 > h / 3, "the image fills the window"
        v.image.set(3)  # image 1 has no spots: the planted series' begin on 3
        v.show()
        settle(root)
        assert v.canvas.find_withtag("unindexed"), "its spots on it"
        v.zoom(1)
        zoomed = v.view.scale
        v.top.geometry("700x650")
        settle(root, 20)
        assert v.view.scale == zoomed, "zoomed, a resize keeps the view"
        v.fit()
        v.top.geometry("900x800")
        settle(root, 20)
        assert (
            v.view.scale
            == render.View((v.view.width, v.view.height), v._canvas_size()).scale
        )
    finally:
        root.destroy()


class Click:
    def __init__(self, x, y):
        self.x, self.y = x, y


def drag(v, mode, start, end):
    v.mode.set(mode)
    v._mode_changed()
    v._press(Click(*start))
    v._move(Click(*end))
    v._release(Click(*end))


def test_regions_drawn_where_they_are_drawn(strong):
    import math

    root, v = open_viewer(strong / "strong.rflx")
    try:
        ims = v._imageset()
        assert v.regions.of(ims) == [], "the planted series has none"
        drag(v, "rectangle", (100, 120), (60, 80))
        (a, b), (c, d) = v.view.to_detector(60, 80), v.view.to_detector(100, 120)
        assert v.regions.of(ims)[0]["rectangle"] == [
            round(a, 1),
            round(b, 1),
            round(c, 1),
            round(d, 1),
        ]
        drag(v, "circle", (300, 300), (330, 340))
        assert math.isclose(
            v.regions.of(ims)[1]["circle"][2], 50 * v.view.scale, abs_tol=0.1
        )
        v.mode.set("polygon")
        v._mode_changed()
        for p in ((400, 100), (500, 100), (450, 200)):
            v._press(Click(*p))
            v._release(Click(*p))
        v._close_polygon()
        assert len(v.regions.of(ims)[2]["polygon"]) == 3
        settle(root, 2)
        assert len(v.canvas.find_withtag("region")) == 3, "all three drawn"
        assert v.masking_text.get().split(" (")[0] == "Masking: 3, not saved"
        # Look: a click chooses the region under it; Delete removes it; Undo.
        v.mode.set("look")
        v._press(Click(80, 100))
        v._release(Click(80, 100))
        assert v.chosen == 0
        v.delete_chosen()
        assert len(v.regions.of(ims)) == 2 and "rectangle" not in v.regions.of(ims)[0]
        v.undo()
        assert len(v.regions.of(ims)) == 3
        v.view.zoom(1.25, 200, 200)
        v.draw()
        x0, y0, x1, y1 = v.canvas.coords(v.canvas.find_withtag("region")[0])
        assert (
            abs(x0 - v.view.to_canvas(a, b)[0]) < 0.2
            and abs(x1 - v.view.to_canvas(c, d)[0]) < 0.2
        ), "where drawn, zoomed"
    finally:
        root.destroy()


def test_saved_through_mxi_untrusted_and_find_spots_reads_it(strong, monkeypatch):
    import json

    from tkinter import messagebox

    import mxi_tools
    from mxi_ui.window import MainWindow

    monkeypatch.setenv("MXI", MXI)  # as mxi ui finds it, and so mxi_tools
    root = tkinter.Tk()
    root.withdraw()
    w = MainWindow(root, str(strong))
    try:
        w.tree.selection_set("find")
        root.update()
        v = w.view_images()
        v.top.geometry("800x700")
        settle(root, 20)
        drag(v, "rectangle", (60, 80), (100, 120))
        drag(v, "circle", (300, 300), (330, 340))
        wanted = v.regions.of(v._imageset())
        before = (strong / "strong.rflx").read_bytes()
        masked = v.save()
        assert (
            masked == str(strong / "masked.rflx")
            and (strong / "mxi_mask.json").exists()
        )
        assert (
            strong / "strong.rflx"
        ).read_bytes() == before, "the file shown as it was"
        saved = mxi_tools.sweeps(mxi_tools.read_experiments(masked))[0]["shapes"]
        assert saved == wanted, "read back as drawn"
        assert json.load(open(strong / "mxi_mask.json"))["imagesets"]["0"] == wanted
        assert w._form_of(w.selected)["inputs"].get() == "masked.rflx"
        assert "Find spots reads it" in w.status.get()
        assert v.masking_text.get().split(" (")[0] == "Masking: 2"
        # Unsaved, closing asks; saved, it does not.
        asked = []
        monkeypatch.setattr(
            messagebox, "askyesno", lambda *a, **k: asked.append(a) or False
        )
        v.close()
        assert asked == [] and not v.top.winfo_exists()
        v2 = w.view_images()
        settle(root, 10)
        drag(v2, "rectangle", (10, 10), (50, 50))
        v2.close()
        assert asked and v2.top.winfo_exists(), "unsaved: asked, and kept"
    finally:
        root.destroy()


def test_undo_by_its_key(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.top.deiconify()
        drag(v, "rectangle", (60, 80), (100, 120))
        assert len(v.regions.of(v._imageset())) == 1
        v.canvas.focus_force()
        settle(root, 3)
        v.canvas.event_generate("<Control-z>")
        settle(root, 3)
        assert v.regions.of(v._imageset()) == [], "Ctrl-Z undid it"
    finally:
        root.destroy()


def test_images_summed_from_the_one_shown(strong):
    import numpy as np

    root, v = open_viewer(strong / "strong.rflx")
    try:
        images = v.images[v.sweeps[0]["master"]]
        singles = [images.frame(i).copy() for i in range(6)]
        v.image.set(2)
        v.summing.set(3)
        v.show(fit_contrast=True)
        assert np.array_equal(
            v.frame, singles[1] + singles[2] + singles[3]
        ), "images 2 to 4"
        # The sum stops at the sweep's last image, and says so.
        v.image.set(5)
        v.summing.set(10)
        v.show()
        assert v.summing.get() == 2 and np.array_equal(v.frame, singles[4] + singles[5])

        class E:
            pass

        E.x, E.y = v.view.to_canvas(100.5, 80.5)
        v._hover(E)
        assert v.readout.get().startswith("images 5 to 6")
    finally:
        root.destroy()


def test_a_sums_spots_are_those_on_any_of_its_images(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.image.set(1)
        v.summing.set(1)
        v.show()
        settle(root, 2)
        alone = len(v.canvas.find_withtag("unindexed"))
        assert alone == 0, "image 1 has no spots"
        v.summing.set(3)
        v.show(fit_contrast=True)
        settle(root, 2)
        _yx, _own, boxes = v.layers[0]["unindexed"].on_boxed(0, 3)
        drawn = len(v.canvas.find_withtag("unindexed"))
        assert drawn == len(boxes) > 0, "image 3's spots, in images 1 to 3's sum"
    finally:
        root.destroy()


def test_the_contrast_refits_to_a_sum(strong):
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.image.set(3)
        v.summing.set(1)
        v.show(fit_contrast=True)
        one = v.ceiling.get()
        v.summing.set(4)
        v.show(fit_contrast=True)
        assert v.ceiling.get() > one, "summed counts, a higher top"
    finally:
        root.destroy()


def test_the_viewer_closes_by_its_button_asking_first_where_unsaved(
    strong, monkeypatch
):
    from tkinter import messagebox

    asked = []
    monkeypatch.setattr(
        messagebox, "askyesno", lambda *a, **k: asked.append(a) or False
    )
    root, v = open_viewer(strong / "strong.rflx")
    try:
        close = [
            b
            for b in v.top.winfo_children()[0].winfo_children()
            if b.winfo_class() == "TButton" and b.cget("text") == "Close"
        ]
        assert close
        drag(v, "rectangle", (60, 80), (100, 120))
        close[0].invoke()
        assert asked and v.top.winfo_exists(), "unsaved: asked, and kept"
        v.regions.changed = False
        close[0].invoke()
        assert not v.top.winfo_exists()
    finally:
        root.destroy()


def test_several_lattices_a_colour_each(strong):
    # Graeme: with more than one lattice, the boxes in different colours. A
    # second lattice on the planted sweep, every other spot its.
    import json

    import numpy as np
    from mxeq import refl

    from mxi_ui.viewer import LATTICE_COLOURS

    doc = json.load(open(strong / "indexed.expt"))
    second = dict(
        doc["experiment"][1],
        crystal=1,
        identifier="1c0ffee0-0000-4000-8000-0000000000a2",
    )
    doc["experiment"].append(second)
    doc["crystal"].append(dict(doc["crystal"][0]))
    json.dump(doc, open(strong / "two.expt", "w"))
    table = refl.load(str(strong / "indexed.refl"))
    ids = np.asarray(table.columns["id"]).copy()
    ids[::2] = 2
    table.columns["id"] = ids.astype(table.columns["id"].dtype)
    table.identifiers = {1: doc["experiment"][1]["identifier"], 2: second["identifier"]}
    refl.write(str(strong / "two.refl"), table)
    run = subprocess.run(
        [MXI, "convert", "two.expt", "two.refl", "-o", "two.rflx"],
        capture_output=True,
        text=True,
        cwd=strong,
    )
    assert run.returncode == 0, run.stderr
    root, v = open_viewer(strong / "two.rflx")
    try:
        keys = [key for key, _label, _colour in v.styles[0]]
        assert keys[:2] == ["lattice1", "lattice2"] and "indexed" not in keys
        colours = {key: colour for key, _label, colour in v.styles[0]}
        assert colours["lattice1"] == LATTICE_COLOURS[0][0]
        assert colours["lattice2"] == LATTICE_COLOURS[1][0]
        boxes = [c.cget("text") for c in v.toggles.winfo_children()]
        assert boxes[0].startswith("Lattice 1 (") and boxes[1].startswith("Lattice 2 (")
        v.image.set(3)
        v.show()
        settle(root)
        for key in ("lattice1", "lattice2"):
            drawn = v.canvas.find_withtag(key)
            assert drawn, key
            assert {v.canvas.itemcget(i, "outline") for i in drawn} == {
                colours[key]
            }, key
        v.showing["lattice2"].set(False)
        v.draw()
        assert v.canvas.find_withtag("lattice1") and not v.canvas.find_withtag(
            "lattice2"
        )
    finally:
        root.destroy()


def test_the_scan_slider_through_the_sweeps_images(strong, monkeypatch):
    # Graeme: a slider, not the number box, to see where the image is among
    # all the sweep's, under the toolbar. Its range the sweep's; moved,
    # that image shown; following the arrows; a fast drag drawn once, at its
    # latest place, not once an event.
    import numpy as np

    root, v = open_viewer(strong / "strong.rflx")
    try:
        assert (
            float(v.scan.cget("from")) == v.first and float(v.scan.cget("to")) == v.last
        )
        v._scanned("4.3")
        settle(root)
        assert v.image.get() == 4 and v.scan_at.get() == 4
        assert v.typed.get() == "4" and v.of.cget("text") == f"of {v.first} to {v.last}"
        s = v.sweeps[v.sweep.get()]
        assert np.array_equal(v.frame, v.images[s["master"]].frame(3)), "image 4 drawn"
        v.step(1)
        assert v.image.get() == 5 and v.scan_at.get() == 5, "the slider follows"
        shown = []
        real = v.show
        monkeypatch.setattr(
            v, "show", lambda *a, **k: (shown.append(v.image.get()), real(*a, **k))
        )
        for place in ("1", "2", "3", "6"):
            v._scanned(place)
        settle(root)
        assert v.image.get() == 6 and shown == [6], shown
        v._scanned("99")
        settle(root)
        assert v.image.get() == v.last, "held to the sweep"
    finally:
        root.destroy()


def test_the_image_number_typed(strong):
    # Graeme: the image's number editable beside the slider. Typed and Return,
    # that image, the slider following; past the sweep, held to it; not a
    # number, or Escape, the image shown again.
    root, v = open_viewer(strong / "strong.rflx")
    try:
        v.top.deiconify()  # keys go to the focused field of a window shown
        v.number.focus_force()
        settle(root)
        v.typed.set("5")
        v.number.event_generate("<Return>")
        settle(root)
        assert v.image.get() == 5 and v.scan_at.get() == 5 and v.typed.get() == "5"
        v.typed.set("99")
        v._typed()
        assert v.image.get() == v.last and v.typed.get() == str(v.last)
        v.typed.set("two")
        v._typed()
        assert v.image.get() == v.last and v.typed.get() == str(v.last), "unchanged"
        v.number.focus_force()
        settle(root)
        v.typed.set("3")
        v.number.event_generate("<Escape>")
        settle(root)
        assert v.typed.get() == str(v.last) and v.image.get() == v.last
    finally:
        root.destroy()


def test_the_masking_menu(strong):
    # Graeme: the masking tools a menu, taking no row from the image. Its
    # entries: the four modes, undo, clear, the previous sweep's, save; a mode
    # chosen from it is what a click draws, its hint in the readout, its name
    # in the menu's label beside the regions' count.
    from mxi_ui.viewer import masking_label

    root, v = open_viewer(strong / "strong.rflx")
    try:
        menu = v.masking_menu
        labels = [
            menu.entrycget(i, "label")
            for i in range(menu.index("end") + 1)
            if menu.type(i) != "separator"
        ]
        assert labels == [
            "Look",
            "Rectangle",
            "Circle",
            "Polygon",
            "Undo",
            "Clear",
            "Same as previous",
            "Save",
        ]
        assert v.masking_text.get() == "Masking"
        menu.invoke(labels.index("Rectangle") + 0)  # no separator before it
        assert (
            v.mode.get() == "rectangle"
            and v.masking_text.get() == "Masking (Rectangle)"
        )
        assert v.readout.get().startswith("Rectangle: drag corner to corner")
        drag(v, "rectangle", (60, 80), (100, 120))
        assert v.masking_text.get() == "Masking: 1, not saved (Rectangle)"
        undo = [
            i
            for i in range(menu.index("end") + 1)
            if menu.type(i) != "separator" and menu.entrycget(i, "label") == "Undo"
        ][0]
        menu.invoke(undo)
        assert v.masking_text.get().startswith("Masking: 0"), "undone"
        assert masking_label(0, False, "look") == "Masking"
        assert masking_label(2, False, "look") == "Masking: 2"
        assert masking_label(0, True, "polygon") == "Masking: 0, not saved (Polygon)"
    finally:
        root.destroy()


def test_hot_pixels_are_drawn_and_seen_however_far_out(strong):
    # mxi find --mask-hot writes single pixels to the untrusted regions; each
    # is drawn as its own square, never under seven screen pixels across.
    root, v = open_viewer(strong / "strong.rflx")
    try:
        ims = v._imageset()
        v.regions.add(ims, {"panel": 0, "pixels": [[4, 4], [200, 100], [379, 250]]})
        v.draw()
        settle(root, 2)
        marks = v.canvas.find_withtag("region")
        assert len(marks) == 3, "one a pixel"
        for mark in marks:
            x0, y0, x1, y1 = v.canvas.coords(mark)
            assert x1 - x0 >= 7 and y1 - y0 >= 7
    finally:
        root.destroy()

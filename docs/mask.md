# Viewing the images, and masking the detector: mxi-view and mxi untrusted

A backstop, its arm, a cryo nozzle, the goniometer: whatever shadows the
detector makes observations there wrong -- low where the spot is shadowed, high
where its background was -- and nothing in the detector's own pixel mask knows
of it. On a small molecule refined to R1 0.026, most of the observations beyond
5 sigma against Fc^2 lay in the backstop's shadow and along its arm
(`docs/outstanding.md`, item 78; `mxeq truth`). These two programs mark such
regions **untrusted**, so that spot finding and integration leave them out.

## Viewing and drawing them: mxi-view, or mxi-ui

```sh
mxi import ../data/*.nxs
mxi-view imported.rflx        # Masking, Save: writes masked.rflx
mxi find masked.rflx
mxi-view strong.rflx          # the strong spots on the images
```

`mxi-view` opens mxi-ui's image viewer on a file, in a window of its own; in
`mxi-ui` it is **View images**, for the step chosen (`docs/ui.md`, The
images). A sweep at a time, where there are several; on each image, whatever
the file holds: the strong spots, indexed apart from unindexed after indexing
-- several lattices a colour each -- and the predictions, each drawn as its
box on exactly the images its box spans. The image shown is a slider under the
toolbar, where it is among the sweep's at a glance; drag it to scan through
them. Zoom with + and −, the keys + and -, a right-button drag or a scroll;
pan by dragging; contrast and colour maps; under the cursor, the pixel, its
counts and its resolution.

To find a shadow, raise **Sum** to 10 or 50: that many images summed from the
one shown, the shadow showing against the summed background; the spots on
any of them marked. Then, from the toolbar's **Masking** menu -- the tools for
untrusted regions, taking no row from the image -- choose what to draw:

* **Rectangle**, dragged corner to corner;
* **Circle**, dragged from its centre to its edge;
* **Polygon**, a click at each vertex, a double-click or **Return** to close
  it, **Escape** to abandon it.

Shift-drag pans while drawing. In **Look**, a click chooses a region, and
**Delete** removes it. **Undo** (Ctrl-Z, Cmd-Z on a Mac) puts the sweep's
regions back as they were before the last change, a step at a time, each
sweep its own; **Clear** removes the sweep's; **Same as previous** copies
those of the sweep shown before, for sweeps that share their shadows. The
regions a file holds already are shown, to change or add to. **Save** writes
the experiment list with every sweep's regions to a new file, `masked.rflx`
(`mxi-view -o` to choose another), and keeps them in `mxi_mask.json` -- in the
directory `mxi-view` is run in, or beside the file `mxi-ui` shows, where Find
spots is then set to read it. The input is left as it was, to compare with:
masked and not, process `masked.rflx` from spot finding on -- `mxi process masked.rflx`, with
its usual options, does so, whatever file was masked. Closing with regions not saved asks first.

`mxi-view`'s options: `-o FILE`, where Save writes; `--sum N`, images summed
to begin with; `--sweep K`, the sweep shown first. It needs what mxi-ui
needs -- Python with tkinter, numpy, h5py and hdf5plugin -- and says what is
missing. It is built and installed with `mxi`, and finds `mxi` -- which it
runs as `mxi untrusted` to save -- beside itself. Through 1.1.4 it was napari's
viewer, retired since.

## Where they are kept, and applying them again

Each image set of the experiment list keeps its own:

```json
"untrusted": [{"panel": 0, "circle": [729, 866, 40]},
              {"panel": 0, "rectangle": [720, 0, 760, 866]},
              {"panel": 0, "polygon": [[0, 0], [9, 0], [0, 9]]},
              {"panel": 0, "pixels": [[3192, 1741], [3251, 2077]]}]
```

in pixels as DIALS counts them, pixel i spanning i to i + 1: a pixel is
untrusted when its centre is inside a region. Every program carries the image
sets forward whole, so the regions go with the data from the imported file to
the scaled one; `mxi find` and `mxi integrate` each say how many pixels they
left out ("Untrusted regions: 1 circle, 1 rectangle, 12345 pixels").

`mxi untrusted` lists, sets and clears them with no window -- the mask kept
from one run applied to the next:

```sh
mxi untrusted imported.rflx                       # list each image set's
mxi untrusted imported.rflx --set mxi_mask.json   # set them: masked.rflx
mxi untrusted masked.rflx --clear -o clear.rflx   # remove them
```

Setting or clearing writes a new file, `masked.rflx` unless `-o` names another;
the input is changed only when named. A mask file is `{"imagesets": {"0": [...],
"1": [...]}}` for the image sets it
names, or a list of shapes alone for every image set.

## Hot pixels

A pixel signal on too many images -- on every one, or twinkling, hot every other
image -- is a fault of the detector's, not diffraction (Graeme, 16 November,
after his DIALS tool). `mxi find` counts how many images each pixel is signal on
as it goes, at no measurable cost, and says how many reach the share given
(`--hot-fraction`, 0.25):

```
Hot pixels: 17 on >= 25% of the 3600 images (--hot-fraction): --mask-hot to exclude and mask them
```

With `--verbose` every one is listed, slow then fast, with its share of the
images. With `--mask-hot` they are written to the image set's untrusted regions
as a list of single pixels, the fourth kind beside circles, rectangles and
polygons, and every later program leaves them out.

`mxi untrusted FILE.rflx --hot` is for the mask alone: it reads every image as
spot finding does, its own output to `mxi_find.log`, not the terminal, and says
only the hot pixels -- every one, slow then fast, with its share -- and writes
them into the image set's regions (`masked.rflx`). The order is the detector's
own: an Eiger's pixel mask is indexed m[slow, fast], so that marking them there
takes the pairs as they are (Graeme, 18 November). `--hot-list FILE` writes them
as JSON as well, [[slow, fast], ...]. The untrusted regions keep them [x, y] --
fast, slow -- as every region is kept.

Judged from 100 images only: a reflection is signal on every image its rocking
curve covers, and of a short series that can be a large share -- a spot three
images wide is a tenth of thirty. From 100 at 25% a pixel must be signal on 25
images, more than any reflection spans but at very fine slicing, which would
make a pixel hot too. In spot finding they mostly do no harm: one on every image
is a column through them all, removed as too large; one every other image,
single pixels removed as too small. In a reflection's box integration would take
one at its word, which is what the mask is for. `mxi-view` draws each, never
under seven screen pixels across, so that zoomed out they still show.

## How they are applied

Rasterised once a sweep (`src/untrusted.cc`), the regions join the detector's
own pixel_mask in the image series (`Series::add_masked`), so every frame read
afterwards carries them: each such pixel becomes the bad-pixel marker, which
spot finding and integration already take for no measurement, on the CPU and
on a device alike. `mxi max` leaves them in, to show what was masked. Panel 0
only, as frames are read here; a region on another panel is refused.

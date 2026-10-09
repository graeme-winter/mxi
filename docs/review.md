# Reviewing this code: a guide for DIALS developers

STATUS: written 30 September 2026 for external review, brought up to date 6
October 2026. It says what this is,
how to build and run it on your own data, where the code for each step lives,
what follows DIALS and where it departs from it, and how correctness has been
established. Every claim points at the document with the evidence;
`docs/README.md` maps the documents, and `docs/outstanding.md` is the one list
of open work.

## What this is

A C++20 implementation of rotation-data processing for **one sweep or several
of one crystal** (`docs/multi-sweep.md`), step for step as DIALS divides it, with the spot finder's threshold also on the GPU
(Metal and CUDA):

    mxi_find -> mxi_index -> mxi_refine -> mxi_integrate -> mxi_symmetry -> mxi_scale -> mxi_export

It reads and writes DIALS' own `.expt` and `.refl`, so any step can be replaced
by its `dials.*` counterpart and the rest carries on: `dials.scale` has scaled
this code's integrated tables, and `dials.merge` and `dials.estimate_resolution`
have read its scaled ones. It is **not a fork of DIALS** and depends on neither
DIALS, dxtbx nor cctbx. Where it follows a DIALS algorithm the DIALS source was
read for it, and the file says which.

Its dependencies: HDF5, with bitshuffle and LZ4 vendored for the Eiger's
compression; gemmi for space groups (MPL 2.0, a submodule, unmodified); FFTW
optionally for indexing's transform (GPL, off by default -- the default build
uses a built-in radix-2); CUDA or Metal optionally for the spot finder. The
package is BSD 3-clause.

**How it was written.** Every commit is authored by Graeme Winter with Claude
(Anthropic) as co-author, and says so. The direction, the data and the decisions
-- which DIALS behaviour to follow, which to depart from -- are Graeme's;
`CLAUDE.md` is the working notes those sessions keep: invariants, conventions
that caused bugs, and what not to do again. It is worth reading for the same
reason a DIALS developer's notes would be.

Its size, excluding third-party code: the library 15350 lines in `src/` and the
spot finder 6339 in `src/spots/`; eleven programs in 4273 lines in `apps/`, and
`mxi_find` in `src/spots/find_spots.cc`; C++ tests 14465 lines; a Python package
of comparison tools, `mxeq`, 5950 lines, with 2852 of tests; 5400 lines of
documents. Some 349 commits over three weeks.

## Building it and running it on your data

```sh
git clone --recursive <this repository> && cd mxi
cmake -S . -B build && cmake --build build -j
(cd build && ctest)              # the ctest suites, the spot finder's among them
build/mxi_tests                  # 297 unit tests
build/mxi_tests parallel         # only those whose name contains 'parallel'
```

Add `-DSPOTFINDER_METAL=ON` or `-DSPOTFINDER_CUDA=ON` for the GPU threshold and profile fitting,
`-DMXI_FFTW=ON` for FFTW. The `README.md` has the details and the dependencies
by platform.

The chain on your own data needs nothing of DIALS -- `mxi_import` reads the
NXmx master, and `dials.import`'s `imported.expt` serves as well; every
program answers `--help`, takes `--timing`, and mirrors its output to
`mxi_<program>.log`:

```sh
mxi_import    master.nxs                                 # imported.expt
mxi_find      imported.expt -o strong.refl               # --gpu for the GPU
mxi_index     imported.expt strong.refl                  # indexed.expt, .refl
mxi_refine    indexed.expt indexed.refl --analytic   # scan-varying, one point per 10 degrees
mxi_integrate refined.expt refined.refl                  # --gpu: fitting on the GPU, in float
mxi_symmetry  integrated.expt integrated.refl            # symmetrized.expt, .refl
mxi_scale     symmetrized.expt symmetrized.refl          # scaled.expt, .refl
mxi_export    scaled.expt scaled.refl                    # scaled.mtz, unmerged
```

Beside a DIALS run of the same data, `mxeq` (in `python/`) compares the two at
each boundary -- `mxeq check strong|indexed|refined|integrated|scaled <dials>
<this>` -- and explains where integrations differ; with no second program,
`mxeq equivalents` measures integration's bias against the data's own
equivalents, and `mxeq unique` compares two scaled data sets one unique
reflection at a time. It needs no cctbx.

On the 3600 images of an EIGER2 XE 16M sweep -- `ins10_1.nxs` of
https://zenodo.org/records/8376818 -- on an M4 Max MacBook, the chain from the
images to scaled data, `mxi_find -j 16 --gpu`, `mxi_index`, `mxi_refine --beam
--analytic`, `mxi_integrate --gpu`, `mxi_symmetry` and `mxi_scale --d-min-auto`,
takes 36.6 s of wall time and 3m42 of CPU (30 September 2026).

## Where to find things

| step | library | program | reference document |
|---|---|---|---|
| import | `nxmx_import` (the NXmx geometry, its transformation chains) | `mxi_import` | `docs/import.md` |
| spot finding | `src/spots/` -- `dext.cc` the threshold on the CPU, `dext_metal.cc` and `dext_cuda.cu` on the GPU, `dext_fused.hh` a fused CUDA kernel, `decompress.cc`, `nxmx.cc`, `find_spots.cc` the program | `mxi_find` | `docs/spots.md` |
| geometry | `geometry` (models, conventions), `predict`, `derivatives`, `derivatives_t` | | `docs/conventions.md` |
| indexing | `index` (3D FFT), `fft`, `fft_fftw` | `mxi_index` | `CLAUDE.md`, indexing sections; `docs/gpu.md`, Indexing |
| refinement | `refine`, `target`, `linalg` | `mxi_refine` | `CLAUDE.md`, refinement sections; `docs/gpu.md`, Refinement |
| integration | `integrate` (summation), `background` (GLM), `shoebox`, `mask`, `profile_model`, `profile_grid`, `reference` (profiles and fitting), `postrefine`; `fit_device.hh`, `fit_batch`, `fit_cuda.cu`, `fit_metal.metal` and `fit_device_*.cc` (fitting on a GPU, `--gpu`) | `mxi_integrate` | `docs/integration.md`; `docs/gpu.md`, Integration |
| symmetry | `symmetry` (space groups, via gemmi), `laue` | `mxi_symmetry` | `docs/symmetry.md` |
| scaling | `scale`, `scale_model`, `resolution` | `mxi_scale` | `docs/scaling.md` |
| formats and plumbing | `expt`, `refl` (msgpack by hand; HDF5 tables read, DIALS's of dials/dials#3255 and dxtbx-h5's, shoeboxes and LZ4 included, `refl_hdf5`), `json`, `args`, `log_mirror`, `timing`, `parallel`, `summary` | | |

Library files are `src/<name>.hh` and `.cc`. Six more programs are for looking
at a step rather than doing it: `mxi_profile` estimates the profile model,
`mxi_mask` marks integration regions, `mxi_grid` and `mxi_forward` show spots in
Kabsch space and rendered onto the pixels, `mxi_residuals` decomposes centroid
residuals, and `mxi_background` fits backgrounds to given pixels.

The spot finder was a repository of its own and keeps its own `.expt` reader
and `.refl` writer in `src/spots/`; `docs/spotfinder.md` is how it came in, and
merging the duplicates is item 30 of `docs/outstanding.md`.

## What follows DIALS, and where it departs

Each departure is deliberate and measured, and the measurement is in the
document named. Where two were compared on the same data the numbers are
given.

**Spot finding** is the extended dispersion threshold, DIALS' default, three
windows of 7 x 7, 5 x 5 and 11 x 11. On a 300 image insulin sweep every
filtering count matches `dials.find_spots` exactly; CUDA and Metal agree with
each other exactly on 3600 frames. Departures (`docs/spots.md`, "Where this will
not match dials.find_spots"): the dispersion test in single precision where
DIALS uses double, so a spot at the threshold can fall either side. The
detector's own pixel mask is read, as dials.import reads it, since October
2026: until then only the sentinel values in the data were, and a defective
pixel recording large counts was data -- on a small molecule of Graeme's, two
observations at -25 sigma from one under their backgrounds.

**Indexing** is the three-dimensional FFT (Bricogne 1986), as DIALS' `fft3d`,
its largest cell estimated as `find_max_cell` does -- ice rings and
overlapping boxes left out, nearest neighbours by sweep, 45 degree block and
entering or not, the edge of their histogram's peak times 1.3. The margin
matters: the median times 1.5, as it was, let a small molecule with a second
lattice in it index in a supercell twice the true one's volume. And it has a
macrocycle of assignment and refinement. The transform, its candidates and the
basis follow fft3d's in three more ways, each found missing on a cell of 43.6,
43.6, 212 A that dials.index indexed and mxi_index did not: the reciprocal grid
a step of 1 / (2.5 max_cell), as fft3d's, and no point beyond its reach, where
1 / (2 max_cell) put c* 2.7 steps apart and the points beyond wrapped round;
candidate peaks a whole multiple of a stronger one dropped, where 2a to 6a had
filled the 30 and left c out; and the basis chosen as dials.index's filtering
ranker chooses, the smallest cell of those indexing 90 per cent of the best,
where the most indexed had won and a supercell, taking in strays, indexes a
little more. Then the macrocycles: assigning once admitted 380
weak misindexed reflections that moved the refined distance by 0.27 mm without
producing a single outlier (`CLAUDE.md`, "Where the cell and distance difference
from DIALS actually came from"). Given DIALS' own indexed reflections, this
refinement reproduces DIALS' distance to two microns and cell to six
thousandths of an angstrom.

**Refinement** has analytical derivatives for crystal, detector and beam,
checked against finite differences. The scan-varying crystal is a **cubic
B-spline** of control points, not DIALS' Gaussian smoother: both are smooth and
local, and they are not the same model. A file carries samples at image
boundaries, N + 1 of them, and those are interpolated linearly as DIALS does;
three bugs came from mixing the two up (`CLAUDE.md`, "Scan points are
boundaries"). Outlier rejection is milder than DIALS' -- 11714 reflections kept
against 9580 on one sweep -- and is item 12.

**Integration** (`docs/integration.md`):

* The background is the robust Poisson GLM of Parkhurst et al. (2016), as
  DIALS'; summation after Leslie (1999); the profile model and reference
  profiles after Kabsch (2010).
* **The profile fit is against the shoebox's pixels**, the reference profile
  interpolated between the neighbouring cells and carried onto them, not on
  the reciprocal-space grid: fitting on the grid treats its points as
  independent when one pixel reaches several, and at high resolution claimed a
  variance 0.35 of the summed one, where DIALS has 0.84.
* **One pass over the images.** A reflection is fitted as soon as the scan
  blocks its profile is interpolated from are complete, its shoebox held until
  then; `--two-pass` reads twice, and the two give the same bytes.
* **A reference cell with too few spots borrows its own scan block's average**
  across the detector, not the whole scan's; and a scan block is 10 degrees by
  default. Both since 29 September 2026, and measured before and after.
* **`--postrefine`** refines against the centres integration measures and
  integrates again: refining against the spot finder's centres leaves a z
  offset of about 0.1 images, varying with strength and orientation, which
  DIALS' integration shows too.
* Reflections crossing a module gap: judged by their clean symmetry
  equivalents, 0.967 here and 0.777 in DIALS where a tenth to a third of the
  reflection is lost.
* Through `dials.scale`, on 1800 images of insulin: Rmerge 0.038 and Rpim 0.009
  on this output and on DIALS' alike.

**Symmetry** (`docs/symmetry.md`) follows `dials.symmetry`: the lattice's
symmetry by Le Page's method through gemmi, each element scored and each
subgroup after Evans (2011), to a resolution limit from CC half, Friedel mates
apart; then the space group by absences. Departures: intensities normalised in
resolution shells where DIALS fits an anisotropic maximum-likelihood model (the
identity's CC 0.858 against 0.931 on one sweep, both choosing I m -3); monoclinic
groups named C 1 2/m 1, the reference setting, where DIALS chooses beta nearest
90; and on one cubic sweep the two chose opposite sides of the indexing
ambiguity, both right for one sweep.

**Scaling** (`docs/scaling.md`) is dials.scale's physical model --
Beilsten-Edmands et al. (2020) -- with cubic B-splines for scale and decay and
spherical harmonics for absorption, fitted by Levenberg-Marquardt with the
variable-projection Jacobian throughout. Departures, each argued and measured
there:

* **The error model's deviations take the exact variance** of a difference from
  a mean the observation is part of. dials.scale's form biases a low by
  (n-1)/n: a planted 1.3 comes back 1.158 in groups of eight and 0.662 in pairs.
* **The scale factors' uncertainty is propagated into the variance before the
  error model**, I^2 var(g) / g^2, where dials.scale multiplies by 1 + sigma_g /
  g after it.
* **The resolution estimate is dials.estimate_resolution's**, and agrees with it
  exactly on 1082994 reflections (1.25 and 1.17 A), except that only bins with
  pairs enough to fit decide whether CC half stays above the limit throughout.
  `--d-min-auto` scales, estimates and scales again to the limit.

**Speed** is measured, not assumed, and the tables are in the documents. On a
MacBook the whole chain took 59 s on 3600 frames of an Eiger 16M, and
integrating 1800 frames of a smaller detector 6.6 s, since made faster. Every
phase of every program is reported by `--timing`.

## What is not here yet

The chain is complete for one sweep and for several of one crystal indexed
together; beyond it (`docs/outstanding.md` has the numbered list):

* **Sweeps indexed apart**, and so the indexing ambiguity between them --
  sweeps indexed together are in one basis, each then refining a crystal of its
  own. Multi-sweep processing works on one sweep split in two and, by Graeme,
  on the four sweeps of cubic insulin at their own orientations
  (https://zenodo.org/records/8376818; `docs/multi-sweep.md`).
* Free-set validation of scaling; overlapping
  reflections and overloads in integration; e.s.d.s for the cell, and the cell
  constrained to the lattice's symmetry, in refinement.
* Reports and merged output: an HTML report, merged MTZ and mmCIF are not
  written; `mxi_export` writes the unmerged MTZ, which AIMLESS reads
  (`docs/export.md`), and `dials.merge` takes the scaled table.
* **Known biases in integration**, measured by `mxeq equivalents` against the
  data's own symmetry equivalents (`python/README.md`): reflections crossing a
  module gap low in profile fitting; on insulin, partials high with a sigma_m
  wider than DIALS's, not repeated on ferritin (`docs/outstanding.md`, items 1
  to 3).
* **The backstop** (`docs/backstop.md`, items 52 and 53): observations its
  shadow attenuates integrate low, and scaling's outlier rejection keeps them
  over the unharmed, so the lowest-resolution reflections merge too low.
  `--d-max` leaves them out, and `mxeq` prototypes filter them; paused, none
  a default, for on ferritin they did not measurably improve a refined
  structure, and no absolute measure of processing quality yet decides it.

## What this work found in DIALS

Measured, and listed with their evidence at the end of `docs/outstanding.md`:
dials.scale's error model biased low by (n-1)/n; the anomalous slope and dI/s(dI)
inflated by it, 1.890 and 1.476 on a sweep with no measurable anomalous signal;
the scale-factor variance applied as a factor after the error model rather than
propagated; and refinement against the spot finder's centres leaving a z offset
that depends on strength and orientation.

## How correctness is established

* **Against DIALS, on the same data**, at every boundary, with the numbers in the
  documents -- and against the data themselves where DIALS cannot judge, such as
  a gap-crossing reflection against its clean symmetry equivalents.
* **Byte for byte, whenever an answer should not move.** A change that should not
  alter a result is shown not to: `integrated.refl`, `symmetrized.refl` and
  `scaled.refl` before and after, one pass against two, one thread against
  sixteen. Work is cut into fixed blocks and combined in block order, so no
  answer depends on the thread count (`src/parallel.hh`). Between compilers it
  may in the last bits: a compiler may fuse multiply-adds, rounding once where
  the source rounds twice -- Clang on ARM does by default, for speed -- so a Mac
  and Linux can differ at about 1e-11, and tests comparing two computations of
  one quantity allow for that rather than ask for bytes.
* **Tests that are shown to fail.** A new test is run against the bug it guards
  -- the fix taken out, or the bug put back -- before it is trusted; the commit
  messages record it. 297 C++ unit tests, 10 ctest suites, 230-odd Python tests,
  and end-to-end tests on real images that run when the data are named.
* **The documents are tested too**: `python/tests/test_documents.py` fails on any
  path or program a document names that does not exist.
* **Each commit says what was found and how it was checked**, so `git log` is the
  lab notebook; `docs/integration_history.md` keeps one for integration,
  conclusions later overturned included.

## Where to start

1. `README.md`, then this, then `docs/conventions.md`: the conventions --
   the sign of s0, the goniometer's composition, scan points at image boundaries
   -- are where the bugs were, and a DIALS developer will read them fastest.
2. The step you know best, from its reference document to its library file.
3. **The places most worth a second pair of eyes**: the conventions in
   `src/geometry.*` and `src/expt.*` (the dxtbx models, read and written);
   integration's one pass (`apps/mxi_integrate.cc`, and what `CLAUDE.md` says
   keeps it the two passes' answer); scaling's error model (`src/scale.cc`,
   `refine_error_model`), since its departure from dials.scale is a claim about
   dials.scale; and the reflection table writer (`src/refl.cc`), which every
   program's output goes through.

Comments in the code explain why rather than what, and often say what was
measured; where a design was changed, the comment says what it replaced and
why. Questions and disagreement are welcome in the same form: a measurement.

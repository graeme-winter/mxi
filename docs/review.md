# Reviewing this code: a guide for DIALS developers

STATUS: written 30 September 2026 for external review, brought up to date 12
October, 6 November and 13 November 2026. It says what this is,
how to build and run it on your own data, where the code for each step lives,
what follows DIALS and where it departs from it, and how correctness has been
established. Every claim points at the document with the evidence;
`docs/README.md` maps the documents, and `docs/outstanding.md` is the one list
of open work.

## What this is

A C++20 implementation of rotation-data processing for **one sweep, several of
one crystal** (`docs/multi-sweep.md`), **or several crystals in one beam**
(`docs/multi-lattice.md`), step for step as DIALS divides it, with the spot
finder's threshold and the profile fitting also on the GPU (Metal and CUDA):

    mxi import -> mxi find -> mxi index -> mxi refine -> mxi integrate
               -> mxi symmetry -> mxi scale -> mxi export

`mxi process` runs them in turn (`docs/process.md`); `mxi-ui` is an interface
to them, their results and the images (`docs/ui.md`); `mxi correlation`
sorts many crystals into clusters (`docs/correlation.md`), and `mxi triage`
says what scaled data say of twinning (`docs/triage.md`).

Its own files are `.rflx`, one HDF5 file a step in dxtbx-h5's layout
(`docs/rflx.md`); it reads and writes DIALS' own `.expt` and `.refl` as well,
and `mxi convert` goes between them, so any step can be replaced by its
`dials.*` counterpart and the rest carries on: `dials.scale` has scaled
this code's integrated tables, and `dials.merge` and `dials.estimate_resolution`
have read its scaled ones. It is **not a fork of DIALS** and depends on neither
DIALS, dxtbx nor cctbx. Where it follows a DIALS algorithm the DIALS source was
read for it, and the file says which.

Its dependencies: HDF5, with bitshuffle and LZ4 vendored for the Eiger's
compression; gemmi for space groups (MPL 2.0, a submodule, unmodified); FFTW
optionally for indexing's transform (GPL, off by default -- the default build
uses a built-in radix-2); CUDA or Metal optionally for the spot finder and the
profile fitting. `mxi-view` and `mxi-ui` are Python, needing tkinter, numpy,
h5py and hdf5plugin, and matplotlib for the plots. The package is BSD
3-clause.

**How it was written.** Every commit is authored by Graeme Winter with Claude
(Anthropic) as co-author, and says so. The direction, the data and the decisions
-- which DIALS behaviour to follow, which to depart from -- are Graeme's;
`CLAUDE.md` is the working notes those sessions keep: invariants, conventions
that caused bugs, and what not to do again. It is worth reading for the same
reason a DIALS developer's notes would be.

Its size on 13 November 2026, excluding third-party code: the library 35614
lines in `src/`, spot finding among it; 26 commands in 14817 lines in `apps/`;
C++ tests 19295 lines; in Python, the comparison and regression tools,
`mxeq`, 11520 lines, the interface, `mxi_ui`, 3825, with 12569 lines of tests;
some 15300 lines of documents. 692 commits over two months, the latest
release 1.3.0, the first with Windows; `RELEASE_NOTES.md` says what each
release changed. Small molecules are in scope: `mxi symmetry
--small-molecule` considers every space group, centrosymmetric ones among
them (`docs/symmetry.md`).
`docs/algorithms.md` lists each stage's methods and their sources.

`mxeq regress` runs the chain over real data sets on each machine and keeps
every run's figures to compare (`docs/regression.md`). Scaling reports the
sigmas' calibration and the anomalous slope's control, and writes its model
into the scaled file; the cell's e.s.d.s come from two-theta.

Each .rflx carries the processing's summary, every program's figures and
tables in the order they ran; `mxi show` tells a file's state and
`mxi report` draws it as a page (`docs/output.md`). Numbers are stored
bitshuffle-LZ4 (`docs/rflx.md`).

## Building it and running it on your data

```sh
git clone --recursive <this repository> && cd mxi
cmake -S . -B build && cmake --build build -j
(cd build && ctest)              # the ctest suites, the spot finder's among them
build/mxi_tests                  # 389 unit tests
build/mxi_tests parallel         # only those whose name contains 'parallel'
```

Add `-DMXI_METAL=ON` or `-DMXI_CUDA=ON` for the GPU threshold and profile fitting,
`-DMXI_FFTW=ON` for FFTW. The `README.md` has the details and the dependencies
by platform.

The chain on your own data needs nothing of DIALS -- `mxi import` reads the
NXmx master, and `dials.import`'s `imported.expt` serves as well; every
program answers `--help` and `--version` and mirrors its output to
`mxi_<program>.log`, and the main sequence's take `--timing`. Each step reads
the last one's `.rflx` and writes its own:

```sh
mxi import    master.nxs          # imported.rflx
mxi find      imported.rflx       # strong.rflx; --gpu for the GPU
mxi index     strong.rflx         # indexed.rflx
mxi refine    indexed.rflx        # refined.rflx: scan-varying, a point per 10 degrees
mxi integrate refined.rflx        # integrated.rflx; --gpu: fitting on the GPU, in float
mxi symmetry  integrated.rflx     # symmetrized.rflx
mxi scale     symmetrized.rflx    # scaled.rflx
mxi export    scaled.rflx         # scaled.mtz, unmerged
```

Beside a DIALS run of the same data, `mxeq` (in `python/`) compares the two at
each boundary -- `mxeq check strong|indexed|refined|integrated|scaled <dials>
<this>` -- and explains where integrations differ; with no second program,
`mxeq equivalents` measures integration's bias against the data's own
equivalents, and `mxeq unique` compares two scaled data sets one unique
reflection at a time. It needs no cctbx.

On the 3600 images of an EIGER2 XE 16M sweep -- `ins10_1.nxs` of
https://zenodo.org/records/8376818 -- on an M4 Max MacBook, the chain from the
images to scaled data, `mxi find -j 16 --gpu`, `mxi index`, `mxi refine` (the
beam, analytically: the default since 14 October), `mxi integrate --gpu`, `mxi symmetry` and `mxi scale --d-min-auto`,
takes 36.6 s of wall time and 3m42 of CPU (30 September 2026).

## Where to find things

| step | library, in `src/` | program | reference document |
|---|---|---|---|
| import | `nxmx_import` (the NXmx geometry, its transformation chains) | `mxi import` | `docs/import.md` |
| the images | `nxmx_reader` (frames, the detector's pixel mask), `decompress`, `hdf5_handle`, `experiment_images`; `untrusted` (regions marked untrusted, joined to the pixel mask) | `mxi-view` (mxi-ui's viewer: the images, summed where asked, the spots, and the regions drawn), `mxi untrusted` (list, set, clear), `mxi max` (a maximum projection) | `docs/mask.md` |
| spot finding | `dext` (the threshold on the CPU), `dext_metal.cc` and `.metal`, `dext_cuda.cu`, `dext_gpu`, `signal_order`, `spots`, `spot_table` | `mxi find` | `docs/spots.md` |
| geometry | `geometry` (the models, the sample offset), `predict`, `derivatives`, `linalg` | | `docs/conventions.md` |
| indexing | `index` (3D FFT, the real-space grid search, a known cell), `fft`, `fft_fftw` | `mxi index` | `docs/algorithms.md`; `CLAUDE.md`, indexing |
| refinement | `refine` (static and scan-varying, the crystal tie, the sample offset) | `mxi refine` | `docs/algorithms.md`; `docs/multi-sweep.md` |
| integration | `integrate` (summation), `background`, `shoebox`, `mask`, `profile_model`, `profile_grid`, `reference` (profiles and fitting), `postrefine`; on a GPU `fit_batch`, `fit_cuda.cu`, `fit_metal.metal`, `fit_device_*`, `cuda_device.cu`, `metal_device` | `mxi integrate` | `docs/integration.md`; `docs/gpu.md` |
| symmetry | `symmetry` (space groups, settings, via gemmi), `laue`, `cosym` | `mxi symmetry` | `docs/symmetry.md` |
| scaling | `scale`, `scale_model`, `resolution`, `two_theta` | `mxi scale` | `docs/scaling.md` |
| many crystals | `correlation`, `combine`, `summary_cut` (a cluster's own history) | `mxi correlation`, `mxi combine`, `mxi split` | `docs/correlation.md`, `docs/multi-sweep.md` |
| the data's quality | `triage` (twinning, as xtriage), `verify` (how much of each lattice's prediction was seen) | `mxi triage`, `mxi verify` | `docs/triage.md` |
| the whole sequence | `self_path` (this program's own path, to run its commands) | `mxi process` | `docs/process.md` |
| output | `rflx`, `refl` (msgpack by hand), `expt`, `views` | `mxi export`, `mxi convert`, `mxi show`, `mxi report` | `docs/export.md`, `docs/rflx.md` |
| plumbing | `json`, `args`, `summary`, `processing`, `parallel` | | |

Library files are `src/<name>.hh` and `.cc`. There is one program, `mxi`, and
every step is a command of it (since 1.1): `apps/mxi.cc` dispatches -- `mxi
find ...` runs `mxi_find_main` -- and each command is still its own source,
`apps/mxi_<name>.cc`, built as an object library with its own settings and
linked into `mxi`. `mxi-view` is Python, mxi-ui's viewer in a window of its
own, `python/src/mxi_view.py`, copied beside `mxi` by the build with the module it
shares, `python/src/mxi_tools.py`; a command of its own since 1.3.0, which
finds `mxi`, where `mxi` starts no other program. Both are also in the wheel
`mxi`, with mxeq and the platform's `mxi`: every Python tool is in `python/src/`. `mxi-ui`, the
interface, is the package `python/src/mxi_ui` (`docs/ui.md`), copied beside
`mxi` with its launcher `mxi-ui`; only its windows import tkinter. In the
build directory each command's old name, `"mxi_find"`, is a link to `mxi`, which runs the command it is called by:
the tests run them so; they are not installed. Eight commands are for looking at
a step rather than doing it, listed apart by `mxi help`: `mxi profile`
estimates the profile model, `mxi foreground` marks each reflection's
foreground for dials.image_viewer, `mxi grid` and `mxi forward` show spots in
Kabsch space and rendered onto the pixels, `mxi residuals` decomposes centroid
residuals, `mxi background` fits backgrounds to given pixels, and `mxi
readtest` times reading; and `mxi verify` says how much of what each lattice
predicts was seen. `cmake --install` puts `mxi`, with the Python it runs --
`mxi-view`'s and `mxi-ui`'s -- in a `bin`.

What files record is unchanged: each processing step is named as before,
`"mxi_find"`, `"mxi_refine"`, in a file's summary, and each command's log is
`mxi_find.log` -- so that files written before 1.1 read as they did.

The referee, `mxeq`, is in `python/src/mxeq` (`python/README.md`): comparisons
with DIALS, regression over real data, and `truth.py` -- a scaled data set
against a refined structure's Fc^2, with figures and the worst observations'
shoeboxes.

The spot finder began as a project of its own and is now a stage of mxi like
the others: its table written by the library's writers, its experiment list
read by the library, its namespaces in `mxi`, its tests in `mxi_tests`
(`docs/spots.md`).

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
entering or not -- found exactly by a grid of cells, across the threads --
the edge of their histogram's peak times 1.3. The margin
matters: the median times 1.5, as it was, let a small molecule with a second
lattice in it index in a supercell twice the true one's volume. And it has a
macrocycle of assignment and refinement. The transform, its candidates and the
basis follow fft3d's in three more ways, each found missing on a cell of 43.6,
43.6, 212 A that dials.index indexed and mxi index did not: the reciprocal grid
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

**Several crystals in one beam** (`docs/multi-lattice.md`): `--max-lattices`
finds them as dials.index does, then assigns every spot again to the lattice
it lies nearest and refines them together, one detector: on two crystals of
insulin, of 20763 spots both index 20762 get DIALS's lattice and indices. A
spot two lattices claim, within three of their own scatter, is left out of
refinement (Graeme's choice; `--ambiguous closest` gives it to the nearer).
Given a refined list, a lattice missed at first is looked for again with the
geometry refined since. Each sweep's own crystal-less experiment is carried
through every program, its unindexed spots its own, as dials/dials#1029
proposes; `mxi convert` leaves it out for DIALS's later programs, by default
once there are lattices (`--keep-sweeps` keeps it).

**Refinement** has analytical derivatives for crystal, detector and beam,
checked against finite differences. The scan-varying crystal is a **cubic
B-spline** of control points, not DIALS' Gaussian smoother: both are smooth and
local, and they are not the same model. Each control point is tied weakly to
the static crystal (`--crystal-restraint`), `--interval` spaces them, and the
scan-varying pass starts from the static pass's reflections, its outliers
kept out -- re-admitted, they had let a small molecule's cell wander within a
sweep (`docs/algorithms.md`). Every sweep is refined on its own by default,
even several of one crystal; `--joint` for one fit. A file carries samples at image
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
* **A sweep's lattices in one pass**, each its own profile model and reference
  profiles, the device's fit a batch a lattice; **each reflection's
  neighbours**, any lattice's, masked out of its background, as DIALS's
  3d_threaded integrator does and its default does not.
* **Variances that do not move with their own noise.** Summation's Poisson term
  from the counts expected, max(I, 0) + I_bg, not those counted; the fitted
  variance's background term carried through the fit's weights, not a sum's
  m^2; the fit's weights on a pixel's least variance of 0.05 counts, robust,
  its variance the fit's own whatever the weights, sum(p^2 w^2 V) /
  sum(p^2 w)^2, V on 0.001 -- 0.001 for both had let a strong spot's tail
  pixels pull its fit about on a near-empty background (item 61). And
  each variance with no signal written, for scaling (below). On a ten-turn
  insulin sweep, multiplicity 210, the outer shell's merged I/sigma went from
  -5.7 to +3.8 (`docs/integration.md`, "The fitted variance").
* **A profile grid of 13 a side** (`--grid-points 6`): reflections cut by a gap
  come out high where the reference is broader than the spot, +2 per cent at
  a tenth cut and +6 at a third, by gaps masked on purpose; the finer grid
  takes a third of it for nothing in time (item 2).
* Reflections crossing a module gap: judged by their clean symmetry
  equivalents, 0.967 here and 0.777 in DIALS where a tenth to a third of the
  reflection is lost.
* Through `dials.scale`, on 1800 images of insulin: Rmerge 0.038 and Rpim 0.009
  on this output and on DIALS' alike.

**Symmetry** (`docs/symmetry.md`) follows `dials.symmetry`: the lattice's
symmetry by Le Page's method through gemmi, each element scored and each
subgroup after Evans (2011), to a resolution limit from CC half, Friedel mates
apart; then the space group by absences. Several crystals are brought to one
setting first by cosym (Gildea & Winter 2018): on two crystals of insulin, I23
in its I432 lattice, the same coset as dials.cosym, then I 2 3. Departures: intensities normalised by
an anisotropic Wilson model with a light atom's scattering and nothing of
DIALS's protein model -- a weaker prior, so B comes out larger and the CCs a
little lower than DIALS's (the identity's 0.842 against 0.95 on a P 4_1
crystal, both choosing P 4_1; resolution shells, before, chose P 1); monoclinic
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
* **Each variance from the model's intensity**, not the observation's own: its
  variance with no signal plus the signal's at g I_h. An observation's own
  variance moves with its noise, so an inverse-variance weighted mean leans
  low; on two crystals each reflection's weighted mean had been 0.9 to 2.4 of
  its sigma below its plain mean, within 0.1 after.
* **The resolution estimate is dials.estimate_resolution's**, and agrees with it
  exactly on 1082994 reflections (1.25 and 1.17 A), except that only bins with
  pairs enough to fit decide whether CC half stays above the limit throughout.
* **The error model's fit is by regression across intensity bins** by default,
  split by background and signal where integration recorded the variance with
  no signal; dials.scale's own default, a and b in turn, is
  `--error-model-fit individual`. Neither fits every intensity on data like
  Graeme's 36 insulin crystals (`docs/scaling.md`; item 69).
* As dials.scale: partiality divides only the summed intensity, and a
  reflection split by integration's blocks is combined before scaling (both
  since 15 October: mxi had divided the profile-fitted intensity too, and
  scaled the pieces apart).

**Clustering** (`docs/correlation.md`): `mxi correlation` is
dials.correlation_matrix -- the sigma-weighted correlation of every pair of
data sets, cosym's coordinates and their dimension, OPTICS as scikit-learn's
(every ordering, predecessor and label the same on four test sets), each
cluster written as its own file, an HTML report. On Graeme's 36 insulin
crystals (Thompson et al. 2025's): the same dimension, min_samples and three
clusters as DIALS. mxi scores every min_samples where DIALS samples them by
optuna; the matrices differ in detail by a step of DIALS's preparation not
yet reproduced.
  `--d-min-auto` scales, estimates and scales again to the limit.

**Speed** is measured, not assumed, and the tables are in the documents. On a
MacBook the whole chain took 59 s on 3600 frames of an Eiger 16M, and
integrating 1800 frames of a smaller detector 6.6 s, since made faster. Every
phase of every program is reported by `--timing`.

## What is not here yet

The chain is complete for one sweep, for several of one crystal, and for many
crystals -- indexed apart, brought to one setting by cosym, sorted by
`mxi correlation`, joined and parted by `mxi combine` and `mxi split`; beyond
it (`docs/outstanding.md` has the numbered list):

* Free-set validation of scaling; overloads flagged in integration; the cell
  constrained to the lattice's symmetry in refinement (`mxi scale`'s cell from
  two-theta is constrained, with e.s.d.s); cosym for many small partial data
  sets.
* Reports and merged output: an HTML report, merged MTZ and mmCIF are not
  written; `mxi export` writes the unmerged MTZ, which AIMLESS reads
  (`docs/export.md`), and `dials.merge` takes the scaled table.
* **Known biases in integration** (`docs/outstanding.md`, items 1 to 4):
  reflections cut by a gap, high by a few per cent where a third is lost; a
  few partials at a scan's ends high on insulin (not sigma_m: narrower merged
  worse); the background half to one per cent high at low resolution, on bare
  background, likeliest its curvature near the beam.
* **The backstop** (`docs/backstop.md`, items 52 and 53): observations its
  shadow attenuates integrate low, and scaling's outlier rejection keeps them
  over the unharmed, so the lowest-resolution reflections merge too low.
  `--d-max` leaves them out, and `mxeq` prototypes filter them; paused, none
  a default, for on ferritin they did not measurably improve a refined
  structure, and no absolute measure of processing quality yet decides it.

## What this work found in DIALS

Measured on two crystals of insulin, both programs' output on the same
observations: DIALS's default integrator, 3d, finds overlapping reflections
and masks nothing with them; dials.scale's final sigma, judged by each
observation's exact deviation from its equivalents, some 30 per cent small for
the stronger two-thirds (`docs/scaling.md`). DIALS's summed variance is |I| + I_bg
(1 + m/n) (summation.h): it moves with each observation's noise, but evenly either
side of zero, so it does not lean a weighted mean as mxi's did -- the counted
foreground, I + I_bg, until October; now max(I, 0) + I_bg, and in scaling the
model's intensity.

And dials.integrate's profile-fitted sigmas for weak reflections are some 2.5
times too large: on twelve crystals of bovine insulin, the weakest sixth's
deviations from their equivalents have variance 0.16 of what DIALS's sigmas
say, where mxi's, on the same reflections, have 1.03-1.05 (20 October; the
intensities themselves agree within 0.13 sigma). Weighted by them, correlations
between data sets effectively count only the strong reflections.

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

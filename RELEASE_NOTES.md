# Release notes

Newest first. Each release lists first what changes results or commands -- a
default changed, an option renamed or gone -- then what is new, then what is
fixed. The tag of each (`git tag -n99 v1.0.5`) carries the same summary.

## Unreleased -- since 1.3.4

**Fixed**
- mxi merge --d-min and --d-max limited the file but not the summary of the
  merging statistics, which went on to every row scaling had: with --d-min
  1.3, a summary to 1.14 A. The summary is now of what was merged, its totals
  the counts on the line above it.

## 1.3.4 -- 26 November 2026

**New**
- The wheels go to PyPI, mxi and mxi-cuda, by trusted publishing after each
  GitHub release (docs/release.md). They carry their build's licence notices,
  as the release archives do, and a licence expression composed from them --
  FFTW's GPL included where the build has FFTW; before, a wheel carried mxi's
  licence alone and said BSD-3-Clause.
- mxi merge: a merged MTZ from scaled data, as dials.merge writes one --
  IMEAN, I(+) and I(-) with their multiplicities, French and Wilson's F, F(+),
  F(-) and DANO as dials.merge computes them (cctbx's tables), and FreeR flags
  in CCP4's convention, a hash of the index under the lattice's symmetry so
  that every data set of one lattice shares one free set. --each merges each
  crystal alone. Each merge prints the summary of the merging statistics, mxi
  scale's from the same observations, line for line (docs/merge.md).
- mxeq lattice-map: where indexed spots lie around their lattice points, in
  reciprocal space -- along the crystal's axes, and radially, along the
  rotation and across in each reflection's own frame, the crystal scan-varying
  where it is; robust widths, the fraction beyond three widths, histograms and
  trends with resolution. A measure of indexing quality, for data whose first
  lattice is a good solution. With -o, an HTML page: the histograms on a log
  scale against the Gaussian of the robust width, reciprocal-space maps of two
  offsets at once, and trends with resolution and image.
- mxi symmetry --scale-p1: scaled in P1 first, scale and decay through the
  sweep, for the symmetry's judgement alone, the file written unscaled -- for
  sweeps whose systematic changes part symmetry mates measured far apart in
  rotation (docs/symmetry.md).
- mxi scale ends with the cell refined from two-theta laid out to be copied:
  the refinement's reflections and RMSD on one line, then "Unit cell:" with
  the e.s.d.s and "Symmetry:" with the space group as CCP4 writes it (P41212).
- mxi search-beam: the beam centre from the strong spots, as
  dials.search_beam_position does by default (LABELIT's method), for when the
  header's centre is too far out for indexing to find the lattice. It writes
  the experiments with the detector moved, ready for mxi index. On insulin,
  centres given up to 4 mm out came back within 3 pixels; where DIALS's rule
  took an alias -- the lattice shifted by one of its own vectors -- the truth
  is chosen (docs/search_beam.md).
- mxi triage measures anisotropy: the anisotropic Wilson model's B tensor,
  constrained to the crystal's symmetry, by its principal directions -- none,
  an axis and the plane across it, or three -- and how far the data reach
  along each, where the merged mean I/sigma falls below 2 (docs/triage.md,
  Anisotropy).
- mxi integrate takes two passes over the images where one would need more
  memory than the machine can spare: the single pass's holding predicted from
  the boxes before an image is read, against a budget, --max-memory or
  three-quarters of the machine's memory (a container's or job's limit if
  smaller), and said when it switches. --one-pass insists; mxi process passes
  both on. On a crowded data set one pass had a footprint of 21 GB, two 7 GB,
  in the same time.
- mxi integrate --timing says where the memory goes: boxes open, boxes held
  for fitting and boxes held in overlap groups, each at its largest and the
  three together, beside the process's peak; and the process at its most -- on
  macOS, the allocator's blocks in use, what it holds free, and the rest
  (device buffers, the program); with mimalloc, what it has committed. The
  held figure had left out each box's background.

**Fixed**
- Two pairs of mxi's internal types shared a name -- the forward model's and
  triage's Moments since before 1.3.3, and mxi merge's MergedSet beside mxi
  correlation's -- which C++ forbids and nothing reports: the linker keeps one
  copy of each inline member for both. MergedSet's crashed the C++ tests on
  Linux and Windows (1.3.4-rc1's builds); Moments' had no effect seen.
  Renamed, and a test now fails on any type defined twice.
- mxi symmetry printed "?" for the Patterson group P -3 1 m: its two-folds lie
  between the conventional a and b, which no rotation of it fixes, and only
  fixed vectors were tried as axes. Vectors perpendicular to an axis of order
  three or more are tried too.
- mxi symmetry judged a scaled file on its integrated intensities, its scale
  factors ignored; it now takes the scaled ones, intensity.scale.value over
  inverse_scale_factor. (A first version of this fix, during 1.3.3's
  successor, took intensity.scale.value as scaled already: --scale-p1 fitted
  its scales and never applied them. With them applied, on 9uoa its symmetry
  elements' CCs rise from 0.70 to 0.77, the four-fold's from 0.665 to 0.756.)
- mxi symmetry refused sweeps indexed apart whose cells were one lattice in
  different bases -- a hexagonal crystal's sweeps with gamma 60 and gamma 120,
  the choice falling to the noise in alpha and beta -- saying they differed by
  67 per cent. Each sweep's lattice is now brought to the first's setting by
  the nearest small change of basis before the cells are compared, and the
  change is reported. A sweep whose cell already agrees is left as it is: a
  change of basis fitting the cells' noise a little better may be a lattice
  symmetry the crystal lacks, its indexing ambiguity.
- mxi symmetry takes only reflections integrated by summation as well as by
  profile fitting, as dials.symmetry does. Summation fails where a spot
  reaches a masked pixel, and the profile fits of spots clipped by a backstop
  or a gap could be far enough out to hide the symmetry: on 9uoa
  (proteindiffraction.org/project/9uoa) the choice fell to C 2 2 21 or P 1 21
  1, depending on the mask, where it is P 41 21 2 (docs/symmetry.md).
- mxi scale's error model could throw away the strong reflections' error: b,
  derived from the fitted a^2 b^2 and a^2, was capped at 0.5, so where a^2
  came out small the strong reflections' variance was made a fraction of what
  was fitted. b is no longer capped.
- mxi scale could fit its model to nonsense -- negative scales, CC half near
  zero -- when the random subset it fits on drew a reflection whose last two
  observations disagreed hopelessly: its outlier rejection left any pair
  alone, unable to say which was wrong. On 9uoa
  (proteindiffraction.org/project/9uoa), P 41 21 2, (0 0 4), near the rotation
  axis, kept 3458 +- 9 and -3 +- 0.3, and about one subset in eight failed --
  with --d-min-auto, so it seemed. A pair disagreeing beyond the limit is now
  rejected whole.
- Random subsets and halves were different on macOS and Linux from the same
  seed -- std::shuffle is each C++ library's own algorithm -- so mxi scale
  could fit a different subset, and mxi symmetry, mxi triage, mxi search-beam
  and the resolution estimates draw different random halves, on a Mac. They
  are now drawn the same everywhere; as a result some figures move slightly
  from 1.3.3's.
- mxi symmetry judged true symmetry absent in unscaled data: it took their
  counting sigmas as their error, so expected too high a correlation from a
  true element -- on 9uoa (proteindiffraction.org/project/9uoa), P 41 21 2,
  E(CC; S) 0.919 where dials.symmetry's was 0.868, and P 1 21 1 recommended.
  The sigmas are now corrected for the Laue group analysis as dials.symmetry
  corrects unscaled data's (docs/symmetry.md).
- mxi import put the detector where some DECTRIS file writers wrongly write
  it, upstream of the sample, between it and the source, so that every spot
  came out below 0.6 A. It now reverses the component along the beam of the
  translations placing the detector, as DIALS does -- the beam centre's
  offsets, which such a file may carry in the same translation, left as they
  are -- and says so.

## 1.3.3 -- 22 November 2026

**Changed**
- mxi integrate's foreground is DIALS's: a circle in eps1 and eps2 judged at
  each pixel's nearest corner, on every image of the box, where mxi had judged
  each pixel's centre and limited eps3 too. Given the same models, mxi's
  integration now agrees with DIALS's within 1 per cent, summation and profile
  fitting alike; the background is no longer 1 per cent high, nor summation 1
  to 3 per cent low. --foreground box gives the old rule (docs/integration.md,
  "The foreground, and sigma_m").
- sigma_m is estimated as DIALS estimates it by default, each image weighted
  by its counts (--sigma-m-estimator extended): on insulin 0.080 degrees where
  it had been 0.119. --sigma-m-estimator basic gives the old estimate.
- mxi integrate's boxes are DIALS's: twice n sigma_b in eps1 and eps2
  (--box-scale 2, as DIALS's sigma_b multiplier), where 1.9 had been measured
  against one run and left them a pixel narrower. Given the same models,
  summation now agrees with DIALS's to one part in ten million. Profile
  fitting's sigmas stay mxi's, the fit's own variance, where DIALS reports a
  summation's over the profile's region, some 20 per cent larger
  (docs/integration.md).
- Together these change every integration's results: on a full insulin sweep,
  merging the same or a shade better (R merge 0.070 against 0.072), I/sigma
  some 5 per cent lower, a larger foreground carrying more background.

**New**
- Hot pixels: mxi find counts how many images each pixel is signal on, and
  says how many are on a quarter of them or more (--hot-fraction), from 100
  images -- a pixel hot on every image, or twinkling on every other. With
  --mask-hot it writes them to the image set's untrusted regions as single
  pixels, a new kind beside circles, rectangles and polygons, so that
  integration and every later step leave them out; mxi untrusted --hot does
  the same to a file on its own, and mxi-view draws them (docs/mask.md).
- mxeq mtz: two unmerged scaled MTZ files compared -- DIALS's and mxi's, say:
  their headers and columns, the indexing between them found from the
  observations, each observation matched by original indices and batch (ratios
  overall and against batch), and the merged data by resolution shell
  (python/README.md).

**Fixed**
- mxeq equivalents said "has no crystal" of every mxi file since 12 October:
  it took the first experiment, and mxi index carries each sweep's own
  experiment through first, crystal-less (dials/dials#1029). It now takes the
  experiments with crystals, and leaves the sweep's unindexed spots out.
- The Linux arm64 build of 1.3.2 died with an illegal instruction on ARMv8.0
  CPUs, a Raspberry Pi 4 for one, before printing anything: mimalloc was
  compiled for ARMv8.1, its atomics needing LSE. It is compiled for ARMv8.0
  now, the CPU's own atomics still chosen at run time where it has them; and
  the arm64 release builds are checked for any instruction an ARMv8.0 CPU
  lacks.
- mxi process refused --anomalous as unknown, so an anomalous data set had to
  be scaled again by hand; it now passes it to mxi scale, as it does
  --d-min-auto.

## 1.3.2 -- 15 November 2026

**New**
- The Linux builds use mimalloc in place of the C library's malloc. On a
  two-socket Xeon with an L40S, integrating 3600 insulin images took 38 s, 11
  of them glibc's allocator with 48 threads building boxes and freeing each
  other's; 27 s with a scalable allocator, every result the same. The banner
  says "mimalloc"; -DMXI_MIMALLOC=OFF to build without it.
- mxi integrate --gpu: each box's local reference profile written into one
  buffer, reused batch after batch, where a vector a box had been some 10 GB
  of new memory a run -- "preparing" 4.9 s on a two-socket Xeon against 0.45
  on an M4 Max. The same results.
- mxi integrate: its profile model's reflecting range (sigma_m) fitted across
  the threads, and its search for neighbouring boxes' tiles built across them,
  where each ran on one: on an M4 Max 0.69 s and 0.41 s of insulin's
  integration, on one thread each. The same on any number of threads; sigma_m
  differs from 1.3.1's in its eighth figure, the search settling elsewhere in
  the likelihood's flat minimum.
- Every program that keeps a log begins its output and every log with what
  made it: the version, the build (CUDA and its version, Metal, or the CPU
  only; the compiler), the machine's name, system, processor and cores
  (docs/output.md).
- Every --timing table now says where the time its phases did not account for
  went ("the rest"); mxi integrate's names its overlaps and planning, its
  chunks' own time, and its summary.
- A snapshot of every push to main: each platform's mxi binary, at a fixed
  address under the pre-release "snapshot", for machines with no compilers.
  Builds say what they are: a candidate 1.3.2-rc1, a snapshot the version and
  the commit, 1.3.2+3806819 (docs/release.md).

**Fixed**
- --gpu with no usable CUDA device gave the driver's and the build's CUDA
  versions and left them to the reader, who took a newer driver for the
  trouble. It now says what they mean: a driver older than the build needs
  replacing; a newer one is as it should be, and where nvidia-smi works but
  CUDA will not start, the GPU is not the process's to use -- on a cluster, a
  job given one; elsewhere /dev/nvidia-uvm.
- mxi integrate --gpu named a reflection with none of its foreground measured
  -- wholly in a gap -- only as "the device's fit failed"; it now says so, as
  without --gpu, from the mask before the device is given it, and the CPU says
  it first too: the reasons in profile.failure the same on both.
- mxi convert of a reflection table alone said "Wrote out.rflx:, 22
  reflections".

## 1.3.1 -- 14 November 2026

**New**
- mxi symmetry --small-molecule, and mxi process --small-molecule: every space
  group with the Patterson group found, not only the chiral ones a
  macromolecule's crystal may be in -- centrosymmetric groups, mirrors and
  glides, the glides judged by their absences -- and of those the absences
  cannot tell apart, the one whose centre of symmetry agrees with mean
  |E^2-1|, which is reported (docs/symmetry.md).

**Changed**
- mxi integrate stops, naming the image and the file, at a frame it cannot
  read -- an unallocated chunk, a frame the detector's writer never received.
  It had integrated around one, the boxes spanning it low, with a warning at
  the end; its boxes' values are no longer cleared before being filled from
  the frames, and a missing frame would leave them holding whatever their
  memory held. Spot finding is unchanged.

**Fixed**
- A thread count (-j or --threads) stopped any command that runs itself for
  each part of the whole: mxi integrate on several sweeps or lattices, and
  with --postrefine; mxi index --no-joint; mxi refine a group at a time -- so
  mxi process -j N on several sweeps. Each part's run was given "-j" without
  its number and stopped ("option '-j' needs a number of threads"); the count
  is now passed on once, as --threads N.
- mxi scale in a centrosymmetric group -- every reflection centric -- printed
  anomalous statistics there are none of: completeness 0, multiplicity the
  multiplicity. It now says there are none, leaves the anomalous columns out
  of its summary, and the report draws no CC anom; --anomalous there says it
  changes nothing.
- mxi triage, given data with no acentric reflections -- a centrosymmetric
  group, every reflection centric -- had said the L test's statistics were as
  expected and no twinning was suspected, from none; it now says it cannot
  judge.
- mxi symmetry could not name a rhombohedral group, R -3 m or R -3, wherever
  the primitive cell gave the three-fold a component of 3: the conventional
  axes were sought among lattice vectors with components -2 to 2 only. A
  rhombohedral lattice longer along its three-fold than across -- most real
  ones, R3 and R32 macromolecules among them -- failed in every reduced cell
  tried, its best group unnamed, and mxi symmetry, and so mxi process, stopped
  ("could not be put in a reference setting"). Found by Graeme on a small
  molecule, R 3 2:H; the components now reach 3, and a test names every
  Bravais lattice from random reduced cells.
- mxi integrate moves less memory: each worker keeps its frame buffers for the
  run, where a new 36 or 72 MB buffer a frame had cost a page fault a page
  (ferritin on a Ryzen 9950X: 38 million faults to 7 million, reading frames
  31 s to 24); its boxes' values are not zeroed, twice, before every one is
  written from the frames; and CUDA profile fitting's buffers grow by half
  again, not to each batch's size. The same results, column for column.
- Faster on x86 Linux, where a user's L40S workstation (a 2.9 GHz Xeon) ran
  the 1.3.0 release's CPU steps 2 to 8 times slower than an M4 Max: indexing
  11.5 s against 1.4, integration 43.6 s against 12.6. The flags for the
  rounding instruction had never reached the library, standing after its
  definition in CMakeLists.txt; and GCC called the C library for fmax, round,
  lround and sqrt where Clang, Apple's on a Mac among them, inlines them. The
  flags now come first, with -fno-math-errno and -fno-trapping-math, Clang's
  own defaults; indexing's hot loop compares where it called fmax. No result
  changes; indexing's choice of basis three times faster here, on one core.

## 1.3.0 -- 12 November 2026

**New**
- Windows: `mxi.exe` for Windows 10 (1903) and 11 on x86_64, on the CPU, as a
  zip of `mxi.exe` alone and as a wheel with the interface -- `pip install` it
  into a Python 3.12 to 3.14 environment for `mxi`, `mxi-ui` and `mxi-view`
  (docs/windows.md).
- A wheel for each machine with everything in it: `pip install` it and get
  `mxi`, `mxi-ui`, `mxi-view` and `mxeq`, all of one version, with the Python
  they need -- `mxi` for the CPU and Metal builds, `mxi-cuda` for the CUDA
  ones (the platform wheels: manylinux_2_28 for Linux x86_64 and ARM64, macOS
  13 on Apple silicon). `mxi-ui` finds the `mxi` installed with it whether or
  not the environment is on the `PATH`.
- `mxi process --timing`: `--timing` to every step that times itself, and at
  the end a table of each step's time, import and export too (Graeme).

**Changed**
- The release archives hold `mxi` alone, and the Python wheel is `mxi`, where
  it was `mxi-python` without the program: the interface, its viewer and mxeq
  come from the wheel, which now carries `mxi` too. An archive is for a
  machine without Python, or only the programs.
- The interface and its viewer are commands of their own: `mxi-ui` and
  `mxi-view`, where they had been `mxi ui` and `mxi view` -- the same
  arguments, beside `mxi` in a release (or from the wheel, `pip install
  mxi-python`). `mxi ui` and `mxi view`, typed, say so and stop: mxi no longer
  starts other programs.
- `mxi process` runs each step in code, the command called in its own process,
  where each step had been a process of its own: each step's output and log as
  before; a step that fails stops the run with the same message, but one that
  crashes now takes the run with it, without the message of where it stopped.
- What mxi prints goes to the terminal and the program's log through mxi's own
  functions, in the same call, where stdout and stderr had themselves been
  replaced -- with functions Windows has not. Logs and terminal byte for byte
  as before; what another library writes straight to stderr (HDF5's error
  stack, where not turned off) now reaches the terminal, not the log.
- Every integer in the code an explicitly sized type -- `std::int64_t` where
  `long` was, `std::int32_t` where `int` was -- the CUDA and Metal code among
  it, the same types on Linux and macOS, so no change in what mxi does; on
  Windows the four uses of `long` that would have been wrong there
  (docs/windows.md) are right.

## 1.2.0 -- 7 November 2026

**New**
- The image viewer's tools for untrusted regions are a **Masking** menu in its
  toolbar -- the four drawing modes, Undo, Clear, Same as previous, Save --
  leaving the image their row; its label says how many regions, whether saved,
  and what a click draws.
- The image viewer (`mxi view`, mxi ui's View images) shows which image as a
  slider under the toolbar, as wide as the window, in place of the number box:
  where the image is among the sweep's at a glance, and dragged, each shown as
  fast as images can be read; an arrow each side to step one, and which image
  it is beside them, its number editable to go straight to one.
- The image viewer (`mxi view`, mxi ui's View images) colours several lattices
  on a sweep apart: each lattice's spots in a colour of its own and its
  predictions in a lighter tint of it, each a layer to show or hide, the
  unindexed spots red as before.
- `mxi triage SCALED.rflx`, begun: what the scaled intensities say of
  twinning, as phenix.xtriage says it -- the moments, N(Z) and the L test,
  independent of twin laws; the twin laws the lattice allows, each one's R and
  H-test fraction; a summary that calls the data twinned only where both kinds
  say so, and a symmetry missed where twin-related pairs are equal but the L
  test is not twin-like (docs/triage.md).

**Changed**
- `mxi integrate` fits reflections whose foregrounds overlap, of one lattice
  or several, together -- each with its own lattice's profile, the shared
  pixels apportioned between them -- and takes their neighbours' counts out of
  their summed intensities, where each alone had counted its neighbours'
  photons as its own. This changes the default output: overlapped reflections
  have different profile-fitted and summed intensities, and every integrated
  file has a column `profile.fitted_together`, the summary a count. On
  Graeme's two crystals of insulin, the 1100 or so overlapped measurements
  went from +0.92 sigma from their equivalents, rms 3.79, to +0.07 and 1.06.
  `--no-fit-overlaps` fits each alone, as before.
- `mxi index` estimates the largest cell in a fraction of the time: on
  Graeme's 428 349 spots of a 128 A cell, the max cell 10.21 s before, 0.28 s
  now, and the whole run 13.2 s before, 2.9 s now -- every candidate cell, the
  cell chosen and the 414 387 indexed exactly as before. Its test for
  overlapping boxes had looked up each reflection's experiment and panel by
  column name for each of some 127 million pairs of boxes; its
  nearest-neighbour search kept its cells in a tree, searched whole cubes, ran
  on one thread, and with points nearly in a plane made its cells minute. Now
  each reflection's experiment and panel are read once, the cells are one
  array with each shell's own visited, the points are shared among the
  threads, and the cells are sized from the points' widest span. The same
  neighbours, the same largest cell, to the bit.

## 1.1.5 -- 5 November 2026

**Changed**
- `mxi view` is mxi ui's image viewer, in a window of its own, no longer
  napari's: the same command and options (`-o`, `--sum`, `--sweep`), and Save
  writing `masked.rflx` and `mxi_mask.json` where it is run, as before. It
  needs Python with tkinter, numpy, h5py and hdf5plugin, as mxi ui does, and
  no napari; the wheel's `[napari]` extra is gone. Regions are rectangles,
  circles and polygons, drawn, chosen and deleted, undone; what napari's
  shapes layer had besides -- ellipses, freehand polygons, moving a region or
  its vertices -- is gone: a region is deleted and drawn again.

**New**
- `mxi show` shows only what is asked for: `--models`, `--unit-cell` (each
  crystal's cell a line and nothing else, for reading or for another program
  -- the tutorial had grepped for it), `--reflections`, `--summary`, `--step
  NAME` (one step's record). With none, everything, as before.
- mxi ui's viewer draws the detector's untrusted regions: rectangles, circles
  and polygons on a sweep's images, chosen and deleted, undone a step at a
  time, the same as the previous sweep's; saved as `mxi view` saves them, into
  `masked.rflx` through `mxi untrusted` and `mxi_mask.json`, and Find spots
  then reads `masked.rflx`. Closing with regions not saved asks first.
- mxi ui has a Quit button (and Cmd-Q, Ctrl-Q), and its viewer a Close button
  (Cmd-W, Ctrl-W): each asks first where something would be lost -- a command
  running, regions drawn and not saved, in the viewer or, quitting, in any
  viewer open.
- mxi ui's notes, for learning: beneath each step's form, what it does, what
  to look at in its results, what goes wrong and which option answers it, and
  links to the guides that say more. Every option a note names is checked
  against its command's `--help`.
- mxi ui's plots: integration a sweep at a time, a line each, where there are
  several -- more than 8, each a grey line under their median and a bar a
  sweep for its overall <I/sigma> and profile correlation; and the Correlation
  tool's -- the correlation and cosine-angle matrices in OPTICS's order, the
  reachability coloured by cluster, the data sets' principal components, the
  functional by dimension -- from the `mxi_correlation.json` it now always
  writes. The tool reads `scaled.rflx` by default, as `mxi correlation` takes,
  where it had offered `symmetrized.rflx`. The Plots tab scrolls where there
  are more than it shows, by its scrollbar, a wheel or a trackpad.
- mxi ui's viewer sums images: from the one shown, as many as are asked for,
  so that a shadow or faint spots show; the spots on any of them marked, the
  contrast set afresh.
- `mxi_tools` finds mxi by the `MXI` environment variable first, as mxi ui
  does.
- `docs/process.md`, a guide to `mxi process`: where each option goes, where
  it starts from a `.rflx` and why, what to do when a step fails, and the ways
  through -- one crystal, a mask first, many crystals to their clusters --
  with their flows, drawn by GitHub from the Markdown.

**Fixed**
- `mxi process masked.rflx` -- a file `mxi untrusted` wrote -- had been
  refused, neither its writer nor its summary's last step having a step to
  follow: it goes on from spot finding, as `docs/mask.md` has it, whatever
  file was masked. And a file whose writer has no step of its own (`mxi
  split`, `mxi combine`) goes on after the last step its summary records that
  has one, where it had looked at its last step only.

## 1.1.4 -- 4 November 2026

**Changed**
- A cluster `mxi correlation --write-clusters` writes carries its own
  crystals' history, not the whole run's: each step's record cut to its sweeps
  and experiments, the whole run's figures left out and named, scaling's
  record left out (a cluster is scaled afresh), and correlation's own record
  -- the cluster, its data sets, the file it was cut from. Its report, and mxi
  ui's summaries, are its crystals'.
- mxi's Python tools are one wheel, `mxi-python`, where mxeq's was its own:
  mxeq, the referee, under the same import name and command; `mxi_tools`,
  which reads mxi's files and images; the viewer `mxi-view` (napari through
  the `[napari]` extra); and `mxi-ui`. matplotlib is now a dependency. The
  release's wheel is `mxi_python-1.1.4-py3-none-any.whl`; where the `mxeq`
  wheel was installed, `pip uninstall mxeq` first.

**New**
- `mxi process MASTER.nxs...`, for routine data: the whole sequence, import to
  export, a step at a time, each the command a script of them would run, with
  its own output and log. `-j` goes to every step, `--gpu` to spot finding and
  integration, `--cell` and `--space-group` to indexing (the cell as
  `--known-cell`) and the space group to scaling, `--no-joint` to indexing for
  several crystals, `--d-min-auto` to scaling. Given one `.rflx` instead, `mxi
  process FILE.rflx` picks up after the step that wrote it and finishes: a
  cluster from `mxi correlation --write-clusters` is scaled afresh and
  exported. A step that fails stops it, saying which, its exit code, its log,
  and how to go on -- its own command, or `mxi process` on its input;
  `--dry-run` prints the commands and runs none.
- `mxi ui`, an interface to mxi, for processing data and for teaching
  (docs/ui.md). The workflow's steps and tools down the side, each marked done
  from the working directory, so that a directory processed before opens as it
  stands. Each step a form -- its inputs, main options, output, threads and
  further options, each field's option a tooltip -- building the mxi command
  as it is filled in, shown to copy, and run with its output streamed into the
  Log, cancellable. Each step's Summary and Plots read from its file's
  records, not its log: its figures and tables, the merging statistics as
  DIALS lays them out; spots per image and by resolution, RMSDs by cycle, the
  cell over the scan, <I/sigma>, CC half, completeness and the R factors by
  resolution, with matplotlib. And View images: a step's images in a window of
  their own, each spot drawn as its box, indexed apart from unindexed,
  predictions; zoom by buttons, keys, a right-button drag or a scroll;
  contrast and colour maps; the pixel, its counts and its resolution under the
  cursor.
- `mxi ui` runs with tkinter, matplotlib, numpy, h5py and hdf5plugin -- no Qt
  -- and where tkinter is missing it says what to install for the system (`mxi
  ui --check`). It is in the release archives beside mxi, and in the wheel as
  `mxi-ui`.

**Not yet**
- Untrusted regions are still drawn in `mxi view` (napari) or set with `mxi
  untrusted`; the interface's viewer will draw them, and then replace `mxi
  view`.


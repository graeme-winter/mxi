# Several lattices in one data set: a plan

STATUS: agreed, October 2026, with Graeme's answers (below). Built:

* Stage 1, in part: models shared in refinement as the experiment list numbers
  them -- ExperimentList::beam_of and detector_of, read from the file; a
  parameter block a distinct detector and a distinct beam, owned by all that
  share it; written back interned, once. A single sweep refines to the same
  bytes as before. The profile model a lattice is moved to stage 5, where
  integration first uses it.
* Stage 2, sweeps carried through (dials/dials#1029), the default (Graeme:
  even in DIALS's format, since dials.split_experiments can take the
  crystal-less experiment off to be indexed); mxi index --no-keep-sweeps and
  mxi convert for the lattices alone (by default, once there are any;
  --keep-sweeps keeps the sweeps). The two views in one
  place (src/views.cc) -- the lattices taken out of a list for a program and
  put back, each crystal-less experiment given the models its sweep's
  lattices refined; mxi index writing each sweep given first, crystal-less,
  its unindexed spots its own, the lattices after it; mxi refine,
  mxi integrate, mxi symmetry, mxi scale and mxi export reading through the
  views; mxi convert, the crystal-less left out by default and their
  spots -1. On Graeme's two crystals, indexed with and without the
  sweep from the same spots and carried through every program: the same
  refined models, the same integrated and scaled tables but for id, I 2 3
  from both, the same MTZ row for row. Several sweeps: integrated a run a
  sweep, each sweep's lattices in one pass and its crystal-less experiment
  carried, the runs joined in the list's order -- a sweep of one lattice the
  run it always was; on insulin split in two and carried through, the same
  table as without, the lattices' ids two on.

  A lattice missed, recovered (Graeme: an unindexed lattice found at the end
  with the refined geometry of another): mxi index given a list with lattices
  already, and --max-lattices more than it has, keeps them and their
  identifiers, sets aside the spots they index, looks for more among the rest
  with the geometry they refined, and assigns and refines all together; the
  sweep's own experiment carried again. On ins2_27, one lattice indexed alone
  -- 15307 spots, the second's among them by chance -- refined, then given:
  the second found, 5916 of the 6001 left; after assignment 13616 and 7580,
  99.5 per cent, RMSD 0.256 and 0.263 px and 0.226 images -- as both found
  together at the start, 13617 and 7579.
* Stage 3, before stage 2: mxi index --max-lattices N (index_lattices in the
  library). The search as dials.index's; then, every lattice known, each spot
  assigned again globally -- to the lattice whose integer indices it lies
  nearest, within the tolerance -- and all refined together with one detector,
  three times. Without that, the first lattice took some 1700 of the second's
  spots by chance within the indexing tolerance (0.3 of an index each way is a
  fifth of a cell), and fitted them. Ambiguous spots (Graeme: left out,
  configurably): within --ambiguity-sigma (3) of each lattice's own RMS
  scatter, in reciprocal space, of a second lattice's point as well as their
  own; --ambiguous exclude, the default, leaves them out, unindexed, so that
  refinement pulls neither model; closest keeps each with the nearer. On
  Graeme's ins2_27: 13617 and 7579 spots, 99.5 per cent, 17 ambiguous; one
  detector, 0.257 and 0.263 px and 0.226 images; scatter 0.00029 and 0.00034
  1/A. mxi refine takes the file as it is. Unindexed spots kept mxi's
  convention, Miller index 0 0 0 and `id` 0, until stage 2 gave them their
  sweep's own experiment (below). Several sweeps with several lattices in one
  indexing: refused still.
* Stage 5, as far as DIALS's default goes: DIALS's 3d integrator, the
  default, finds the overlaps between reflections and masks nothing with them
  (src/dials/algorithms/integration/integrator.py, read for this); only
  3d_threaded marks a neighbour's foreground in a shoebox Overlapped. So one
  lattice is integrated alone as beside the other, and mxi integrate's
  experiment-at-a-time path is already that: each lattice its own profile
  model, from its own spots -- sigma_b 0.0214 and 0.0193, sigma_m 0.0557 and
  0.0437, where DIALS has 0.0227 and 0.0206, 0.0473 and 0.0454 -- 42137 and
  42370 reflections. Joined, the identical models are one again (each run had
  written its own detector and imageset), each lattice keeping its own crystal
  and profile model. Against DIALS, reflection by reflection: profile-fitted
  CC 0.998 and 0.994, median ratio 0.999 and 0.997; summation, above 100
  counts, CC 0.992 and 0.991, ratio 0.985 and 0.976 -- the profile model part of
  that, the rest to find.
* Stage 5, beyond DIALS's default (Graeme: deviate, and mask neighbours
  first; one pass then follows): each box's voxels inside another reflection's
  foreground marked Overlapped (mark_overlapped, sharing build_shoebox's region
  test), left out of its background, kept in its foreground and flagged
  overlapped_bg or overlapped_fg (bits 11 and 12, as DIALS); not learned from
  if its foreground is overlapped. Which boxes meet, by a hash of 32 pixel
  tiles. On by default, --no-mask-neighbours to leave it out, which gives the
  same bytes as before. On insulin, 2 of 21030 boxes meet another and nothing
  overlaps: the same bytes either way.
* Stage 5, each lattice masking the others': the lattices of a sweep --
  experiments on the same image set and scan; a sweep split in two shares the
  one and not the other -- integrated each with the others' reflections
  masked. First every lattice's profile model, each from its own spots,
  before an image is read (--profile-only); then each lattice's run given the
  others with theirs (--mask-also), predicting and boxing them, with their own
  crystals and profile models, for masking alone. Each lattice is still read
  once a lattice: one pass over the images, the profile fitting shared between
  lattices, is a re-architecture left for after (since built: the next stage,
  one pass). On ins2_27: 5756 of each
  lattice's boxes meet another; overlapped background 2552 and 2375,
  overlapped foreground 573 and 573. Against DIALS, unchanged to four places:
  the 205 and 52 strong reflections whose backgrounds were overlapped rose 0.1
  and 0.5 per cent -- the robust background had mostly set the other lattice's
  pixels aside already -- so the 2 to 3 per cent by which mxi's summation lies
  below DIALS's is in what every reflection shares, not in contamination. Two
  faults found on the way: the neighbours had been found before the list they
  index was sorted, so every box was masked with others' reflections and none
  was ever overlapped; and the joined table's imageset_id was numbered before
  the image sets were shared, the second lattice's naming one no longer
  there.
* Stage 5, one pass (Graeme: single pass next): the lattices of a sweep --
  every experiment on one image set and one scan -- integrated in one pass
  over its images, in four steps each the same bytes as before for one
  lattice: a profile model a lattice from its own spots; prediction, boxes,
  masks and rows a lattice; a profile grid and reference profiles a lattice,
  finalised a lattice, an empty block borrowing only its own lattice's
  average; and the routing, with each experiment's own profile block written
  and read (ExperimentList::profiles). --each-alone integrates them a run
  each, as before; postrefinement a run each still, and several sweeps a run
  a sweep (below, stage 2); --gpu
  a fit batch a lattice, each on its own grid -- emulated, the same bit for
  bit as each alone, the CPU's count fitted. ins2_27: the same,
  row for row, as a run a lattice -- but partial_id, the row's number, and the
  fitted intensities to 2e-11 and correlations to 1e-8, the same reflections'
  sums in another order; 16.6 s where a run each took 20.1, on two threads.
  Lattice 1's profiles learned from 4336 reflections, lattice 2's from 1246,
  22 of its 54 regions borrowing.
* Stage 5, overlaps fitted together (Graeme, 6 November: with profile fitting
  on the pixels and every lattice in the one pass, fit overlapping spots
  together): each group of reflections whose foregrounds overlap, of one
  lattice or several, fitted together, each with its own lattice's profile,
  and the summed intensities corrected -- docs/integration.md, Several
  lattices. On ins2_27, the overlapped measurements from +0.92 sigma from
  their equivalents, rms 3.79, to +0.07 and 1.06: the default since,
  `--no-fit-overlaps` each alone.
* Stage 6, symmetry: cosym (src/cosym.cc), in mxi symmetry before the
  pooling, for experiments whose crystals are not one model. Each experiment's
  intensities merged in P1, normalised, Friedel mates averaged; each pair's
  correlation under each of the lattice's rotations, so the matrix r_ik,jl as
  the paper's from n^2 m correlations through the group's product table; its
  leading eigenvectors -- the best fit of low rank to a matrix with every
  entry, which the paper's L-BFGS fits where entries are missing -- with the
  components holding a tenth of the largest's variance or more kept; each
  experiment reindexed by the rotation whose point lies nearest in direction
  to the first's identity point, the identity kept where it is as near, within
  0.1. Then pooled as before. On ins2_27's two crystals: components 54.1 and
  36.5 per cent, then 1.4 -- two clusters; the second crystal reindexed
  h,k,l to -k,-l,h+k+l, its CC with the first 0.942 over 13565 reflections;
  I m -3 at 0.966, I m -3 m refused; I 2 3 by absences. Pooled without cosym
  (--no-cosym), C 1 2/m 1. Planted test: P23 in a P432 lattice, one data set
  indexed through the 4-fold P23 lacks, reindexed to CC above 0.95; one in the
  reference's setting left as it is. For many partial data sets, as the
  paper's examples, the correlations with too few reflections in common will
  want leaving out, and L-BFGS for the missing entries: not yet.
  Against dials.cosym, run by Graeme on mxi's own integration: the same
  Patterson group, I m -3; its reindexing of the second crystal -x+y,y,y-z,
  mxi's h,k,l to -k,-l,h+k+l -- different operators of one coset. Put in the
  conventional basis by b+c,a+c,a+b, both are in 432 and not in 23, the twin
  coset; they differ by a rotation of 23, the crystal's own symmetry, and so
  bring the second crystal to the same setting. mxi's output agrees with
  itself: merged in m-3, the second crystal correlates with the first at 0.934,
  and at 0.409 after the 4-fold m-3 lacks. A lesson in judging it: correlating
  raw P1 indices in the primitive basis, without merging under the crystal's
  symmetry, compares different reflections for different members of a coset,
  and with partial data gave 0.36 for one and 0.86 for another of the same
  coset -- the algebra decides, not that.
* Stage 7, scaling and export: as they were, a scale model and batches an
  experiment, the lattices as the sweeps were -- only the words changed, a
  model an experiment, not a sweep. ins2_27's two crystals: 35 parameters each,
  the second crystal's scale 0.43 to 0.59 where the first's is 1.23 to 1.73;
  CC half 0.997, R merge 0.168 overall and 0.077 at low resolution, 1.81 A by
  CC half. Each crystal merged apart after scaling agrees with the other at CC
  0.987, the second 6 per cent above the first on the stronger half. Exported
  in I 2 3, 80420 reflections, 1200 batches, the second crystal's from 701 --
  1001 since, Graeme's blocks (below) -- one
  data set (one wavelength, as dials.export). So the whole chain runs on two
  crystals in one beam: mxi index --max-lattices 2, mxi refine, mxi integrate,
  mxi symmetry, mxi scale, mxi_export.

  Against dials.scale, run by Graeme on dials.cosym's output from mxi's own
  integration, the same observations, matched by predicted position (0.01 to
  0.02 px apart, mxi having predicted again on its regularised cell): scaled
  intensities above 10 sigma, CC 0.9997 for both crystals, mxi's 0.8 and 0.5
  per cent above; scale factors at the same level, 0.995 and 0.991, varying
  across each sweep alike, CC 0.98 and 0.93. Merged by one and the same
  function, to 1.81 A in m-3: R merge 0.165 and 0.166, CC half 0.9971 and
  0.9970 -- and mean I/sigma 12.5 and 9.4. The difference is the error model:
  mxi's widens a weak reflection's sigma some 38 per cent more than dials's,
  a strong one's some 7: its a, 1.12, where dials's must be near 0.8. To be
  looked into, apart from the lattices; dials.scale's log would give its a
  and b.

  Batches, Graeme's rule: a sweep whose images overlap another's moved up by
  whole blocks, the next power of ten above the largest image number -- 1 to
  600 and 1001 to 1600 for the two crystals, where dials' rule gave 701 to
  1300. One that does not overlap keeps its own numbers, as before.
* Indexing's worst-fitting spots: Graeme, October 2026 -- left out of the
  indexed set, as DIALS leaves them, is fine, the problem being overdetermined,
  but better after the final refinement, the model then good. To do.
* Stage 0's spots planted in the tests: two lattices 11.6 degrees apart on one
  sweep, found and refined; its images, for integration, at stage 5.

## Decisions (Graeme, October 2026)

1. **Sweeps carried through**, as dials/dials#1029 proposes, in mxi's own files;
   the crystal-less experiments dropped only when exporting for DIALS.
2. **Data**: a real data set, two crystals of I23 symmetry, with DIALS's
   processing of it to judge against. I23 in an I432 lattice has a twofold
   indexing ambiguity, so the lattices' settings need a cosym-type analysis.
3. **First case**: two crystals in the beam.
4. **Models shared where they are the same model**, as the experiment list
   records them: a sweep's lattices share its detector, beam and goniometer;
   separate sweeps share a detector where it stood still between them -- as in
   some of Graeme's data -- and not where it moved, as in the four sweeps,
   whose results so do not change.

dials.index finds several lattices in one data set (`max_lattices`): a split
crystal, a twin that is not merohedral, two crystals in the beam. Each lattice
has its own orientation and cell -- its own crystal model, its own matrix --
and its own spots, with their own mosaicity and so their own profile; the
lattices share everything else, because they share the images: the beam, the
detector, the goniometer and the scan. mxi should do the same, and should do
it without the problem dials/dials#1029 describes.

## What DIALS does, and the problem with it

An experiment in DIALS is a set of images with every model needed to predict
from them, the crystal included. Indexing turns each experiment it is given
into one experiment per lattice found: one lattice, one out; two, two out,
sharing the imageset and the scan; none, none out -- the experiment is deleted,
and with it any way to look at its images and the spots that did not index.
Unindexed spots keep `id` -1, which names no experiment. Graeme's issue,
dials/dials#1029 (2019, open): carry every experiment given through
indexing, and add one for each lattice found, so that the count out is always
the count in plus the lattices, and nothing given is lost. The objection in
the issue is the cost in DIALS, where the assumption that every experiment has
a crystal is everywhere -- `if exp.crystal is None: continue`, sprinkled.

mxi can be built so that it is never sprinkled: the distinction made once, where
experiment lists are read, and nowhere else.

## The model

Two things DIALS calls by one name:

* **A sweep**: images, and the models they need -- beam, detector, goniometer,
  scan, the imageset. Every sweep given is carried through every program,
  whether anything is found in it or not.
* **A lattice**: a crystal model and the profile model of its spots, in one
  sweep. A sweep has none, one or several.

In a file -- an `.expt`, or a `.rflx`'s `/experiments` -- the experiment list is
DIALS's, unchanged in form, so that DIALS and dxtbx read it:

* an experiment for each lattice, referring by index to its sweep's beam,
  detector, goniometer, scan and imageset -- shared, as DIALS shares them, not
  copied -- and to its own crystal and profile;
* and, as #1029 proposes, each sweep's own experiment, crystal-less, carried
  through from import: the experiment its unindexed spots belong to.

In a reflection table, `id` is the experiment -- a lattice's, or for an
unindexed spot its sweep's crystal-less one, never -1 for a spot that has a
sweep -- and `imageset_id` its sweep, always.

In memory, the experiment list says which experiments share which model,
rather than each experiment holding its own copy as now; and holds a profile
model for each lattice, where it now holds one for the whole list. Every
program that predicts takes the lattices; every program that reads images
takes the sweeps -- two views of the one list, made in one place, so that no
program asks of an experiment whether it has a crystal.

For DIALS, mxi convert writes what DIALS expects: the crystal-less experiments
dropped, the ids renumbered, unindexed spots -1 again -- that conversion, too,
in one place.

## The programs

**Indexing** (mxi index `--max-lattices N`, default 1). As dials.index:
index; refine; take the spots the lattice indexes out; index what remains;
until N lattices or too few spots are left. A lattice that is the same as one
found -- the same cell and orientation to within the tolerance, or related to
it by the lattice's symmetry -- is not new, and stops the search. A spot that
two lattices both predict within the tolerance goes to the nearer prediction,
and is flagged; a minimum number of spots makes a lattice. Each found lattice
refined with those before it, between cycles, as dials.index does.

**Refinement** (mxi refine). One parameter block for each distinct model, not
each experiment: the lattices of a sweep share one detector, one beam, one
goniometer and so one set of their parameters; each lattice its own crystal,
static or scan-varying, with the sweep's control points. Now each experiment
gets its own six detector and two beam parameters -- fine for separate sweeps,
wrong for lattices on the same images, which would refine as many detectors as
lattices. Outliers judged within each lattice, as now within each sweep.

**Integration** (mxi integrate). The lattices of a sweep integrated together,
in one pass over its images -- not each in a run of its own, as several sweeps
are now, which would read every image once a lattice and leave each lattice
blind to the others' spots. Every lattice predicts; where one reflection's
foreground lies over another's -- of the same lattice or another -- those
pixels are no one's background, and both reflections are flagged as
overlapped and kept out of the reference profiles. Each lattice estimates its
own profile model, sigma_b and sigma_m, from its own strong spots, and learns
its own reference profiles: a split crystal's two halves need not be equally
mosaic.

**Symmetry** (mxi symmetry). Lattices of different crystals -- the first case,
two crystals in the beam -- are separate data sets, each indexed in a basis of
its own choosing among those its lattice's symmetry allows: before they can be
scaled together they must be brought to one setting, and their Patterson group
found from them all. Gildea & Winter (2018), Acta Cryst. D74, 405-410, which
dials.cosym implements, does both at once:

* The lattice group: the highest symmetry the cells allow, its m operations --
  I432's 24, for I23.
* Each data set i under each operation k, its intensities merged in P1 and
  reindexed by it; the correlation r_ik,jl of each such with every other over
  the reflections they share -- a matrix (n m) by (n m), n data sets.
* Each data set under each operation a point x_ik in m dimensions, placed by
  minimising the sum over every pair of (r_ik,jl - x_ik . x_jl)^2, by L-BFGS
  from random starting points in 0 to 1.
* The points' principal components: they fall into as many clusters as the
  lattice group has cosets of the true Patterson group -- two for I23 in I432
  -- and the share of the variance on each component says how many.
* Within one cluster, the operations S_k^-1 S_l between its points are the
  Patterson group; a data set whose point in that cluster carries an operation
  S_k outside the group is reindexed by it -- the twinning operation -- taken
  to the unique representative of its coset of the group (Flack, 1987).

With two data sets the matrix is small and the answer could be had by trying
the ambiguity's operators directly; built as the paper describes, it serves
any number of crystals, and a multi-crystal data set is the next case after
two lattices. The Patterson group so found is then tested, and the space group
chosen by absences, by what mxi symmetry already does for one data set.

**Scaling** (mxi scale). A scale model a lattice, as now a sweep; mxi export a
data set a lattice, with its own batches.

## The plan

Each stage checked before the next, and none changing any result on data with
one lattice: insulin, the four sweeps, the small molecule and Graeme's P 4_1
crystal give the bytes they give now, stage by stage.

0. **Data.** A planted data set: two lattices predicted onto one sweep, of known
   orientations, cells and profiles -- including a pair whose spots overlap.
   And Graeme's two crystals of I23 (ins2_27, October 2026): 600 images of
   0.1 degree on an Eiger 4M at 170 mm, 21299 spots. One lattice indexes 71.9
   per cent of them, insulin's primitive cell, 67.47 A and 109.47 degrees; the
   rest, indexed alone, 98.6 per cent of theirs to a second, 67.51 A, the two
   11.6 degrees apart under cubic symmetry -- two crystals, as said. mxi
   marks a spot unindexed by its Miller index and flags, `id` staying 0: stage
   2's crystal-less experiment is where those rows' `id` will point.
1. **Shared models.** The experiment list's sharing kept, read and written by
   index; a profile model a lattice; the two views, sweeps and lattices.
   Refinement's parameters a block a model, separate sweeps sharing theirs as
   DIALS shares them (decision 4). On every data set of one sweep, the same
   results; on the four sweeps, a change, set against dials.refine's -- which
   shares -- and judged on its own.
2. **Carrying sweeps through** (#1029): the crystal-less experiments, `id` and
   `imageset_id`, every program reading the list through the two views;
   mxi convert's DIALS form. A sweep that indexes nothing still there, its
   spots on its images.
3. **Indexing several lattices.** On the planted data, both lattices found, to
   their true cells and orientations; one lattice not found twice; on the real
   data, the lattices dials.index finds.
4. **Refining them**, shared detector and beam, separate crystals: on the
   planted data, the true models; on the real, dials.refine's.
5. **Integrating them** together, overlaps flagged, a profile model each: on the
   planted data, each lattice's intensities as they were planted, overlapped
   spots flagged and the rest unharmed; on the real, against dials.integrate.
6. **Symmetry** of several lattices, cosym as above: on planted data -- two data
   sets of an I23 crystal, one reindexed by the I432 operation I23 lacks, and
   then many partial ones, as the paper's examples -- the Patterson group and
   each data set's operation recovered; then on Graeme's two crystals, against
   dials.cosym.
7. **Scaling and export** of several lattices: a scale model and a data set a
   lattice; against dials.scale.
8. The documents.

## Against DIALS: ins2_27

Graeme's DIALS processing of ins2_27 -- indexed and integrated, its tables in
DIALS's HDF5 form, read for this with h5py alone (mxi does not read them;
docs/rflx.md).

**Indexing.** The same form as mxi's: two experiments, one detector, one beam,
one imageset, two crystals. Each of mxi's lattices is one of DIALS's, the
bases related by an integer matrix of determinant +1 off integers by at most
0.0003 -- the same orientations. DIALS found 21315 spots, mxi 21299; matched
within half a pixel and image, 21299. DIALS indexes 13348 and 7445, 522 not
(id -1); mxi 13617 and 7579, 103 not, 17 of them left out as ambiguous. Of the
20763 both index, 20762 are given the same lattice and the same indices. The
433 only mxi indexes lie three times as far from their points as the rest --
0.025 of an index, median, against 0.008 -- and are no more often near the
other lattice than any spot is: DIALS leaves its worst-fitting spots out of
the indexed set, it seems by outlier rejection in its final refinement, where
mxi rejects them from its fit and leaves them indexed. A convention, to be
decided.

**Integration** -- the target of stage 5. A profile model a lattice: sigma_b
0.0227 and 0.0206 degrees, sigma_m 0.0473 and 0.0454; scan-varying crystals.
Predicted 42107 and 42244; summation-integrated 41569 and 41739, profile-fitted
40678 and 40827. Foreground with bad pixels 538 and 505; background 2712
and 2356. No reflection flagged overlapped -- DIALS's overlap filter is off by
default; each reflection's background excludes others' foregrounds instead.

## Questions settled

Graeme's answers to the plan's questions are the decisions above, and his
two-crystal data is here, and DIALS's processing of it -- indexing,
integration, cosym and scaling -- judged against throughout above.

## How much of a lattice was seen: mxi verify

Indexing reports the share of spots a lattice explains. That can look healthy
while the lattice is wrong: a cell doubled on every axis still indexes every
spot, as does a lattice that has absorbed a second crystal's. The converse
question catches them -- of the reflections the lattice predicts, how many
were seen?

```sh
mxi verify indexed.rflx
```

For each lattice, every reflection it predicts over its scan, to the
resolution its indexed spots reach (the d that 99.5 per cent of them reach, so
that a few misindexed spots far out do not carry it; `--d-min` to choose), in
shells of equal reciprocal volume (`--shells`, 10). A line a shell, and one
for all: the reflections predicted, those unobservable, those of the rest
observed, the fraction observed, and how many of those were indexed to the
lattice.

It is a diagnostic, run when wanted: it reads the file, writes nothing but its
log, `mxi_verify.log`, and no step needs it. It is not part of indexing, where predicting every
reflection of every candidate would slow the search for no need.

**Observed** is judged as indexing judges a spot, not by what indexing
assigned. Every strong spot of the lattice's sweep -- indexed or not, to any
lattice -- is given fractional indices by the lattice's matrix, and a
prediction is observed where a spot's lie within 0.3 of its own on all three
(`--tolerance`, mxi index's), the spot taken by the nearest in rotation of its
reflection's crossings. Judged at first by indexing's assignment, a spot in
place but not indexed -- outside the tolerance before refinement, as many
are at high resolution -- counted as missing, and on Graeme's data the outer
shells fell for it, to 13 per cent. The column **indexed** keeps that measure:
observed and indexed to the lattice with those indices. The two apart say how
much of a shortfall is indexing's assignment rather than absence.

**Chance.** A stray spot -- ice, another crystal, noise -- has fractional
indices within 0.3 of a whole number on all three axes 0.6^3, a fifth, of the
time, so it can make a reflection observed that is not: the same strays
indexing takes. The test's 20-degree misplaced spots observed 6 per cent of
predictions this way. A smaller `--tolerance` tightens it.

Predictions whose centre falls on a masked pixel -- the detector's own mask,
module gaps, the regions marked untrusted -- cannot be seen, are counted
apart, and are left out of the fraction; the images are read for the mask
(one frame), and without them, or with `--no-mask`, it says so. Predictions
off the detector are not predicted at all.

What to expect. Weak reflections are not found as spots, so the fraction falls
with resolution even for a right lattice, and the outer shells say more about
the data than the lattice. On Graeme's data, a set diffracting weakly to its
limit fell steeply with resolution, every shortfall a weak reflection, not a
missing one; a strongly diffracting crystal kept about 84 per cent observed in
its outer shell. It is the low-resolution shells, where every reflection
should be strong, that judge a lattice: well short of 100 per cent there, the
lattice predicts reflections that are not there. A supercell shows it plainly
-- an eighth for a cell doubled on every axis -- and so does the wrong one of
two crystals. It reads any file with lattices: indexed, refined,
integrated (whose rows are predictions, the flags saying which were seen) or
scaled.


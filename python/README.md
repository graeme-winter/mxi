# mxeq

Equivalence checks between two runs of an MX processing pipeline, boundary by
boundary, without cctbx -- and, for integration, tools that find and explain
where two runs differ.

```sh
mxeq check strong      dials/strong.refl     gpu/strong.refl
mxeq check indexed     dials/indexed.refl    gpu/indexed.refl -e dials/indexed.expt
mxeq check refined     dials/refined.expt    gpu/refined.expt
mxeq check integrated  dials/integrated.refl gpu/integrated.refl -e dials/refined.expt
mxeq check scaled      dials/scaled.refl     gpu/scaled.refl     -e dials/scaled.expt
```

The target is **equivalence, not identity**. Identity is achievable for
thresholding and stops being achievable somewhere around integration, and a
checker that demands it would report every difference in summation order as a
failure. So each boundary is compared on quantities that mean the same thing on
both sides, and the tool reports distributions rather than verdicts.

## Comparing two integrations

`check` says whether two runs are equivalent at a boundary. These say where and
why two integrations differ:

```sh
mxeq explain  ours.refl theirs.refl                 # why rows went unpartnered
mxeq trend    ours.refl theirs.refl --value intensity.prf.value
mxeq html     ours.refl theirs.refl -o comparison.html
mxeq disagree ours.refl theirs.refl -o disagree.refl --sigma 5
mxeq residuals integrated.refl -o residuals.html    # one file: predicted positions
mxeq profiles profiles.txt -o profiles.png          # mxi_integrate --save-profiles
```

**The ratio is always the first file over the second**, so the order of the
arguments sets which way a ratio above one points. Put the one being judged
first.

**Rows are paired as observations, not reflections.** Miller index and entering
flag group them and the frame separates them, because a sweep of several turns
records each reflection many times; `--radius` (5 images) is a sanity check and
not a discriminator. Every report states how many matched. **That count should
be close to the smaller table's rows**, and when it is not, nothing after it is
worth reading -- run `explain`, which says for each unpartnered observation
whether the other program did not predict it, put it in another turn, disagreed
about its side of the Ewald sphere, or placed it further than the radius.

Bins hold equal populations, since equal widths on a quantity like I/sigma put
nearly everything in the first bin. The band on a ratio is the robust spread,
not the standard error.

`trend` and `html` bin against everything a disagreement might follow:
resolution, I/sigma in the second file, |zeta|, partiality, and -- from the
first -- profile correlation, foreground pixels and background, then distance
from the beam centre and frame. `disagree` writes only the disagreeing rows, ours
with the other side's value alongside, for `dials.image_viewer`; its three
criteria find different things -- `--factor` proportional error, `--difference`
absolute counts, `--sigma` the only one that knows whether a difference is
larger than the measurement -- and each one given must be exceeded.

`residuals` reads `xyzres.px`, the observed centre less the predicted, and
shows it against image, resolution and detector position, with a summary that
separates the systematic offset, the counting noise and the prediction error.
See `docs/integration.md` for what it cannot tell you in z.

## Two scaled data sets, one unique reflection at a time

`mxeq unique a.expt a.refl b.expt b.refl` -- DIALS's scaled data against mxi's,
say -- matches the two data sets' unique reflections and, by resolution shell,
splits the difference in I/sigma into its three parts: the mean intensity's
ratio (a scale or a bias), the root of the multiplicity's (outlier rejection,
observations lost) and the inverse of the per-observation sigma's (variance
estimates, the error model). Their product is the ratio of I/sigma, per
reflection. CC1/2 does not see sigma, so each reflection's scatter -- the
standard deviation of its observations -- is compared too: less scatter is
observations that agree better; the same scatter with a smaller sigma is the
error model -- and scatter over each reflection's mean intensity, in which a
difference of overall scale between the two cancels. Each data set's own CC1/2, I/sigma, multiplicity and chi2/nu over
the reflections both have are given, and how many observations its table flags
overloaded -- DIALS does, mxi does not apply the trusted range. Intensities are
what merging uses, `intensity.scale.value` over `inverse_scale_factor`; the
second data set is reindexed by whichever lattice operator makes the two agree
best. `--csv` writes every matched reflection.

## The background against resolution

`mxeq background integrated.expt integrated.refl -o background.png` takes each
reflection's `background.mean` -- every mxi or DIALS integrated table has it --
and the beam centre from the experiments, and prints the background by
resolution shell: its median, its spread about the median, and the share of
reflections below half or above twice it. The figure has four panels: the
background against 1/d^2, as a density, with its running median and 5th and
95th percentiles; each reflection's background over that median against the
azimuth around the beam, and against the image; and that ratio on the detector
near the beam. The background from air and the sample's surroundings should be
a smooth function of resolution and polarisation, so this is for looking at
before modelling it (`docs/backstop.md`). `--range DMAX DMIN` limits the
table to that range and adds tables of the ratio against the azimuth and the
image there -- the ratio still against the median over all, so a range does
not move its own reference -- and limits the azimuth and image panels;
`--zoom` sets the map, `--no-plot` gives the tables alone.

## A model of the background, and the table filtered by it

`mxeq background-model integrated.expt integrated.refl -o filtered.refl`, a
prototype (`docs/backstop.md`), fits each reflection's background to

    B = R(s) . G(phi) . P . Omega . Q

-- R and G cubic B-splines in s = 1/d and in the rotation, G periodic over a
full turn, penalised for roughness; P the polarisation, Omega the pixel's solid
angle and Q the sensor's efficiency, computed from the experiment list -- in log
space, robustly (Tukey's biweight, iterated), each reflection weighted by its
background's counting variance and an intrinsic spread from the residuals. A
reflection whose z lies beyond `--z-max` (5) is flagged, and one whose
background is too low -- by default only those, being the biased;
`--reject-high` takes the too-high too -- has, in the filtered table, its
integrated flags cleared and excluded-for-scaling set, so that neither
`mxi_scale` nor `dials.scale` takes it. Every reflection gains
`background.expected` and `background.z`. The report gives, by resolution
shell, the median of observed over model, the z spread -- 1 where model and
counting explain the backgrounds -- and how many were flagged low and high;
`--reject-shells SPREAD` leaves out whole shells beyond it, and
`--reject-inner SPREAD` the innermost resolutions whole -- outward from the
lowest, in fine shells of equal width in 1/d (`--inner-width`, 0.005), while
each shell's z spread exceeds SPREAD -- for beside a backstop, where no
background is normal and z cannot tell the unharmed from the attenuated; the
report gives the limit, a `--d-max` the data chose. `--annotate-only`
adds the columns and changes no flag, so that the flagged can be judged
against their equivalents first: `mxeq equivalents` bins by `background.z`
where the table has it. The report says the background is too low or too high,
not why, and lists the fine resolution bins richest in the too-high, where a
ring too narrow for R's spline -- ice -- would pile up. `--plot` draws R
and G through the data, z against resolution, z on the detector by the beam,
and where reflections were removed -- background too low, too high, by whole
shells -- and kept,
over the whole detector and by the beam. `--knots`, `--phi-spacing` and `--smoothness` shape the splines.

## Attenuation, judged from the equivalents and the background together

`mxeq attenuation scaled.expt scaled.refl` -- a prototype (`docs/backstop.md`)
-- judges each observation scaling considered, its own outliers among them,
good or attenuated: good, I ~ N(Ibar, sigma^2) with sigma at Ibar; attenuated,
I ~ N(T Ibar, sigma^2) with T uniform on (0, 1), so that attenuation only
lowers. The prior is from its background against its reflection's 75th
percentile -- each divided first by the background model's expectation where
`mxeq background-model --annotate-only` has added one -- and expectation-
maximisation finds Ibar. A reflection too weak for its equivalents to judge, or
observed fewer than three times, is judged by its background alone, against the
model's expectation; without one it is kept. Observations more likely
attenuated than good are flagged: `--apply-to symmetrized.refl --next
next.refl` writes `mxi_scale`'s next input with them left out, to iterate until
nothing more is flagged, and `--show H,K,L` lists one reflection's judgement.
Flags accumulate: one flagged is not judged again.

## Every observation of a reflection

`mxeq observations scaled.expt scaled.refl 1,1,1 2,2,2` lists every observation
of the reflections given -- all their symmetry equivalents, by the experiment
list's own space group, so a scaled table gives them all where an integrated
one gives only what its indexing setting relates -- with whatever the table
records: position, summed and fitted intensities, background, profile
correlation, mxi's reason for not fitting, the background's dispersion and,
after `mxeq background-model`, its z and expected value, LP, partiality, the
flags, and for a scaled table the scale factor and scaled intensity. Columns a table does not
have are left out, so DIALS's tables list too.

## Where profile fitting fails

`mxeq failures integrated.refl` reads the `profile.failure` column
`mxi_integrate` writes -- each reflection's reason for not being profile fitted,
0 if it was -- counts the reasons, and bins the failures by position in the
scan, |zeta|, the box's depth and width, place on the detector, resolution and
summed I/sigma (`--code N` for one reason). A cause shows as the variable whose
bins differ sharply where the others are flat.

## Two experiment lists

`mxeq compare-expt a.expt b.expt` puts two experiment lists' models side by
side -- the beam's wavelength and direction; each panel's origin, axes, pixel and
image size, trusted range and sensor; the goniometer's axes and angles; the
scan's images and angles -- and says of each "same", within tolerances small
enough that a difference means something, or by how much it differs. It exits 1
if anything differs. For `mxi_import` against `dials.import` on each new data
set (`docs/import.md`).

## Bias against the data itself

`mxeq equivalents scaled.expt scaled.refl` compares every observation with the
symmetry equivalents of its reflection, so that a bias in one kind of
observation shows without a second program or a reference data set.

* **The intensities** are corrected as scaling corrects them -- times `lp`, over
  `qe` and the partiality, over `inverse_scale_factor` -- since the integrated
  columns are raw.
* **The reference** is the unweighted mean of the reflection's CLEAN equivalents,
  itself left out: fully recorded, nothing of the profile masked, away from the
  scan's ends. Clean, or a bias leaks into the reference (planted partials 10
  per cent high read low against every equivalent); unweighted, because weights
  from each observation's own variance favour those that came out low, and read
  unbiased data +0.7 per cent high at I/sigma 10 to 20. `--reference weighted`
  compares.
* **The tables** bin the relative difference by partiality, by
  `profile.measured` -- the profile's fraction on valid pixels, which a module
  gap lowers -- by the masked-foreground flag, by images from each end of the
  scan, by the background's dispersion where the table has it
  (`mxi_integrate --save-background-parameters`), and by resolution
  (quantiles of what is counted) and I/sigma as controls; each counts, by default, only the observations clean in every other
  respect (`--all` for every one). Read the median and the ratio of the sums.
* **sigma_m from the partials.** A partial's intensity before the division by
  its partiality, against its clean reference, is its observed partiality.
  Partiality is recomputed as the integrator computes it, for a trial sigma_m,
  and the report gives the sigma_m at which the partials' median bias is zero --
  with the sigma_m the integration used, recovered from each partial's own
  partiality.
* **`--worst N`** writes the N observations furthest from their equivalents to
  `equivalents_prf.refl` and `equivalents_sum.refl`, with
  `equivalents.reference` and `equivalents.difference` beside our columns, for
  `dials.image_viewer`.

On the 300 image insulin sweep: partials of partiality 0.8 to 0.9 +9.5 per cent
and 0.9 to 0.99 +2.4, fully recorded +0.2; 80 to 95 per cent of the profile on
valid pixels -3 to -4; the excess at the scan's ends the partials'. The partials
recover the 0.1286 degrees integrated with, and suggest 0.086 (profile fitted)
and 0.082 (summed) -- where DIALS's profile model has 0.089. `mxeq trend`,
comparing two integrations, bins by `profile.measured` too.

## No thresholds

Version one applies no pass/fail criteria at all. Every check prints
measurements; a human decides what is acceptable. This is deliberate: choosing
a tolerance before seeing the distribution on real data means choosing it by
guessing. The `--json` output carries the same numbers as the text, so
thresholds can be written against it once there is something to write them
against.

Exit status is zero whenever the comparison ran. Non-zero means it could not
run, never that the two disagreed.

## Two ways to run the pipeline

Both are needed, and they answer different questions.

**Pinned.** Each stage is given DIALS' upstream output as its input. This
isolates one stage: a difference in the integrated intensities is the
integrator's, because both integrators were handed the same experiment model
and the same predictions.

**Cascade.** Each stage is given the previous stage's own output. This is the
number that matters, because it is what a user of the pipeline gets. It is also
the one that is hard to read without the pinned run beside it: a hundredth of a
degree of orientation difference at refinement moves every shoebox by a
fraction of a pixel, and the integrator then looks wrong when it is not.

Run pinned first. When pinned agrees and cascade does not, the compounding is
the finding.

## Install

```sh
pip install -e .
```

Dependencies are numpy, scipy, msgpack and gemmi. Notably **not** cctbx or
DIALS: `.refl` is msgpack -- or HDF5, DIALS's layout (dials/dials#3255) or
dxtbx-h5's, read with h5py when it is -- and `.expt` is JSON, and both are read directly, so
the checks run in a container with no crystallographic software in it at all.

## When a file will not read

```sh
mxeq inspect strong.refl
```

prints the columns and their C++ types, and if the file is not the shape this
expects, falls back to walking the msgpack document without assuming any of the
key names. That output is the bug report.

## The boundaries

| boundary | join | headline metric |
| --- | --- | --- |
| `strong` | spatial, mutual nearest neighbour | matched fraction, and what did *not* match |
| `indexed` | spatial, then reindexing operator, then Miller index | agreement after reindexing |
| `refined` | none; models compared directly | cell, misorientation, detector distance |
| `integrated` | (Miller index, entering, id) | relative difference and pull, in resolution shells |
| `scaled` | as integrated, plus a merge into the ASU | CC-half of each, and CC between them |

`docs/boundaries.md` explains why each join is the way it is.

## Reindexing

Two indexing runs on the same images can produce the same lattice in a
different basis. `mxeq check indexed` finds the operator from the data --
spatially match the spots first, then search the candidate operators for the
one that maps A's indices onto B's -- and prints it. Pass it to the later
boundaries:

```sh
mxeq check integrated a.refl b.refl -e a.expt --operator 0,1,0,0,0,1,1,0,0
```

Without it the keyed join fails, which the tool says plainly rather than
reporting a near-total disagreement in intensity.

## Status

The `.refl` format is validated against real `dials.find_spots` and
`mxi_find` output, and `tests/data` holds a 48-row cut of it so
that is checked on every run. `.expt` is read from real `dials.import`,
`mxi_import` and `mxi_scale` output by `compare-expt` and `equivalents`, and
`tests/test_import.py` holds `mxi_import`'s against `dials.import`'s.

The first version of the reader was wrong about the format in two ways and its
entire test suite passed, because the only format check was a round-trip
against its own writer. `CLAUDE.md` records that; it is the reason
`tests/test_format.py` asserts on raw bytes.

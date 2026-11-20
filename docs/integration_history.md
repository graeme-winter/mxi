# Integration: the notebook

**This is a record, not a reference.** It was written as the work happened, in
the order it happened, and later sections overturn earlier ones -- sometimes
flatly. For what the integrator does now, how to run it, what it writes and what
is still open, read `docs/integration.md`. Read this for *why* it is the way it
is, and for the mistakes that are recorded so as not to be made twice.

Nothing below has been edited to agree with what came later, with one
exception: a block of sections -- the precision contract, how it would be
compared, and `mxeq trend` -- had been appended twice, the second copy an older
draft that attributed the near-axis outliers to overlap. The corrected copy
records that the overlap guess was tested and was wrong, so the older one was
removed (119 lines). That is the failure CLAUDE.md warns about under "a silent
replace leaves two documents that disagree".

## What later sections overturned

Read the right-hand section before trusting the left.

| An early conclusion | Overturned by |
| --- | --- |
| "Where this stands": integration not started | everything after it; see `docs/integration.md` |
| "What is still missing": Parkhurst (2016) and Leslie (1999) | both obtained and implemented |
| The anisotropy hunt | "Rendering the model onto the pixels" -- the comparison, not the model |
| Near-axis outliers as overlap | "What the outliers are: the near-axis reflections" |
| Two passes over windows of frames | "Reading each frame once a pass" |
| The fitted variance measured against DIALS after the variance fix | measured on mismatched inputs; confirmed instead by scaling |
| Gap recovery "unfinished", 1.1 per cent where 4.5 was expected | mismatched inputs; on matched data ours is 3 per cent low where a tenth to a third is lost, DIALS 22 |
| A constant -0.15 image z residual, from the first `xyzres` table | mismatched inputs, and meaningless |
| The z residual sinusoid as rocking-curve asymmetry | "The pixel plant": it grows with narrowness, not width |
| Module-edge outliers as overconfident variances | "The outliers along the module edges were a flag, not an intensity" |
| The z sinusoid as the integrator's centre following its foreground window | "What the z offset follows is strength": the spot finder's centre, not the integrator's |
| "Two runs at the same thread count agree exactly" ("Making the second pass pay for itself") | measured wrong: two four-thread runs differ by up to 3.6e-11 in profile-fitted intensities, because profile learning gives reflections to whichever thread is free; only one thread reproduces exactly. Summation is exact either way. Worse, it was a data race: see "Deterministic profile learning" below. Fixed |

**Mismatched inputs.** From the arrival of the 1800 image `i04-ins-small` model
until the end of the record, both `.expt` files in the working directory were
that model while the only images were the 300 image `minute` sweep. Real-data
numbers measured *here* in that period paired one dataset's images with
another's model. Numbers from the maintainer's own runs -- the comparisons, the
scaling, `integrated_3.refl` against `integrated_2.refl` -- were matched and are
sound. Each affected section says so.

**Read `## Order of work, revised` first.** Several sections above it reach
conclusions that later work overturned; they are marked as superseded and kept
because the reasoning is instructive.

Not started. This is the plan, what it is bound by, and what is missing before
parts of it can be written at all.

## Where this stands

Not started. The profile model is implemented and the tools to judge it exist;
nothing integrates yet.

*(`mxi_mask` in this history is the program now called `mxi_foreground`: it
writes each predicted reflection's foreground region for dials.image_viewer.
Its name went to the mask editor on 22 October, `docs/mask.md`.)*

| | |
| --- | --- |
| profile model | implemented, `mxi_profile`; does not yet match DIALS |
| the region | implemented, `mxi_mask`; box and ellipsoid |
| forward model | implemented, `mxi_forward`; agrees with the data to four per cent |
| background | not started; needs Parkhurst et al. (2016) |
| summation | not started; needs Leslie (1999) |
| profile fitting | not started; needs the reference profiles of Kabsch section 3.3 |

Neither sigma agrees with DIALS. **Run `mxi_profile` for the numbers**: it
prints both, and the disagreement with each, every time. They are not repeated
here or in the README, because a number copied into prose is wrong from the
first time either estimator changes and nothing checks it.

What the forward model says about them is the most useful reading: rendered
onto the pixels, the observed spot is about three per cent narrower than the
model on the detector and five per cent narrower in rotation, so both sigmas
are a little large and by far less than the raw comparisons suggested.

Read `## Rendering the model onto the pixels` before anything below it. Several
earlier sections reach conclusions it overturns, and they are marked.

## What this follows

DIALS, transcribed, as the spot finder transcribes `DispersionExtendedThreshold`
window for window. That decision was taken earlier and it still holds: a
transcription can stand in for `dials.integrate`, while an integrator built on
this project's own ideas would be a fast pipeline that agrees with nothing
anyone runs.

Its cost is real and worth restating. Every entry in the diff checklist from
`dials_algorithm_map.md` becomes a specification rather than an interesting
divergence -- the 2D-disc foreground mask, the inert `filter.threshold`
parameter, LP and QE stored as columns and applied at scaling rather than at
integration, the GLM background. Some of those are things a research pipeline
would deliberately do differently. Divergences go in as run-time options that
default off, each with its reason recorded, so that a disagreement with DIALS is
always a choice and never a drift.

## What integration is, per Winter et al. (2018), §3.4

Three steps, in order:

1. **Profile parameters.** A three-dimensional Gaussian in a local
   reciprocal-space frame, with two parameters: `sigma_D`, the extent on the
   detector face, and `sigma_M`, the extent over images. Estimated from the
   indexed strong spots, per Kabsch (2010a).
2. **Background.** Shoeboxes are read from the images, and the background under
   the peak is modelled from the non-peak pixels around it. The default is a
   robust generalised linear model that assumes the counts are Poisson, which
   matters below one count per pixel where a normal approximation is biased
   (Parkhurst et al., 2016).
3. **Intensity.** Summation of background-subtracted pixels in the peak region,
   with errors from Poisson statistics (Leslie, 1999); and profile fitting,
   where the shoebox is transformed into the local reciprocal-space frame and
   fitted against a reference profile (Kabsch, 2010a).

Two details from the paper that are choices rather than consequences, and so
have to be copied deliberately:

* **DIALS differs from XDS in the transform.** Counts are distributed onto the
  reciprocal-space grid by computing the overlap of each detector pixel with
  the transformed grid point using Sutherland–Hodgman polygon clipping, rather
  than by XDS' assignment. This changes the profile and therefore the fitted
  intensity.
* **Blocks overlap by half.** Images are integrated in blocks whose start is
  aligned with the centre of the preceding block, so that most reflections are
  whole within one block. Reference profiles are built per block, at several
  points across the detector, each strong reflection contributing to its
  nearest profiles with a Gaussian weight by distance.

## What is still missing

Winter et al. (2018) defers to others for everything numerical. Two of those
are still not here, and each blocks one stage:

| | for | blocks |
| --- | --- | --- |
| Parkhurst et al. (2016) | the Poisson GLM background | background |
| Leslie (1999) | summation error estimates | summation |

Kabsch (2010a) and DIALS' own `calculator.py` are here, and between them they
settled the profile model. From the paper:

* §2.3, the `{e1, e2, e3}` frame and the mapping of a pixel to `(eps1, eps2,
  eps3)`, with `zeta = m2 . e1` correcting for the path length through the
  Ewald sphere;
* §3.1, the reflection mask `|eps1| <= delta_D/2`, `|eps2| <= delta_D/2`,
  `|eps3| <= delta_M/2`, and the estimators: `sigma_D^2` as the mean of the
  per-spot variances of the intensity-weighted beam directions, and `sigma_M`
  by maximising the likelihood of the observed offsets under `R(Delta,
  sigma_M/zeta)`;
* §3.3, the profile grid, the `f_3j` fractions that split a frame's counts
  between grid planes, and the 5x5 subdivision of each pixel that DIALS
  replaces with polygon clipping;
* §3.4, the fitted intensity `I = sum (c - b) p / v / sum p^2 / v`, with
  `v = b + I p` iterated from `v = b`, three cycles.

**Pixels: this package had them all along and was skipping them.** A
`strong.refl` carries a `Shoebox<>` column -- 13.6 MB of it for insulin -- and
the reader dropped it as an undecoded type. `src/shoebox.hh` now decodes it. The
layout was derived from a real `dials.find_spots` file and checked against all
13766 of its records:

    int32          panel
    int32 x 6      bbox as x0, x1, y0, y1, z0, z1, half open
    uint8          a flag, 2 in every record seen
    float32 x N    data          N = (x1-x0)(y1-y0)(z1-z0)
    uint8  x N     mask          0 and 5, which is Valid | Foreground
    float32 x N    background    all zero out of dials.find_spots

Every bounding box agrees with the table's own `bbox` column and the records
consume the blob to the byte: 1467208 voxels holding 14163216 counts.

So the profile model can be estimated here, on real data, with no images and no
HDF5. What still needs the images is integration proper -- the shoeboxes of the
*predicted* reflections, which are a superset of the strong ones and are not in
any file this package has.

## sigma_D and sigma_M, against DIALS' own source

    mxi_profile refined.expt refined.refl

With `calculator.py` to hand the recipe is no longer guesswork. Three things it
settled, all of which had been wrong here:

**The reflections.** DIALS does not use every strong spot. It selects those
flagged `used_in_refinement`, then cuts on `|zeta| >= 0.05`. On 1800 images of
insulin that is 70425 of 78618, and it moves sigma_b from -4.2 per cent of the
DIALS value to -2.9 and sigma_M from a factor of three to twenty per cent.

**zeta uses the CROSS product.** `e1 = s1 x s0`, normalised -- the axis about
which the point would cross the Ewald sphere by the shortest route, Kabsch
section 2.3 after Schutt & Winkler. This had `s1 - s0`, which is the reciprocal
lattice vector and points somewhere else entirely. Correcting it moved sigma_M
from +50 to +21 per cent.

**Two places DIALS departs from Kabsch**, both followed here because standing
in for `dials.integrate` is the point:

* the background is NOT subtracted before the angular spread is measured, where
  Kabsch section 3.1 step (v) says to. DIALS' source carries a note saying so.
  It costs nothing on a table out of `dials.find_spots`, where the background is
  zero, and it would matter on one where it is not;
* an image contributes to the reflecting range if the spot finder marked any
  pixel on it as valid foreground, whatever the counts. Requiring counts as
  well is a different criterion that happens to select the same images here.

DIALS' `R` is a partiality rather than a density -- it is not divided by the
oscillation width as Kabsch writes it -- but that is a constant in the log and
the argument of the maximum is identical.

Both still disagree; `mxi_profile` prints by how much. The figure quoted here
was 0.030786 for sigma_b until the pixel-to-millimetre conventions were matched
two sections below, which moved it -- which is the argument for not quoting it
at all.

The remaining candidate is in code this has not read: `Shoebox::beam_vectors`,
which decides exactly which lab coordinate a pixel maps to. Parallax applied
the other way is not it -- that moves sigma_b to +24 per cent and inverting it
to -14, so DIALS is doing neither.

The estimators themselves are checked against planted values rather than
against DIALS: a known angular spread comes back exactly, and samples drawn
from the reflecting-range model with a known sigma come back within five per
cent at 0.05, 0.1 and 0.3 degrees.

## Where the centre is, and the half pixel that was hiding in it

Every measurement here maps a pixel into the frame of a reflection:

    lab   = origin + mm_fast * fast + mm_slow * slow
    s'    = lab * |s1| / |lab|                       elastic, so |s'| = |s1|
    eps1  = degrees( e1 . (s' - s1) / |s1| )
    eps2  = degrees( e2 . (s' - s1) / |s1| )
    eps3  = degrees( zeta * (phi_image - phi_calculated) )

The inputs are the panel geometry, the beam, the rotation axis, `s1`, and
`phi`. What was wrong was the first line: `mm` came from a plain multiplication
by the pixel size, while the `s1` it is compared against was built from
`xyzobs.mm`, which is parallax corrected. Two conventions, differing by about
half a pixel radially.

A width cannot see that, which is why it survived every test here. Comparing
each spot's centroid with its own centre in the frame does see it:

    eps1   mean -0.000007 deg   -0.000 sigma      sd 0.014 sigma
    eps2   mean +0.013506 deg   +0.439 sigma      sd 0.110 sigma
    eps3   mean +0.000176 deg   +0.001 sigma      sd 0.127 sigma

A systematic 0.44 sigma in the radial direction and nothing in the other two --
which is the signature of a radial displacement, not of anything physical.
Mapping the pixel through `px_to_mm` instead takes all three to zero within a
millionth of a degree.

**It costs agreement with DIALS.** sigma_D moves from 0.0308 to 0.0274 degrees,
from -2.9 per cent of the DIALS value to -13.6. That is recorded rather than
tuned away: a mapping that is demonstrably inconsistent cannot be the right one
however well its number happens to agree, and the remaining difference is now a
cleaner question than it was.

It also means `s1` in a DIALS reflection table is the OBSERVED scattering
vector, not the predicted one -- computed from `xyzobs.mm`. So these
measurements are centred on the observation, and the centroid sitting at zero
is a check on the arithmetic rather than a result. The mask in `mxi_mask` is a
different matter: it is built on the PREDICTED `s1`, which is what it must be,
since the point of a mask is to say where the model expects the signal.

## Rendering the model onto the pixels

    mxi_forward refined.expt refined.refl --out forward.txt

Every comparison before this one took a number from the data through a pixel
grid that truncates and quantises it, and a number from the model in closed
form. At widths below a pixel that difference is most of what was being
measured, which is why every conclusion carried the same caveat.

This renders instead. The model is integrated over the same pixels, the same
images and inside the same mask as the observation, and both are reduced the
same way, in pixels and images. Whatever the grid does to the data it does to
the model. What is left is the model being wrong.

The answer is that it is barely wrong at all, over 61047 spots:

    sensor model            shift fast   shift slow   obs/model fast  slow    z
    depth distribution        -0.008      -0.002          0.967     0.964   0.948
    mean depth, no smear      -0.008      -0.002          0.979     0.987   0.946
    no sensor                 -0.007      -0.051          0.984     1.004   0.946

**The positions agree to a hundredth of a pixel** once the sensor is in.
Leaving it out leaves a systematic 0.05 pixel shift in the slow direction and
nothing in fast, which is the parallax displacement and is the size it should
be.

**The widths agree to within four per cent**, not the factors that every
earlier measurement suggested. The captured-fraction test said one sigma held
0.74 where a Gaussian holds 0.47; rendered onto the grid, the observed spot is
3.3 per cent narrower than the model on the detector and 5 per cent narrower in
rotation. Nearly all of that apparent disagreement was the comparison, not the
model.

Taken at face value it says `sigma_D` should be about 0.0265 rather than 0.0274
and `sigma_M` about 0.113 rather than 0.119 -- small corrections in the same
direction as everything else, and much smaller than they looked.

### And the anisotropy is predicted

    anisotropy slow / fast    observed 1.074
                              model    1.092   with the depth distribution
                              model    1.076   with the mean depth only
                              model    1.062   with no sensor

The model predicts an anisotropy of the same size as the observed one, and
gets closer as more of the sensor goes in. So the elongation is geometry and
absorption acting on an isotropic Gaussian, not a missing physical effect: the
projection onto a flat detector and the depth at which a photon stops are
enough to produce it.

That is the answer to a question asked several ways in this document and
answered wrongly twice. The cubic crystal does argue for isotropy, and the
spots are consistent with an isotropic model -- once the model is compared with
the data on the data's own terms.

## Superseded: the anisotropy hunt

Everything from here to the end of this section was measured by comparing a
number taken from the data with a number taken from the model in closed form.
At widths of two pixels that comparison is dominated by the grid, and its
conclusions did not survive rendering the model onto the pixels instead: the
spots are consistent with an isotropic Gaussian once model and data are reduced
the same way.

It is kept because the reasoning is sound given what was measured, and because
three separate hypotheses were tested and rejected on evidence that turned out
to be an artefact of the method. That is worth being able to recognise again.

### The anisotropy, remeasured

With the pixel-to-millimetre conventions matched, over 61047 spots with more
than fifty counts:

    in the reflection frame   tangential 0.017363   radial 0.021149   ratio 1.237
    in the laboratory         along axis 0.018159   across   0.019496  ratio 1.051

**It survives the fix.** The earlier figure was about 1.5 from the aggregate
profile and 1.20 from the per-spot moments; it is now 1.24. The half-pixel
convention error was not the cause.

**It is not the sensor.** The ratio is flat with obliquity -- 1.248, 1.249,
1.236, 1.226, 1.216, 1.253 from 2 to 31 degrees -- and an absorption-depth
smear must grow with obliquity. This is the third measurement to say so, and
the cleanest.

**It is not the beam either.** A synchrotron beam is routinely wider in one
direction than the other, which would be fixed in the laboratory; resolved
along and across the rotation axis the ratio is 1.05, against 1.24 in the
reflection's own frame. Whatever it is prefers the radial direction, not a
laboratory one. Worth saying because the cubic crystal argues for isotropic
mosaicity, which is `sigma_M`, and says nothing about `sigma_D`, which is the
beam.

**Nor is it the crystal.** Diffraction from the front and the back of the
crystal starts from points separated along the beam, and at a scattering angle
those rays land `extent * tan(2 theta)` apart -- a hundred micrometres at thirty
degrees is fifty-eight, purely radial. The difficulty is that seen from the
crystal this goes as `sin(2 theta) cos(2 theta) / distance`, which is exactly
the sensor's dependence, so the two cannot be separated by this data; only
their sum can be tested, and only if the excess follows that shape.

It does not. Fitting a single source extent to `w2^2 - w1^2` gives an answer
that is not single:

    two-theta     excess      implied extent (T = sigma sqrt 12)
      2 - 11    8.27e-05        593 um
     11 - 15    1.51e-04        577 um
     15 - 17    1.56e-04        492 um
     17 - 19    1.64e-04        454 um
     19 - 20    1.54e-04        406 um
     20 - 22    1.50e-04        375 um
     22 - 24    1.47e-04        348 um
     24 - 31    1.59e-04        337 um

The excess is nearly flat while `sin(2 theta) cos(2 theta)` rises sixfold, so
the implied extent falls from 593 to 337 micrometres across the range. A real
source extent is a property of the crystal and cannot do that.

The magnitudes are worth having anyway. At the widest angle a 100 micrometre
crystal would contribute 1.4e-05 square degrees, the sensor alone predicts
2.5e-04, and 1.6e-04 is measured. So the crystal term is about a tenth of what
is seen and the sensor term alone already overshoots it.

**And it is not simply radial.** Split by azimuth around the beam centre the
radial ratio runs from 1.01 to 1.42, which a purely radial effect cannot do.
That may be confounded with radius, since the azimuth bins cover different
parts of the face, and it is not resolved.

The caveat from before still holds and still matters: the widths are 0.69 and
0.84 pixels. A second moment below the sampling interval is not a reliable
shape, and none of these numbers should be modelled until they can be
reproduced on data where a spot is several pixels across.

### Is the model any good? Ask the data, not DIALS

The Gaussian is supposed to hold essentially all of a spot's density by three
sigma. So measure it: over the flagged shoeboxes, what fraction of the counts
lies within n sigma, for n from one to four, in the detector directions and the
rotation direction separately.

    mxi_profile --compare --compare-sigma-b 0.031698 --compare-sigma-m 0.097667 \
                refined.expt refined.refl

On 1800 images of insulin, 70425 spots. A one-dimensional Gaussian holds
0.6827, 0.9545, 0.9973; two independent directions hold the square of that,
0.466, 0.911, 0.995.

                          1       2       3       4
    ours     detector  0.7578  0.9768  0.9990  1.0000
    ours     rotation  0.8653  0.9848  0.9988  0.9999
    DIALS    detector  0.7726  0.9801  0.9993  1.0000
    DIALS    rotation  0.8017  0.9668  0.9952  0.9995

**Both models hold by three sigma**, which is the first thing the test was for
and both pass: 0.999 against an expected 0.995 on the detector, 0.999 and 0.995
in rotation.

**The rotation direction says DIALS is right and we are not.** A correct sigma
would hold 0.6827 at one sigma. DIALS holds 0.8017 and this holds 0.8653 -- both
too concentrated, meaning both sigmas are larger than a Gaussian fitted to the
core, and ours is the further out. That is the same +21 per cent seen against
DIALS' number, now confirmed against the data rather than against DIALS.

**The detector direction says ours is marginally better**, 0.7578 against
0.7726 where 0.466 is expected, which matches being 2.9 per cent smaller.

Two things this does not say. The expected fractions assume the spot really is
Gaussian, and 0.76 where 0.47 is expected is far too large to be a small
sigma error: the real profile is much more peaked than a Gaussian, which is
why profile fitting uses learned reference profiles rather than the analytic
form. And the shoeboxes are the spot finder's, cut at its threshold, so the
density outside the box is not in the denominator at all and every fraction
here is an overestimate. Neither affects the comparison between the two sets of
sigmas, which is over identical pixels.

### Looking at the spots

    mxi_grid  refined.expt refined.refl --out grids.txt --n 5 --neighbours 300
    python3   docs/plot_grid.py grids.txt kabsch.png

Each strong spot's density on a grid in `(eps1, eps2, eps3)`, beside the
average of the spots nearest it on the detector, beside the average of all of
them. A single spot is a handful of pixels across and its grid is coarse and
noisy; the reference is what says whether a feature belongs to the spot or to
the sampling.

Two departures from Kabsch section 3.3, both because this measures the data
rather than applies the model. A pixel is subdivided in the detector plane and
its counts shared between the subdivisions, five ways per axis as Kabsch does,
because the data are badly undersampled and assigning a whole pixel to one grid
point turns the result into a staircase -- `--subdivisions 1` shows that. And
along `e3` an image's counts are shared between grid planes by the plain
geometric overlap of its angular range with theirs, NOT by the Gaussian weights
of Kabsch's `f_3j`: placing the counts with the model would beg the question
the picture is asked to answer.

The picture as a number. The average profile falls to a tenth of its peak at

    eps1   1.20 sigma_b        a Gaussian falls to a tenth at 2.146 sigma
    eps2   1.80 sigma_b
    eps3   1.20 sigma_m

So the spots are far narrower than the model that is supposed to describe
them, in every direction, and `eps3` worst -- which is the same conclusion the
captured-fraction test reached and the same one the comparison with DIALS
reached, now visible.

`eps1` and `eps2` differ from each other by half again, which a single
`sigma_D` cannot express: the model is isotropic on the detector face and the
spots are not.

### Is the anisotropy the sensor? Apparently not, and everything here is a pixel wide

    mxi_grid refined.expt refined.refl --out grids.txt --map map.txt
    python3 docs/plot_anisotropy.py map.txt anisotropy.png 0.0253

`e2` lies in the scattering plane, radially on the detector, and `e1` across
it. A photon absorbed at a random depth in the sensor is recorded further out
than where its ray entered, so that smear is radial: it belongs to `eps2`
alone. That makes it testable -- the excess `w2^2 - w1^2` should equal the
predicted smear squared and grow with obliquity as absorption says.

It does not. Over 61047 spots with more than fifty counts, binned by obliquity:

    obliquity     w2^2 - w1^2     predicted^2     ratio
     2 - 11 deg     8.3e-05         4.2e-05        1.98
    14 - 17         1.6e-04         1.1e-04        1.36
    19 - 20         1.5e-04         1.7e-04        0.93
    24 - 31         1.6e-04         2.5e-04        0.64

The measured excess is flat at about 1.5e-4 while the prediction rises eight
fold. At low obliquity there is twice as much anisotropy as the sensor can
account for, and at high obliquity two thirds as much. **Whatever it is, it is
not the depth of absorption**: that effect is real and is in there, but it has
the wrong shape.

The map of `w2/w1` across the detector face says the same thing more plainly:
it is not radial. A sensor effect on a flat detector must be, since it depends
only on the angle at which the ray strikes. This has large smooth patches that
do not centre on the beam.

**And the measurement sits at the sampling limit.** One pixel subtends 0.0253
degrees at 170 mm. The measured second moments are

    width1, tangential   median 0.69 pixels
    width2, radial       median 0.84 pixels
    sigma_b                     1.22 pixels
    a single lit pixel          0.29 pixels

So a spot is about two pixels across and its second moment is within a factor
of three of what a single lit pixel would give on its own. At that scale a
second moment is not a shape: it depends on where the spot centre falls within
a pixel, and the shoebox mask truncates it. An apparent anisotropy correlated
with position could be produced by that alone, and the non-radial map is as
consistent with a sampling artefact as with anything physical.

That caveat applies backwards as well. The earlier statement that `eps1` and
`eps2` differ by half again, and that the profile falls to a tenth of its peak
at 1.2 sigma, are both measurements made below the pixel scale. They are
evidence that the model is too wide, which is corroborated independently by the
captured fractions and by DIALS' own number; they are NOT evidence about the
shape of the underlying spot, which this data cannot resolve.

Two things worth doing before modelling any of it: repeat this on data with
finer sampling, where the spot is several pixels across; and test the sampling
hypothesis directly by measuring the width against the sub-pixel position of
the spot centre, which should show nothing if the widths are real.

## Seeing the model on the images

    mxi_mask refined.expt refined.refl -o masked.refl \
             --d-min 1.6 --first-image 0 --last-image 20
    dials.image_viewer refined.expt masked.refl

Predicts the reflections, works out each one's integration region, and writes a
shoebox whose mask marks it: `Valid | Foreground` inside the n-sigma region,
`Valid | Background` around it. **The pixel values are left at zero.** Nothing
here has read an image, and invented counts would be worse than none -- the
point is to see where the model says the signal is, drawn over the real image
by the viewer.

The bounding box comes from inverting the region rather than guessing a size:
the four corners of the `(eps1, eps2)` square are offset from `s1` in the
frame's own tangent plane and intersected with the panel, and the rotation
half-width is `n sigma_M / |zeta|` turned into images. It is then clipped to
the panel and the scan, so a box hanging off an edge is kept as the part of it
that is real.

An image range is not optional in practice. A whole 1800-image sweep at 1.6
Angstrom is 129000 reflections and about a gigabyte of empty shoebox; the first
attempt at this was killed by the machine. The boxes are also built and encoded
one at a time rather than all held at once.

### The region is a box, and the model is an ellipsoid

`|eps1| <= n sigma_D` and `|eps2| <= n sigma_D` and `|eps3| <= n sigma_M` is
what Kabsch section 3.1 writes for the mask, and what DIALS uses, so it is the
default. But it is a BOX in Kabsch space, and the model is a three-dimensional
Gaussian, whose surface of constant density is the ELLIPSOID

    (eps1/sigma_D)^2 + (eps2/sigma_D)^2 + (eps3/sigma_M)^2 <= n^2

These are not the same set and the difference is not small. At a corner of the
box all three coordinates are at n sigma at once, so the Gaussian is at
exp(-3n^2/2) there: 1.4e-6 of its peak at n = 3. There are eight such corners
and they hold nothing at all.

    volume          ellipsoid is pi/6 of the box, 0.5236
    density held    0.9919 in the box, 0.9707 in the ellipsoid

So the box buys two per cent more of the density with ninety per cent more
volume, all of it in places the model says are empty. `--shape ellipsoid`
draws the other one; on insulin it marks 23.9 per cent of the voxels where the
box marks 43.7, a ratio of 0.546 against the 0.5236 expected, the difference
being that a voxel is in or out as a whole and the boxes are only a dozen
pixels across.

Mapped onto the image the ellipsoid is not an ellipse: the transform from
Kabsch space to pixels and images is not a similarity, and it varies across the
detector with obliquity and with zeta. That distortion is the thing worth
looking at, and it is what the box hides.

What it shows on insulin, twenty images at 1.6 Angstrom: 1445 boxes, typically
12 by 12 pixels by 8 images. Those are big boxes for spots that are two pixels
across, which is the same conclusion as everything else in this section, in the
form most likely to be believed.

## Order of work, revised

Steps 1 to 3 of the original plan are done and what they found changed the
rest. What follows is written knowing that.

### What the profile work settled

**The Gaussian model is nearly right, and every measurement that said otherwise
was measuring the pixel grid.** Spots here are two pixels across. Comparing
their moments with a Gaussian in closed form said the model was wrong by
factors and that the spots were anisotropic; integrating the same model over
the same pixels, inside the same mask, put the positions within a hundredth of
a pixel and the widths within four per cent, and predicted the anisotropy from
projection and absorption alone.

That is the thing to carry into integration, because integration is nothing but
comparing a model with pixels. **Anything compared with the data has to be
rendered into the data's own terms first.**

**`sigma_D` and `sigma_M` do not match DIALS** and `mxi_profile` says by how
much every time it runs. The forward model says the truth is a few per cent
below both, so neither is exactly right and the difference between them matters
less than it looked. Integration takes the model as a parameter rather than
inheriting a number.

**The region is a box in Kabsch space and the model is an ellipsoid.** The box
is what Kabsch section 3.1 writes and what DIALS uses, so it is the default,
but its eight corners hold 1.4e-6 of the peak and the ellipsoid is pi/6 of its
volume. Once background is summed from that volume the difference stops being
cosmetic.

**The pixels were in the file all along** -- the `Shoebox<>` column -- and
`src/shoebox.hh` reads them.

### Checked against a DIALS integrated.refl

`integrated.refl` has no shoeboxes, DIALS discards them, but it has `bbox`,
`num_pixels.foreground`, `num_pixels.background` and `num_pixels.valid`, which
is enough to check the masking with no pixels at all.

**Predictions agree exactly.** Over 4018 reflections on the first sixty images:
100 per cent within half an image in `z`, 100 per cent on the `entering` flag,
and the box `z` extents agree 99.3 per cent of the time.

That only worked once it was compared against the right file. Predicting from
`refined.expt` and joining to `integrated.refl` matched almost nothing, because
something between them reindexes: `real_space_a` is (20.46, -14.89, 62.45) in
one and (-20.50, 14.81, -62.46) in the other, so the Miller indices are not
comparable across that step. It looked like a prediction bug and was a join
bug. **Pin the geometry to the file being compared against.**

**The box was wrong, and wrong in a way that blocks integration.** DIALS':

    num_pixels: foreground 462, background 3094, valid 3542
    foreground + background = valid   for 100 per cent of rows
    valid / box volume = 1.000

It is a measurement box in Leslie's sense: the foreground region plus a rim,
the rim being 87 per cent of the volume. `mxi_mask` built the foreground alone,
which has no background in it, and the background estimate has to come from
somewhere. `box_scale` widens the box on the detector; at 1.9 the extents match
DIALS. The rotation direction is untouched, since those extents already agreed.

**One thing still disagrees.** Our foreground is 372 voxels where DIALS'
is 462. The `z` extents match, so it is on the detector, and 1.11 per axis is the
wrong size to be rounding. Recorded rather than chased, with the oracle that
will settle it.

### Real images, and what the background looks like

A 30 degree sweep of insulin on a 4M detector, with its own `refined.*` and
`integrated.*`: 300 frames of 2162 by 2068, bitshuffle compressed, 297 MB.
`h5py` with `hdf5plugin` reads it here; one frame decompresses in 0.02 s.

**65535 is the bad-pixel marker**, not a count. It is 5.8 per cent of the frame
-- module gaps and dead pixels -- and reading it as data puts 17 billion counts
on a frame that has 1.65 million.

**The pixel addressing is `data[frame, slow, fast]`**, verified rather than
assumed: over the forty strongest reflections, the centroid of the extracted
box against `xyzcal.px` is

    fast   median -0.046  rms 0.146 px
    slow   median -0.145  rms 0.303 px
    frame  median -0.150  rms 1.111 images

Swapping fast and slow would have shown as a large offset in both.

**The background is 0.3 counts per pixel**, which is the regime Parkhurst's
paper is about: most pixels are 0 or 1, and a normal approximation to a Poisson
that small is not one. On 1482 reflections, taking the outer shell of each box
as background so the region is not chosen by value, against DIALS' robust GLM:

    DIALS (robust GLM)   0.3044 counts/pixel
    plain mean           0.2993    -1.3%
    5 per cent truncated 0.2334   -22.5%
    3 sigma clipped      0.2343   -14.2%
    median               0.0000  -100.0%

Which is Parkhurst's Table 1 and Figure 3 reproduced on this data: every
traditional outlier rejection biases the background DOWN, the median collapses
to zero because most pixels are zero, and the GLM sits beside the unrejected
mean. Since the intensity is the foreground minus the background, a background
biased low is an intensity biased high, for every reflection in the dataset.

A first attempt at this measurement used "the lowest n pixels" as the
background region and produced a 48 per cent bias for the plain mean. That was
the sampling, not the estimator: selecting the low tail guarantees a low
answer, and the median coming out at exactly zero should have been the giveaway
before the numbers were read.

### Background: implemented and checked against DIALS

`src/background.hh`, Parkhurst's robust GLM with a Poisson link and Huber
weights at c = 1.345. Against DIALS' own `background.mean` on 2000 reflections
of the real 30 degree sweep:

    DIALS       median 0.2997 counts/pixel
    ours        median 0.2963
    difference  median -1.35 per cent, rms 3.8, all within 20
    correlation 0.9972, converged 100 per cent, median 4 iterations

**The remaining difference is the region, not the estimator**, which was
checked rather than assumed. Widening the background region towards the number
of pixels DIALS uses moves it monotonically towards zero:

    pixels used   1120    1596    2016    2380     (DIALS 2936)
    difference   -1.46%  -1.40%  -0.89%  -0.54%
    correlation  0.9946  0.9973  0.9986  0.9991

So the estimator agrees and what is left is that the background region here is
a shell of the box rather than everything outside the foreground mask. That is
`mxi_mask`'s job and is the next thing to connect.

**The expectations are summed, not solved.** Appendix B gives closed forms for
C1 and C2 in regularized gamma functions, to avoid summing over the
distribution. At 0.3 counts a pixel the distribution is over by twenty, so the
sum is a dozen terms and is exact; the closed forms are an optimisation for a
regime this is not in, and they are the part of the paper whose exponents did
not survive the PDF's text layer. Transcribing them from a guess would have put
an error where no test could find it.

The tests check the reason rather than the answer: with the tuning constant
large the estimator must be the plain mean and the corrections must vanish to
their known values, C1 to zero and C2 to sqrt(mu); C1 and C2 are compared with
an independent summation using factorials rather than the production
recurrence; the fit is unbiased on clean Poisson data at four background
levels; one outlier in five hundred moves the mean by a factor of five and the
GLM by less than a tenth; and an all-zero background is answered rather than
iterated towards, since there is no log of zero.

### Summation integration, on real images

    mxi_integrate integrated.expt master.nxs -o mine.refl \
                  --sigma-b B --sigma-m M --d-min 1.8 --save-shoeboxes

Predicts, builds each measurement box, fills it from the images through the
spot finder's NXmx reader, fits the background and sums the foreground. The
output is a DIALS reflection table, so `intensity.sum.value` compares directly.
On 2504 reflections of the real 30 degree sweep:

    background.mean      ours 0.3042   DIALS 0.3019   correlation 0.9991
    intensity.sum.value  ours  155.0   DIALS  160.4   correlation 0.8753
    intensity.sum.var    ours  310.1   DIALS  348.6   correlation 0.8730

    on the 1112 with I/sigma > 10 in DIALS:
      ratio ours/DIALS   median 0.9817, 10th 0.9410, 90th 1.0038

**The background is right and the intensity is two per cent low**, which is
what the foreground mask being small predicts: 372 voxels against DIALS' 462,
measured before any of this touched a pixel. A foreground that misses the
skirts of a spot misses signal, and a two per cent deficit on strong
reflections is the size of that difference. So the mask discrepancy is no
longer a curiosity to be chased when convenient -- it is the outstanding error.

**The profile model is estimated rather than given.** In order of preference:
the command line, then this package's own estimate from a `.refl` of strong
spots with shoeboxes, then the `profile` block of the `.expt` if it carries one
-- which is now read. Carrying two numbers by hand between two programs is a
way to run the second on the wrong ones.

Which exposes something worth not mistaking for progress:

    DIALS' sigmas   sigma_b 0.028676  sigma_m 0.092108   ratio to DIALS 0.9817
    our estimates   sigma_b 0.027315  sigma_m 0.128501   ratio to DIALS 0.9943

Our own estimates give intensities closer to DIALS' than DIALS' own numbers do.
That is compensation, not correctness: our `sigma_m` is forty per cent larger,
which widens the foreground in the rotation direction and offsets the mask
being too small on the detector. Two errors of opposite sign look like
agreement, and the only reason this is visible at all is that both were
measured separately first.

**The images come from the `.expt`.** Its imageset block already says where
they are; being told the path a second time on the command line is how the two
come to disagree. `--images` overrides it for data that have moved since the
file was written, and the program prints which it used.

That found a real bug in the NXmx reader, reproduced before it was fixed. When
the virtual dataset maps back into the master file itself -- HDF5 writes `"."`
for that -- the reader set the path to the master and then resolved it against
the master's own directory a second time. A master named `../ins10_1.nxs`
became `../../ins10_1.nxs` and failed to open.

The first fixture written for it did not reproduce the failure: with the master
at `../sub/self.h5` the doubled resolution gives `../sub/../sub/self.h5`, which
is the same file. It only escapes when the master is one level up, which is
exactly the case reported. A fixture that does not fail on the old code is not
a test of anything.

**Every prediction that gets no shoebox is counted, by reason.** On a ten
rotation sweep 749786 predictions became 367050 boxes and there was no way to
tell whether that was the detector, the rotation axis, or a bug. The reasons
are now printed:

    21521 reflections predicted
    21032 shoeboxes to fill
      471 a corner misses the detector
      18 zeta below the cut

The reasons are: no such panel, no Kabsch frame, zeta below the cut, a corner
of the region missing the detector plane, a box spanning more images than any
reflection should, and clipped away entirely by the panel or the scan. Each is
reachable and each has a test, because a reason that is never produced is a
string nobody has checked.

**A multi-turn sweep was predicted once, not once per turn.** The reason
counters found it on their first outing: 749786 predictions of a ten rotation
sweep, of which 378278 had a `z` off the end of the scan, against
DIALS' 7516507. The ratio is 10.02, which is the number of turns.

Two faults, one line apart:

    if (span < 2.0 * kPi) { phi = wrap_from(phi, lo); ... }

A scan of a full turn or more skipped the wrap entirely, on the grounds that
everything is inside it. Everything is -- but only after wrapping, and the
caller needs phi in the scan's own coordinates or `z_from_phi` puts it in the
wrong turn. That is the 378278.

And a reciprocal lattice point that crosses the Ewald sphere at `phi` crosses
it again at `phi + 2 pi`, so ten rotations record every reflection ten times
over -- which is the entire reason for collecting them. Each turn's root is
converged from its own seed rather than taken as `phi + 2 pi k`, because with a
scan-varying crystal the setting matrix at turn nine is not the one at turn
zero.

The patch went into `predict_indices` first and changed nothing, because
`predict` has its own copy of the loop and is the one that runs. There is now
one emitter that both call.

**It walks the frames once, with shoeboxes open across them.** Every shoebox
cannot exist at once: ten rotations of insulin is 7.4 million reflections and,
at about 3500 voxels each, 233 GB of pixels.

Blocks of images were the first attempt and were wrong. A reflection near the
rotation axis spans `n_sigma sigma_m / |zeta|` in phi, which at the zeta cut is
about a hundred and fifty frames; one of those in a block forces the whole
block to read that far, and the next block reads it again. On three hundred
frames it read 2951 of them at a block of ten, 817 at fifty -- which is how
this was noticed, a three hundred frame sweep reporting 817 frames read.

So the frames are walked once in order. A shoebox opens when the frame it
starts on comes round, takes a slice from every frame it spans, and is
integrated and released on the frame it ends. Every frame is read exactly once,
and memory is bounded by what is open at the time -- which is what the block
size was trying to bound anyway, and bounds it by the data rather than by a
guess.

    frames read     300 of 300
    peak memory      86 MB
    open at once    931 shoeboxes

with the Miller indices, intensities, variances and backgrounds identical to
the block version.

The first pass is unchanged: work out every bounding box and keep nothing but
the box, 24 bytes a reflection rather than 31 kB.

**Where the time goes**, from `--timing`, on the thirty degree sweep:

    the profile model             0.171 s    4.4%
    prediction                    0.251 s    6.4%
    bounding boxes                0.010 s    0.2%
    opening shoeboxes             0.876 s   22.5%
    fetching frames               0.281 s    7.2%
    decompressing                 0.835 s   21.5%
    filling shoeboxes             0.312 s    8.0%
    background and summation      1.049 s   26.9%
    writing                       0.038 s    1.0%
    total                         3.894 s

It was 21 seconds when the timing went in, and finding out where meant adding
the phases twice: the first breakdown accounted for 15 per cent of the run and
the second for 35, and each gap was somewhere the clock had not been put. A
breakdown that does not add up is not a breakdown.

Two things it found, neither of which was the images.

**Every chunk was fetched twice.** A map from frame number to key was built by
reading every key to ask what frame it was, and then the loop read them all
again -- 18 of the 21 seconds. The loop is now driven by the frames as they
arrive, and checks they arrive in order rather than assuming it.

**Building the masks was 63 per cent of what was left.** `build_shoebox`
evaluated the Kabsch mapping once per voxel, and `eps1` and `eps2` depend only
on where a pixel is on the detector while `eps3` depends only on which image it
is. So the mapping was being run `nz` times for every pixel, and it has a
parallax correction with an `exp()` in it. Computing the face once and the
images once is `nx*ny + nz` evaluations instead of `nx*ny*nz`: 5.344 s to
0.876, with every intensity, variance, background and foreground count
identical.

    21.0 s -> 8.5 s -> 3.9 s

### Threading: the reads were the wrong thing to thread

On ten rotations, 310 seconds:

    decompressing               158.107 s   51.0%
    background and summation     53.300 s   17.2%
    opening shoeboxes            46.137 s   14.9%
    prediction                   25.739 s    8.3%
    filling shoeboxes            11.755 s    3.8%

Threading only the fetch and decompress settles at about 116 per cent of one
core on a sixteen core machine, and the arithmetic says why: 46 + 12 + 53 is
111 seconds of work on the thread that owns the shoeboxes, so the readers
finish their lookahead and wait. No number of reader threads moves that.

**The ordering that forced the pipeline was not needed.** A frame writes only
its own z plane of a shoebox, so two frames of the same box can be filled by
two threads without touching the same double. Nothing needs ordering; the boxes
need only to exist.

So the scan is cut into windows. Within one: the masks are built in parallel,
the frames are fetched, decompressed and filled in parallel, and the boxes are
integrated in parallel. Only the window boundary is serial.

**The window must be long compared with a shoebox**, which the first two
attempts were not. Bounded at a thousand frames it swallowed a three hundred
frame sweep whole -- 21032 boxes, 650 MB, and the allocation cost more than the
parallelism saved. Bounded at four thousand boxes instead it became six windows
of fifty frames and read 748 frames instead of 321, with decompression going
from 1.13 seconds to 2.66: the overlapping-block problem again, in a new place.
Frames are the bound and boxes are a safety net, at twenty thousand.

**What this costs and what it cannot show.** This container has one core, where
the window version is 5.2 seconds against the streaming version's 4.5 and peaks
at 843 MB against 86. Both are real and neither is offset here; the case for it
is that three phases now parallelise instead of one, and that is an argument
rather than a measurement. The results are identical at every thread count, and
the numbers to watch elsewhere are the total, the CPU share, and whether frames
read stays near the frame count -- if it climbs, the window is too short.

`--threads`, `--window` and `--max-boxes` are the knobs.

### Every turn collapsed into the first, and a diagnostic found it

    27366 frames read (3627 wanted by a shoebox, each read 7.55 times)

3627 frames of a 36000 image sweep is one rotation. Every shoebox of all ten
turns was landing in the first turn's frames, so the same pixels were
integrated ten times over and every intensity of that run was wrong.

Two faults, and the second hid behind the first.

`wrap_from` returns a value in `[lo, lo + 2 pi)`, and `build` called it
unconditionally -- so a phi correctly placed in turn nine was wrapped straight
back to turn zero. It now wraps only a phi that is OUTSIDE the scan; one
already inside it is already in the turn its caller meant.

And `converge_root` answers with a crossing from `ewald_intersections`, which
is in a principal 2 pi interval whatever it was seeded with, so the turn was
lost on the way out too. `emit_turns` now puts it back, taking the turn nearest
the seed.

**The test that should have caught this used a scan-static crystal**, where the
turn was restored by hand with `c.phi = seed` -- so it passed, while every real
experiment, all of which have scan-varying crystals, was broken. There is now a
test on a scan-varying crystal with the same `A` at every scan point, so the
geometry is identical to the static case and only the code path differs; it
fails on the old predictor and asserts that every turn holds its own share of
the predictions rather than only that the count is right. A count can be right
while every one of them is in the wrong place.

The diagnostic that found it exists because `27366 frames read` was not
interpretable on its own, which was itself a complaint about the output rather
than a hypothesis about the code.

### The re-read rate, and where it comes from

With the turns no longer collapsed, ten rotations wants all 36000 frames and
reads 59134 of them, 1.64 times each. That is arithmetic rather than a puzzle:

    7309317 boxes / 20000 per window = 365 windows
      own span       99 frames
      actually read 162 frames
      overlap        63 frames past each window end

and a shoebox at the zeta cut of 0.05 spans 82 images, because its extent in
phi is `n_sigma sigma_m / |zeta|`. So the overlap IS the near-axis reflections,
and the lever is window length against memory:

    --max-boxes  20000:   99 frames per window, re-read 1.65 x, 0.6 GB
    --max-boxes  40000:  197 frames per window, re-read 1.32 x, 1.3 GB
    --max-boxes  60000:  296 frames per window, re-read 1.22 x, 1.9 GB
    --max-boxes 120000:  591 frames per window, re-read 1.11 x, 3.8 GB

Raising `--min-zeta` shortens the long boxes instead and costs reflections.

### The timing report was summing to 124 per cent

Fetching 77.1 per cent and decompressing 47.4 per cent of the same run: both
were the busiest thread's wall clock inside one parallel region, so they
overlap and the percentages are not shares of anything. The region now reports
its own wall clock as the phase, with the work inside it in thread-seconds and
a ratio against that wall -- which are additive, comparable, and show the
difference between working and waiting.

### What that left, on sixteen cores

    total                        65.403 s   from 310
    user                        370       s   so 5.7 cores busy

    prediction                   25.982 s   39.7%
    decompressing                22.716 s   34.7%
    writing                       8.203 s   12.5%
    background and summation      5.461 s    8.4%
    opening shoeboxes             4.792 s    7.3%
    the profile model             4.541 s    6.9%
    filling shoeboxes             1.825 s    2.8%

Prediction became the largest item by being the only phase still serial, so it
is threaded too: one unit of work per `h`, each collecting into its own vector,
joined in `h` order. The order matters as much as the content -- everything
downstream is indexed by position in that list, so a prediction list that
reordered itself with the thread count would make every comparison between two
runs meaningless. Checked with a checksum that depends on position: identical
at one thread and four.

The report also says how many frames a shoebox wanted and how many times each
was read, because `27366 frames read` of a 36000 image sweep is not
interpretable on its own -- it could be windows re-reading, or frames nothing
needed, or reads that failed. Reads that find no frame are counted separately:
an unallocated chunk is a frame the writer never received, and a shoebox
spanning one is missing a slice and will integrate low.

Three things the pixels settled that no synthetic test would have:Three things the pixels settled that no synthetic test would have:

* **The bad-pixel marker is excluded from both sums, not counted as zero.** It
  is 5.8 per cent of a frame, module gaps and dead pixels, and 470000 voxels of
  the boxes here. Counting them as zero would drag the background down wherever
  a gap crosses a shoebox.
* **`d` is `1 / |A h|` from the STATIC cell.** Three nearly equal numbers were
  candidates: the static cell agrees with the column to 1.2e-15 for every
  reflection, while `1/|s1 - s0|` and the scan-varying `A` are both 2.6e-4 out.
  A tolerance of a part in a thousand would have accepted any of them.
* **`partiality` is the fraction of the rocking curve inside the BOX**, not
  inside the scan. What was summed is what is in the box, and a box spanning
  three sigma holds 0.9973 of the curve rather than all of it. Against DIALS:
  0.99887 against 0.99914, with ninety per cent of differences below 0.0014,
  and 208 reflections that both call genuinely partial. Computing it against
  the scan instead gives 1.0000 for everything away from the ends, which is the
  kind of small error a scale factor multiplies through a whole dataset.
* **`zeta`, `partial_id`, `num_pixels.background_used` and
  `xyzobs.mm.variance`** are written too. What is still missing is the three
  profile-fitting columns, which is the next step rather than an oversight.
* **`xyzobs.px.value` is the centre of mass of the foreground**, background
  subtracted, which `dials.scale` requires. Over the whole box instead it is
  0.15 to 0.19 pixels from DIALS'; over the foreground it is 0.07 in fast and
  0.10 in slow, and `xyzobs - xyzcal` comes out at [-0.001, -0.007, -0.076]
  against DIALS' [-0.002, -0.002, -0.076] -- including that frame offset, which
  is the part that would show a convention error.

  `xyzobs.px.variance` does **not** reproduce DIALS'. The second moment alone is
  about twenty times too large and the variance of the mean about thirty times
  too small, and neither is a constant factor away, so DIALS is computing
  something else; its values sit near 0.1 square pixels on every axis, which
  looks like a quantisation term rather than anything that scales with
  intensity. What is written is the variance of the mean, which is the standard
  quantity, and this is recorded rather than tuned to match.
* **`lp` is `L / P`**, with `L = |s1 . (m2 x s0)| / (|s1||s0|)` and
  `P = (1-p) + (2p-1)(u.n)^2 + p(u.s0hat)^2`. Recovered from the oracle rather
  than recalled: the Lorentz part was identifiable as the candidate with the
  least scatter against the column, and the three coefficients of P were then
  solved for by least squares and came back as 0.001000, 0.998000 and 0.999000
  with a residual of 2e-16 -- which is `(1-p)`, `(2p-1)` and `p` for the
  `p = 0.999` in the file, not three free numbers. At `p = 0.5` it collapses to
  the unpolarized `(1 + cos^2 2theta)/2`, which is the check that the form is
  physics; there is a test for that limit. Against the column it agrees to
  4e-5, the residual being our predicted `s1` rather than the formula.
* **`qe` is `1 - exp(-mu t / cos theta)`**, the fraction of photons the sensor
  stops rather than passes. Checked against the `qe` column: identical for 100
  per cent of reflections to 2e-16. It is stored rather than applied, as DIALS
  does, so the scaler applies it later and the two tables stay comparable.
* **The flag bits were read from the oracle, not guessed**: 256 is
  integrated_sum and 512 integrated_prf, from flag value 769 splitting 21972
  and 20688 ways.

`--save-shoeboxes` keeps the pixels, the mask and the fitted background in the
output, at about a megabyte per hundred reflections. It is the difference
between "the intensity is wrong" and "the intensity is wrong because the mask
is here and the spot is there".

### Profile fitting: implemented, and not yet right

`src/reference.hh`, Kabsch sections 3.3 and 3.4 with Leslie section 6. Two
passes over the images, which is the slow way round and the right one: the
reference profiles are learned from the reflections themselves, and a profile
learned from part of a scan and applied to the rest would be a different
algorithm whose errors would be hard to attribute.

Both learning and fitting happen on a cube in the Kabsch frame. That is what
the frame is for: a reflection's shape is the same there wherever it sits on
the detector, so profiles from different reflections can be averaged, which on
the detector they cannot. A pixel maps to a region rather than a point, so each
is subdivided five ways an axis as Kabsch does, and one image covers a range of
`eps3` which is shared between grid planes in proportion to the overlap.

**What works.** Against a DIALS `integrated.refl` on 19360 profile-fitted
reflections:

    intensity.prf.value   ours 153.1   DIALS 159.7   ratio 0.9813 on I/sig > 10
    profile.correlation   ours 0.8141  DIALS 0.8709

and the unit tests hold both of Leslie's limits: fitting a profile to itself
returns the intensity exactly, and the fit reduces to the sum when the
background is negligible, which is section 6.4 and is a check on the weighting
rather than a remark.

**What does not.** The per-reflection correlation with DIALS is 0.4414, against
0.9453 for the summed intensities from the same run. So the scale is right and
the weighting is not. And the variance is too small: on weak reflections it is
52.7 against the summed 185.4, a factor of 3.5 where Leslie section 6.6
predicts about 2 for a typical profile.

Both point the same way. The grid is 729 points and a single pixel is spread
over several of them, by the subdivision on the detector and by the `eps3`
overlap across planes. Neighbouring grid points are therefore correlated, and
`1 / sum(P^2 / v)` treats them as independent observations -- which overcounts
the information, understates the variance, and mis-weights the fit. The pixels
are the independent measurements, not the grid points.

So the next step is to fit against the pixels with the profile transformed onto
them, rather than against the grid with the pixels transformed onto it -- or to
carry the covariance the transform induces. Recorded here rather than tuned
away, because a factor of 3.5 where the theory says 2 is the kind of
disagreement that a fudge factor would hide permanently.

The timing, on the thirty degree sweep at one thread: learning 7.8 s, fitting
20.1 s of 35.1 s total. It was 65 and 150 before the subdivided face was
computed once per box instead of once per image -- the same optimisation, and
the same mistake, as the mask.

### The steps

1. **Bounding boxes and masks.** Done, `mxi_mask`, and checked above.
2. **The Kabsch transform and the grid.** Done, `mxi_grid`.
3. **Background.** Parkhurst et al. (2016) is in hand: a robust GLM with a
   Poisson link, Huber weights at c = 1.345, and the constant-background case
   simplified in its Appendix B to a scalar iteration. The oracle is
   `background.mean` in a DIALS `integrated.refl`, which is present.
4. **Summation.** Leslie (1999) is in hand: the intensity and, more to the
   point, the variance, `G(Is + Ibg + (m/n) Ibg)`, which says the background
   dominates the error for weak reflections. The oracle is
   `intensity.sum.value` and `.variance`.
5. **Reference profiles and profile fitting.** Kabsch sections 3.3 and 3.4 and
   Leslie section 6. The profile work says why a learned profile is necessary
   rather than merely traditional: the real spot is more peaked than a
   Gaussian.
6. **Predicted shoeboxes need images.** Everything above is checkable on the
   strong spots' own pixels or against the oracle columns. Integration proper
   is not: predicted reflections are a superset of the strong ones.

### What this will cost, before it is written

Integration touches every pixel of every shoebox of every predicted reflection.
The performance work was practice for it and three lessons apply directly.

**Measure before optimising, and check the instrument first.** Two conclusions
here were drawn from timings that were wrong: a clock inside a lambda called
four hundred and fifty million times, and a benchmark passing the wrong kind of
memory to a device.

**Do not materialise what can be consumed.** Refinement builds a Jacobian of
parameters by reflections by three doubles, reallocated every iteration, and
threading its inner loop gave 1.29x on sixteen cores because the allocation is
serial. A shoebox does not have to exist as an array to be summed.

**The layout is expensive to change afterwards.** Decide whether a shoebox is
pixel-major or reflection-major, and whether the profile grid is dense or
sparse, before writing the loop that walks it.

## The precision contract, decided in advance

Settled per stage before any of it is written, not discovered afterwards:

* **Summation integration sums integer counts.** Done in integers, exactly, as
  the spot finder's window sums already are. There is no precision question and
  there should not be one.
* **The coordinate transform and the clipping** are geometry in double, and
  port to float with the same argument as the refinement target: measure the
  analytical path, do not assume it.
* **Background and profile fitting** are least-squares, and get a tolerance
  rather than a byte-for-byte test.

## How it will be compared

Both modes, from the start:

* **Pinned** -- this stage gets DIALS' upstream output. Isolates it. Without
  this, a hundredth of a degree of orientation difference shifts every shoebox
  and a correct integrator looks broken.
* **Cascade** -- this stage gets our own upstream output. The number that
  matters.

The join key at this boundary is (`miller_index`, `entering`, frame), and the
metric is the pull distribution, `dI / sqrt(sigma_a^2 + sigma_b^2)` -- its mean,
width and tails -- together with the correlation. Not percentage differences on
intensities.

## mxeq trend: where two integrations disagree, and against what

    mxeq trend ours.refl theirs.refl
    mxeq trend ours.refl theirs.refl --value intensity.prf.value --bins 5

A single correlation says two columns disagree. It does not say whether the
disagreement is with resolution, with intensity, with position on the detector,
with how near a reflection sits to the rotation axis, or with how well the
profile describes it -- and those point at different causes. This bins the
ratio against each in turn.

Bins hold equal populations rather than equal widths: equal widths on a
quantity like I/sigma put almost everything in the first bin and say nothing.

On the profile-fitted intensities it localised the problem straight away. The
ratio is flat -- 0.966 to 0.989 across every variable -- so the scale is right
everywhere. What moves is the agreement, and it moves the wrong way round:

    intensity.prf.value against I/sigma
              from         to        n       ours     theirs    ratio    corr
            -3.729      2.539     3846      11.97      12.32   0.9720  0.3718
             6.238      11.72     3845      150.5      155.7   0.9704  0.9008
             23.61      357.2     3846       1693       1783   0.9873  0.2288

    intensity.sum.value against I/sigma
            -3.729      2.465     3872      13.23      12.41   0.9593 -0.0099
             6.151      11.64     3872      154.6      158.6   0.9714  0.1163
             23.48      357.2     3872       1731       1735   0.9994  0.9751

Summation agrees with DIALS almost perfectly on the strongest reflections
(0.9751) and hardly at all within the weak bins, which is what noise looks
like and is unremarkable. Profile fitting does the opposite: it agrees best in
the middle and WORST on the strongest (0.2288), where there is the least noise
to blame.

So the fit misbehaves where the signal is largest, which is the opposite of
what a weighting error alone would do to weak data, and it is what drags the
overall correlation to 0.42. Strong reflections are where the profile's tails
carry the most counts and where a saturated or mismodelled pixel has the most
leverage. That is the next thing to look at, and this tool is how.

Pearson within a bin is not a measure of agreement, and reading it as one is
misleading. Two extra columns were added because of that:

* **`rho`, the rank correlation.** Pearson is dominated by the largest values,
  so a handful of gross outliers in a bin of five thousand drags it down while
  the other four thousand nine hundred agree perfectly. Spearman cannot be
  moved that way. Where the two disagree, the disagreement is the reading:
  Pearson low and Spearman high means a few disasters, both low means real
  scatter.
* **`spread` and `out`**, the median absolute deviation of the ratio and how
  many reflections lie beyond five of them -- or five per cent, whichever is
  wider. The floor matters: where the bulk agrees exactly the deviation is
  zero, and five times zero excludes nothing however wrong a value is, so a bin
  with twenty catastrophes in it reported none.

With those, summation on the thirty degree sweep reads cleanly:

        from         to       n      ours    theirs   ratio  spread    corr     rho   out
      -3.729      1.352    2420     6.055     4.655  0.9457  1.1276 -0.0125  0.5808   311
       3.267      5.619    2420     63.53     65.69  0.9642  0.1348  0.0381  0.8369    20
       8.588      12.57    2420     201.7     206.6  0.9741  0.0516  0.1443  0.9595     9
       19.19      33.55    2420     819.9     824.9  0.9905  0.0212  0.3066  0.9937     7
       33.55      357.2    2420      2854      2839  1.0013  0.0108  0.9753  0.9938    17

`rho` rises monotonically to 0.994 and `spread` falls monotonically to 0.011,
which is what agreement improving with signal actually looks like. `corr` does
not, and never did -- it was measuring how wide each bin is.

### What the outliers are: the near-axis reflections

    mxeq disagree ours.refl theirs.refl -o disagree.refl --limit 200
    dials.image_viewer imported.expt disagree.refl

A table of only the reflections two integrations disagree about, so they can be
looked at on the images. The rows are ours, because the question is what our
boxes did; the reference's value rides along in `reference.intensity` and the
ratio in `disagreement.ratio`, so the viewer shows both.

Three ways of asking what disagrees, because they find different things:

    mxeq disagree ours.refl theirs.refl --factor 2         1403 reflections
    mxeq disagree ours.refl theirs.refl --difference 200    277
    mxeq disagree ours.refl theirs.refl --sigma 5           236

`--factor` is relative and finds what is proportionally wrong, which is mostly
weak reflections, where a ratio means least. `--difference` is absolute, in
counts: a background biased by a fraction of a count costs every reflection the
same number of counts, and only this sees that as one thing rather than as a
trend. `--sigma` divides by the two variances added and is the only one of the
three that knows whether a disagreement is larger than the measurement.

Each criterion given must be exceeded, so passing two narrows. `--floor`, which
skips pairs where both values are near zero, applies only to `--factor`: the
other two are not confused by small numbers. It was called `--absolute`, which
meant the opposite of what it sounds like next to `--difference`.

The first guess was overlap -- a strong neighbour leaking into our foreground,
which Leslie sections 6.3 and 6.7.1 handle and nothing here does. **It was
wrong.** Only 6 per cent of the disagreeing reflections lie within twenty
pixels of a very strong one, against 0 per cent of a random sample: an
enrichment, but it accounts for a handful of 1403.

What they actually have in common, against the whole run:

    |zeta|              0.192  against  0.831
    z extent           41       against 10      images
    foreground pixels  2012     against 486
    our intensity     997.9     against DIALS'  2.032

They are the reflections near the rotation axis. Their rocking curve extent is
`n_sigma sigma_m / |zeta|`, so at small zeta the box is forty images deep and
its foreground is two thousand voxels of which only a few hold the reflection.
We sum all of them and report a thousand counts; DIALS reports two.

And that is the whole of the summation disagreement:

    |zeta| > 0.0 :  19360 reflections, sum corr 0.9390
    |zeta| > 0.2 :  19110 reflections, sum corr 0.9859
    |zeta| > 0.3 :  18720 reflections, sum corr 1.0000

`--min-zeta` is the lever, and it was recommended here before it existed on
`mxi_integrate` -- it was on `mxi_mask` and in `MaskOptions` and had never been
exposed by the program that needed it. On the thirty degree sweep, raising it
from the default:

    --min-zeta 0.05   15092 integrated, sum corr 0.9373, prf corr 0.9667
    --min-zeta 0.2    14825 integrated, sum corr 0.9855, prf corr 0.9932

267 reflections fewer, and both agreements improve. It costs time twice over as
well, since a window has to read every frame its longest reflection reaches,
which is what drives the re-read rate.

**Summation agrees with DIALS exactly once the near-axis reflections are set
aside.** Which leaves a real question rather than a bug: what should happen to
them. The zeta cut is currently 0.05 and lets in boxes forty images deep; DIALS
evidently measures them and gets nothing, which is the honest answer for a
reflection smeared over forty images. Raising `--min-zeta` would drop them, at
the cost of reflections near the axis that a longer sweep would measure
properly.

**Profile fitting was a different problem, and it was the variance floor.**

Its disagreement with DIALS was not spread through the data; it was 79 of 19771
reflections, 0.40 per cent, whose fitted intensity came back with the opposite
sign to their summed one. Their summed intensities have a median of 14845 --
the strongest reflections in the dataset, fitted large and negative. The worst
had a summed intensity of 128300 and a fitted one of -53610, with a
`profile.correlation` of 0.913, so the profile described it well and the scale
was still wrong. One extreme value of the wrong sign is what a Pearson
correlation is least able to survive.

The weights are Poisson, `1/v` with `v` the variance of the expected counts,
and the floor under `v` was an epsilon of 1e-6. A grid point whose expected
value is near zero then carries a weight of a million and the fit does what
those few points say. Such points are ordinary rather than pathological: the
background at a grid point is shared out in proportion to how much of a pixel
reached it, so a point at the edge of a spot's footprint has a background of a
ten-thousandth of a count.

Raising the floor to 0.05 counts -- the least variance a measurement of
anything is allowed to have:

    neither             79 sign flips, prf corr 0.3786
    clamp only          78 sign flips, prf corr 0.3782
    clamp and floor      0 sign flips, prf corr 0.9678

Profile fitting now agrees with DIALS better than summation does, which is what
it is for.

Three things about how that was established, none of them comfortable.

**The first comparison was not controlled.** The two runs used different
resolution limits, 1.8 and 2.0 Angstroms, and different numbers of reflections.
The improvement was real but the experiment did not show it, and the doubt was
only resolved by running both at the same limit.

**The cause was not what it looked like.** The change made two things at once,
a clamp on the scale and the floor, and the clamp was the one that looked like
the fix. Measured separately, it moves 79 flips to 78. The floor moves 78 to
zero. It stays because a negative expected count is not a Poisson mean, but it
is not why this works.

**There is no test for it.** Four synthetic cases were built to reproduce the
failure and all four pass with the old epsilon floor. What establishes the
floor is a controlled experiment on real data -- same file, same resolution,
one line changed -- and nothing in the suite would catch its removal. The test
that remains says so in its own comment rather than implying otherwise.


### The steps

1. **Bounding boxes and masks.** Done, `mxi_mask`, and checked above.
2. **The Kabsch transform and the grid.** Done, `mxi_grid`.
3. **Background.** Parkhurst et al. (2016) is in hand: a robust GLM with a
   Poisson link, Huber weights at c = 1.345, and the constant-background case
   simplified in its Appendix B to a scalar iteration. The oracle is
   `background.mean` in a DIALS `integrated.refl`, which is present.
4. **Summation.** Leslie (1999) is in hand: the intensity and, more to the
   point, the variance, `G(Is + Ibg + (m/n) Ibg)`, which says the background
   dominates the error for weak reflections. The oracle is
   `intensity.sum.value` and `.variance`.
5. **Reference profiles and profile fitting.** Kabsch sections 3.3 and 3.4 and
   Leslie section 6. The profile work says why a learned profile is necessary
   rather than merely traditional: the real spot is more peaked than a
   Gaussian.
6. **Predicted shoeboxes need images.** Everything above is checkable on the
   strong spots' own pixels or against the oracle columns. Integration proper
   is not: predicted reflections are a superset of the strong ones.

### What this will cost, before it is written

Integration touches every pixel of every shoebox of every predicted reflection.
The performance work was practice for it and three lessons apply directly.

**Measure before optimising, and check the instrument first.** Two conclusions
here were drawn from timings that were wrong: a clock inside a lambda called
four hundred and fifty million times, and a benchmark passing the wrong kind of
memory to a device.

**Do not materialise what can be consumed.** Refinement builds a Jacobian of
parameters by reflections by three doubles, reallocated every iteration, and
threading its inner loop gave 1.29x on sixteen cores because the allocation is
serial. A shoebox does not have to exist as an array to be summed.

**The layout is expensive to change afterwards.** Decide whether a shoebox is
pixel-major or reflection-major, and whether the profile grid is dense or
sparse, before writing the loop that walks it.

## Looking at the profiles

    mxi_integrate ... --save-profiles profiles.txt
    mxeq profiles profiles.txt -o profiles.png --widths widths.png

Plain text out of the integrator and pictures out of `mxeq`, because the thing
that reads it is a person with a question rather than a program with a format.

`mxeq profiles` draws three central SECTIONS of each region's profile rather
than projections: a projection hides a profile that is hollow, double-peaked or
displaced, which is exactly what is worth seeing. `--widths` plots the second
moment of each profile along each axis.

On the thirty degree sweep, nine regions:

* **The widths vary by a factor of 1.9 between regions**, narrowest at the
  detector centre (0.95 grid points in `e1`) and widest at the corners (1.81).
  That is obliquity and parallax, and it is the reason per-region profiles
  exist -- so the region division is earning its place.
* **Several profiles are visibly off-centre**, one region markedly so. A
  profile is built in the Kabsch frame about the PREDICTED position, so a
  displaced profile means the prediction and the observation disagree
  systematically in that part of the detector. Fitting a displaced profile to a
  reflection biases its intensity, and a profile displaced differently in
  different regions biases it differently across the detector.

Both are worth having pictures of, and neither is visible in a correlation.

### Two things the trends say, which are not the same thing

**Summation.** The ratio to DIALS climbs monotonically with intensity: 0.95 at
I around 25, 0.977 at 170, 1.0017 at 5800. That is the shape of a constant
ABSOLUTE offset rather than a scale error -- a background biased slightly high
costs every reflection the same number of counts, which is a large fraction of
a weak one and nothing to a strong one. Ours measured 0.3042 against DIALS'
0.3019 on the same reflections.

**Profile fitting.** The ratio drifted with FRAME, 0.986 at the start of a
1800 image scan to 0.951 at the end, because the profiles were learned per
detector region for the whole scan. The crystal is refined scan-varying for the
reason that it changes, and on a long sweep so does the sample; a profile
averaged over a scan fits the middle of it and neither end, which is that
drift's shape.

Fixed, by doing what XDS and MOSFLM do. The scan is divided as well as the
detector -- `--scan-blocks`, five by default -- and the profile used at a
reflection is a WEIGHTED AVERAGE of the nearby cells rather than the nearest
one, trilinear in fast, slow and frame, with weights falling linearly with
distance as Leslie section 6.1 describes. Taking the nearest makes the model
jump at a cell boundary, so two reflections a pixel apart either side of one
are fitted with different profiles.

Measured on the thirty degree sweep, where the drift was smaller to begin with:

    without scan blocks   0.9789 -> 0.9731 across the scan, monotonic
    with blocks and interpolation
                          0.9824 -> 0.9816, flat

The drift falls from 0.0058 to 0.0008 and the median ratio improves from 0.977
to 0.982. On a scan six times longer the drift was six times larger, so this
should show more there.

What it does NOT fix is the disagreement on the strongest reflections, which is
a separate problem and still open.

## mxeq html: the same comparison, drawn

    mxeq html ours.refl theirs.refl -o comparison.html
    mxeq html ours.refl theirs.refl -o comparison.html --value intensity.prf.value

The trends are numbers about numbers -- a ratio, a spread, two correlations and
an outlier count, for nine variables and five columns, which is several hundred
figures. What identifies a disagreement is its SHAPE: flat, trending, kinked at
one end. A table does not convey a shape.

Two panels per variable per column. The first is the median ratio with the
robust spread as a band, against agreement at one. The second is the rank
correlation, the Pearson correlation and the outlier count on one pair of axes,
because the three together say which kind of disagreement it is: rho high with
Pearson low and outliers present is a few disasters, both low is real scatter.
Above them, ours against theirs on log axes, sampled to four thousand points,
which shows whether a disagreement is a few reflections or all of them.

Plotly is loaded from its CDN and nothing in `mxeq` imports it: this writes
JSON into a page. So there is no new dependency, the report is one file that
can be sent to someone, and it needs a network connection once to draw.
`--plotly` points it somewhere else for a machine that has none.

**The axes are the ones the quantity is usually read on.** Resolution is drawn
against 1/d^2 with the ticks still reading in Angstroms, because shells of
equal 1/d^2 hold equal volumes of reciprocal space -- on a linear axis in d
every high-resolution shell is crushed into the left of the plot, which is
where most of the reflections are. Pixel counts and backgrounds span decades
and are drawn on a log axis for the same reason. Which axis a variable wants is
a property of the variable and is declared with it, so the text report and the
drawing cannot disagree about it.

A variable that does not vary gives one bin, and a panel with one point in it
takes up half a screen saying nothing, so those are skipped; the text report
still lists them, where a short table costs nothing.

## The background is one per cent high at low resolution, and why

Binned against resolution, our `background.mean` against DIALS':

    4.33 - 55.1 A   0.6564  0.6524  1.0047
    3.36 -  4.33    0.6349  0.6292  1.0097
    2.94 -  3.36    0.5009  0.4958  1.0083
    ...
    1.80 -  1.99    0.1740  0.1736  1.0013

An overestimate, not an underestimate, and it is worst where the background is
largest. An overestimated background makes the intensity too LOW, which is the
direction the summed intensities are wrong in.

**The cause is spot signal in the background region.** Widening the foreground
so that the skirts fall inside it moves the background ratio straight through
one:

      n-sigma   fg px   bg ratio   sum ratio (I>50)   sum corr
      3.0        450    1.0073     0.9920            0.9373
      3.5        720    0.9935     1.0196            0.9360
      4.0       1078    0.9873     1.0378            0.9352

    DIALS' foreground: 476 pixels

**But n_sigma is the wrong lever.** At 3.0 our foreground is 450 pixels against
DIALS' 476, which is close; by 3.5 it is 720, half as large again, and the
summed intensities have gone from 0.8 per cent low to 2 per cent high. The
foreground is very nearly the right size already and the background region is
still eating signal, so the two cannot both be fixed by one number.

What that points to is a gap between the two regions rather than a bigger
foreground: a guard ring, whose pixels are neither summed nor used for the
background. Nothing here has one -- `mxi_mask` marks every valid voxel either
foreground or background, and `foreground + background = valid` exactly. DIALS'
boxes have the same property, so if DIALS avoids this some other way the
difference is in where its foreground boundary falls, not in a gap.

### The measurement that settles it

Made on 400 strong low-resolution reflections of a real integration with its
shoeboxes kept. The background per pixel, against distance from the foreground
edge in units of the foreground's own radius, so reach 1.0 is the edge:

    reach   pixels   counts/pixel
      1.0    27676      0.7382
      1.1    34688      0.6731
      1.2    37885      0.6629
      1.3    63527      0.6549
      1.5    65483      0.6279
      2.0    89918      0.5952
      2.5    71674      0.5898
      3.0    22226      0.5800

**The background outside the foreground is not flat.** It falls steadily and
only levels off around 0.58 counts a pixel at two and a half foreground radii.
Just outside the edge it is 0.74, twenty-seven per cent higher. So the spot
reaches well beyond the region called foreground, and a constant fitted over
everything outside that region is fitted partly to the spot -- which is the one
per cent, and which is worst at low resolution because that is where the spots
are strongest.

**A guard ring out to 1.5 radii** would exclude the elevated part. It costs
about fourteen per cent of the background pixels, and Leslie equation 11's
third term goes as m/n, so that term rises about seventeen per cent -- against a
bias it removes of twenty-one per cent of the background in the pixels
concerned. That is a trade worth making and worth measuring afterwards rather
than assuming.

Not built yet. The measurement says where the boundary should go, which is what
was missing; the change itself is in `mxi_mask`, marking voxels between the
foreground and the rim as neither.

## A shoebox table above four gigabytes was written corrupt

msgpack's largest binary type is `bin32`, so four gigabytes is the most a single
column can hold, and there is no larger one to reach for. `put_blob` wrote the
length into four bytes regardless and then appended all of them:

    put(out, 0xC6);
    put_big_endian(out, bytes.size(), 4);   // wraps above 4 GiB
    out += bytes;                            // all of them anyway

A 4.87 GB table declared its shoebox column as 529638180 bytes. The true size is
that plus exactly 2^32, and everything past the declared end was unreachable.
The file opened, parsed, and was wrong.

It now refuses, naming the limit and saying to write a slice. The spot finder's
own writer in `src/spot_table.cc` already had this guard; the one the pipeline
uses did not, which is what two copies of a format get you.

**Shoeboxes cost about 37.5 kB a reflection**, so:

    0.4 GB    10430 reflections    8 per cent of a 125806 row scan
    1.0 GB    26076               21
    2.0 GB    52152               41
    4.0 GB   104304               83

`--first-image` and `--last-image` are how to take a slice, and a tenth of a
scan is plenty to iterate a profile fitting algorithm against -- the profiles
are learned per detector region and per scan block, so what matters is that the
slice covers the detector, not that it covers the scan.

## The fitted variance was wrong twice, and scaling said so

Scaling our integration and DIALS' through the same pipeline gave nearly the
same merging statistics, ours a little worse -- Rmerge 0.043 against 0.038 --
with two numbers that pointed at the errors rather than the intensities:
`dI/s(dI)` 0.946 against 0.840, and an anomalous slope of 1.142 against 0.936.
Both say our differences are large for the sigmas we quote.

They were. Binned against resolution, the variance ratio ours to DIALS':

    resolution        sum       prf
    55.09 - 3.08   1.0031    0.9217
     3.08 - 2.46   0.9988    0.7658
     2.46 - 2.15   1.0034    0.6180
     2.15 - 1.61   1.0239    0.4265

**The summed variance was right everywhere.** Only the fitted one was wrong,
and worse the weaker the data. Two causes, found in that order.

**Fitting on the grid counts the same measurement several times.** The grid has
729 points and a shoebox has about 450 pixels, and one pixel's counts reach
several grid points through the subdivision and the eps3 overlap. Treating them
as independent overcounts the information. `profile_on_pixels` carries the
profile back onto the pixels -- the transpose of the transform that carried the
counts onto the grid -- and `fit_on_pixels` fits there. That took the variance
from 0.43 of DIALS' to 0.66 at high resolution.

**And the background term was missing.** Leslie equation 34: the fitted
variance has two parts, the fit and the background, the second being the same
`(m/n) I_bg` that summation carries. The background was estimated from n pixels
and subtracted from m of them, and weighting the foreground by a profile does
not make that uncertainty go away.

What gave this away was not the comparison with DIALS but the theory. Our
fitted variance was 0.32 of our summed one; Leslie section 6.6 derives a floor
of about 0.5 for a typical profile. **A ratio better than the theory allows is
not a better algorithm, it is a term that has been forgotten.**

Together:

                          var vs DIALS    our prf/sum   their prf/sum
    grid, no bg term         0.6597          0.3219        0.8523
    pixels + equation 34     0.8864          0.4323        0.8523
       at 2.93 - 2.35 A      1.0039          0.5938        0.8939
       at 55.1 - 2.93 A      1.0394          0.8102        0.9658

Still eleven per cent light at the highest resolution, where the data are
weakest and the profile matters most. But the variances now agree with DIALS to
a few per cent over most of the range, and are no longer better than theory
permits.


## Making the second pass pay for itself

Three things, after the variance fix made the second pass worth having.

**The second pass was transforming twice.** It carried the counts onto the grid
and then carried the profile back onto the pixels, and since the fit is over
pixels the first of those is work whose answer is thrown away. They cost the
same, so removing it halves the phase.

**Learning profiles was the last serial phase**, 38 per cent of a slice. The
transform is the cost and it is per reflection with nothing shared; only the
accumulation into a cell is shared, and that is a few hundred adds against a
few hundred thousand multiplies. So each thread keeps its own set of profiles
and they are added at the end.

That makes the result depend on the thread count at the eleventh decimal --
3e-11 on intensities of a few hundred, which is addition of the same numbers in
a different order. Worth saying rather than claiming bit-identical: two runs at
the same thread count agree exactly, two at different counts agree to a part in
1e13.

**And the unused `GridSpec` in a test** was a real signal rather than noise:
that test fits over pixels and needs no grid at all, so the variable being
unused was the code saying the test had been written by editing another one.


## What happens where a reflection crosses a module gap

0xffff is the detector's bad-pixel marker, and 11 per cent of the reflections in
a real dataset have at least one voxel of their box in one; 5 per cent lose more
than half the box.

**Summation drops them and reports what is left.** That is the only thing
summation can do -- the counts are not there -- and the intensity is
correspondingly low. Nothing in the table said so, which is why
`profile.measured` now does.

**Profile fitting was doing the same thing, which defeats its purpose.** Leslie
sections 6.7.2 and 6.7.3: a fitted profile recovers a reflection whose pixels
are missing or saturated, and that is most of its value beyond the variance.
The intensity is `scale x sum(profile)`, and the sum was being taken over the
pixels that were MEASURED rather than over the whole foreground -- so a
reflection with a third of itself in a gap reported two thirds of its
intensity, silently. It now sums the whole profile and fits against the part
that was measured, and `--least-measured` refuses to flag a fit as profile
fitted when too little of the reflection was seen.

That needed a mask change: a bad pixel now keeps its region flag and loses only
its validity, so the fit can tell a foreground voxel with no measurement in it
from one that was never foreground.

**And that change broke three other things before it was noticed**, in the way
a convention change does. Five places tested `mask == 0` to mean "no
measurement", which stopped being true the moment a bad pixel kept a flag --
including the transform that learns profiles, which began putting bad pixels on
the grid as genuine zeroes and punched a hole in every profile learned from a
reflection crossing a gap. They all test validity now.

**Unfinished.** The unit test shows a reflection with a fifth and two fifths of
its foreground masked recovering its full intensity exactly. On real data the
intensities of gap-crossing reflections rise by 1.1 per cent where the measured
fractions say 4.5, and those two numbers should agree. Something is still
wrong, and it is recorded here rather than left as a claim that the problem is
solved.


## A Miller index is not a key on a sweep that goes round twice

The comparison of a ten rotation integration reported:

    6977214 rows against 7309317, matched 647334, 722427 duplicate keys

Nine per cent. Ten rotations record every reflection about twenty times, on
each turn and on each side of the Ewald sphere, and all twenty share their
Miller index and their entering flag. Matching on those alone pairs one of each
and discards the rest -- so every number in that report came from a ninth of
the data, and from pairs that may not have been the same observation.

The index groups the observations; the FRAME separates them. `mxeq` now walks
both sides of a group in frame order and pairs them off, which is right because
two observations of one reflection are a whole turn apart while the two
programs disagree about where one is by a fraction of an image.

The tools say how many observations went unpartnered instead of how many keys
were duplicated, since a duplicate key is no longer a problem and an
unpartnered observation still is.

That the old behaviour was wrong was visible in the report all along -- 722427
duplicate keys is not a footnote on a dataset of seven million -- and it was
read as a quirk of the data rather than as the tool saying it could not do the
job.

**And then the radius was too tight**, which the next run showed at 41 per
cent. The frame difference between the two programs' predictions is 0.001
images for most observations and reaches 1.6 for one in a hundred, so half a
frame threw away a fifth of them:

    radius   0.5   matched  81 per cent
    radius   2.0            96
    radius   5.0            97
    radius  50.0            97

Five and fifty give the same answer, which is what says the pairing is
unambiguous: within a group the observations are a whole turn apart, so the
radius guards against nothing and a tight one only loses real pairs. It is a
sanity check, and it is generous.


## mxeq explain: why an observation found no partner

    mxeq explain integrated.refl mint.refl

Two fixes to the matcher took a ten rotation comparison from nine per cent to
fifty-four, while the same code matches ninety-seven per cent of a single
sweep. So the remaining problem is specific to multiple turns, and a third
guess at it would be a guess. Instead the tool now says why.

For each unpartnered observation it finds the nearest observation of the same
Miller index on the other side, ignoring the entering flag and the radius, and
reports what separates them:

* **not predicted by the other** -- the other program has no such index;
* **entering flag disagrees** -- the same reflection, close in frame, called
  entering by one and leaving by the other;
* **a whole number of turns apart** -- put in different turns, which is a
  prediction disagreement and not a matching one;
* **further apart than the radius** -- the positions differ;
* **within the radius yet unpaired** -- the matcher should have taken it.

The turn length comes from the data: a reflection seen on successive turns is
seen one turn apart with the same flag, so the commonest separation between
repeats is the turn.

On the 1800 frame pair it found something straight away. Of DIALS' unpartnered
observations, 9.8 per cent were within the radius and unpaired -- and that is
DIALS writing **partials**: 423 rows share a `partial_id` with another, a
reflection split into pieces across a block boundary, where this code writes one
row per reflection. One piece pairs and the rest cannot. Small, 0.3 per cent,
and not the ten rotation problem; but real, and the kind of thing that only
shows once a tool is asked to account for everything it did not do.

## The turns were collapsing, and a diagnostic rather than a guess found it

`mxeq explain` on the ten rotation pair:

    unpartnered in integrated.refl (DIALS):
       95.3%  further apart than the radius
    unpartnered in mint.refl (ours):
       85.7%  within the radius yet unpaired

Read together those are the signature of duplicates. If this code predicts two
observations near one of DIALS', the matcher pairs one; the second is left with
a same-index, same-flag partner inside the radius that is already taken; and the
DIALS observation our other copy should have had is left with nothing near it.

A synthetic four turn sweep with a crystal that genuinely moves confirmed it:
218640 of 583040 predictions within five frames of another observation of the
same reflection with the same flag. A scan-static crystal gave none.

**Two bugs in the scan-varying branch of `converge_root`, compounding.**
`ewald_intersections` answers in a principal 2 pi interval whatever turn it is
asked about. The proximity test compared an angle in turn k against two roots in
turn zero, both about 2 pi k away, so which was nearer depended on which side of
zero each fell -- and it could take the OTHER crossing, flipping the entering
flag. And once a pass had assigned a principal angle, the next evaluated the
setting matrix at turn zero's frame rather than turn k's. Each root is now
brought into the current turn before it is compared or kept, and the root on
the seeded side of the sphere is preferred, that being what distinguishes the
two crossings.

**The test that should have caught it used the same A at every scan point** --
which is the scan-static geometry wearing a scan-varying label. It exercised the
code path and not the behaviour. The new one moves the crystal, fails on the
old predictor with exactly 218640, and passes on the new.

On a single sweep the same bug was rarer and still there: five reflections of
111310 had taken the other crossing. None were lost by the fix, all five were
gained, and all have ordinary zeta, so it is not an edge case being redefined.


## Saved shoeboxes follow DIALS' mask convention

`--save-shoeboxes` produced tables DIALS rejected as an invalid structure,
however far under the four gigabyte limit they were. The size limit was a red
herring; the cause was the gap-recovery change.

That change made a bad pixel keep its Foreground or Background bit and lose only
Valid, so a profile fit can tell a foreground voxel with nothing in it from one
that was never foreground. It is what lets a reflection crossing a module gap
keep its intensity. But DIALS' mask calculator sets those bits only on voxels
that are already Valid, so the combination never occurs in a DIALS table:

    spot finder     mask codes 0, 5
    mxi_integrate   mask codes 2, 3, 4, 5     2 and 4: a region bit, no Valid

The container and the byte layout were identical to the spot finder's, which
DIALS reads -- both `[rows, bin]`, both `29 + 9n` bytes a shoebox, both a flag
byte of 2 -- so the only difference was the values.

`to_dials_convention` now puts a mask into DIALS' form before it goes to a file:
a voxel with no measurement is zero, as the spot finder has always written it.
The internal representation keeps the region information the fit needs; the
file follows the format's owner. What is lost in the file is recoverable, the
foreground region being geometry that can be rebuilt from the bounding box and
the profile model -- which matters for re-fitting from saved shoeboxes, and is
worth remembering when that is done.

Not verified against DIALS itself, which is not installed here. The evidence is
that the failure began with the change that introduced codes 2 and 4, and that
everything else about the two writers' output is the same.


## xyzres.px: how well positions were predicted

    xyzres.px.value      observed centre minus predicted, fast / slow / image
    xyzres.px.variance   its variance

Both ends are in the frame the images are in. The prediction is the pixel that
fires -- `Panel::intersect` passes the face intersection through `mm_to_px`,
which adds the parallax offset -- and the centre is of the counts those pixels
recorded, so the residual is a straight difference and needs no correction. NaN
where there was no signal to find a centre in, rather than the zero the
observed column has to fall back to for `dials.scale`'s sake. The prediction's
own uncertainty, from the refined model's covariance, is not propagated.

**The uncertainty is the whole point, and it took four attempts.** It is only
as useful as it is honest, and on real data it cannot be checked -- prediction
error swamps counting noise at every intensity. So it is checked on spots
planted at KNOWN sub-pixel positions with Poisson noise, where an honest
variance gives a pull rms of one:

* weighting by the excess over the background rather than by the count made it
  independent of the background, and far too small for weak spots;
* leaving out the 1/12 of a pixel a count's position within its pixel adds gave
  spots lying on one image a variance of nothing, and pulls of 1e12;
* dropping negative excesses -- what `xyzobs.px` does, for robustness -- gave
  pulls of 1.2 to 1.8, because keeping the pixels that fluctuate up and not
  those that fluctuate down adds scatter the variance does not know about;
* and without that clipping, 0.94 to 1.00 for moderate and strong spots, 0.60
  to 0.75 for very weak ones.

So `xyzres` uses an UNCLIPPED centre of mass and is not `xyzobs - xyzcal`, and
is not meant to be. `xyzobs.px` keeps the clipped one, which is what
`dials.scale` reads and which a weak reflection's negative pixels cannot throw
about. There is a test for the pull at two signals and two backgrounds.

### A centre only for a reflection that was found

The first version gave a centre to any reflection whose summed excess W was
positive, and on real data that meant values like

    xyzres.px.value      -355142.28  -800627.82  -809973.37   (the minimum)
    xyzres.px.variance   8.1e21      4.1e22      4.2e22       (the maximum)

The centre is `sum(w x) / W`, and a W that is barely positive by noise throws
it anywhere -- 117 of 17203 residuals more than fifty pixels out, with a median
I/sigma of 0.07 among them. The variance said so honestly, and it did not help:
a min, a max, a plot or an unweighted mean is wrecked by one such row.

Now W must stand clear of its own noise, three standard deviations by default
with `sigma_W^2 = sum(counts)`: the same quantity the instability comes from,
rather than an intensity cut chosen for the purpose. And the centre must lie
inside its own box -- a centre of mass with signed weights can leave the region
it was taken over, and one that has is describing the noise. What remains runs
to about five pixels on the detector. Along the scan it reaches seventeen
images, which is the near-axis reflections whose boxes are forty to eighty
images deep; their variance says sigma of about ten images, so those are
honest rather than wild.

### Reading it

What makes the column useful is separating the two components of the scatter.
The counting part is `sqrt(variance)` and falls as 1/sqrt(I); the observed
spread does not fall as fast, and what it has beyond counting,
`sqrt(spread^2 - variance)`, is the prediction error. On strong reflections the
counting part is a hundredth of a pixel and the spread is nearly all prediction
error; on weak ones they are comparable. A median that is not zero is a
systematic, and one that is the same at every intensity is a systematic in the
model rather than in the centroiding.

### A warning about the measurements made here

Both `.expt` files in this conversation are now the 1800 frame `i04-ins-small`
model, while the only images are the 300 frame `minute` sweep. Every real-data
measurement made after those files arrived integrated one dataset's images with
another's model. That covers a first table of these residuals, which showed a
constant -0.15 image offset in z that would have been reported as a finding,
and it covers the unresolved gap-recovery discrepancy above, where the
intensities of gap-crossing reflections rose 1.1 per cent where the measured
fractions said 4.5. That discrepancy may be this mismatch rather than a bug, and
needs measuring again on matched data before anything is changed because of it.

The variance fix from the same period is not affected: it was confirmed
independently by scaling on matched data.

### mxeq residuals: plotting it

    mxeq residuals integrated.refl -o residuals.html
    mxeq residuals integrated.refl --least-signal 3      # weaker ones too

An HTML page, plotly from its CDN as with `mxeq html`, with four views, each
answering a different question:

* **against image number** -- whether the scan-varying model has drifted. A
  median that wanders along the scan is the crystal or the beam moving in a way
  refinement did not follow.
* **against resolution**, on 1/d^2 -- whether the cell is right. Cell and
  distance errors both scale positions with 1/d, so they grow outward.
* **across the detector**, the median fast and slow residual in a grid of
  cells -- whether the detector model is right. A pattern that follows the
  panel rather than the reflections is a tilt, a distance or the parallax.
* **the pull**, residual over sigma, against a unit Gaussian -- whether the
  prediction error is larger than the counting noise at all, and by how much.

Above them a table: median (the systematic), spread (the robust scatter),
counting (the median sigma) and prediction, `sqrt(spread^2 - counting^2)`.
Strong reflections by default, I/sigma of ten or more, because for those the
counting part is a hundredth of a pixel and what is left is the prediction.
Rows with no centre of mass are skipped rather than drawn as zero, which would
pull every median toward a perfect prediction.

## Leaving reflections were predicted with the crystal from the wrong end of the scan

`mxeq residuals` on a matched 1800 image run showed a z median of -0.154 images
at I/sigma of ten or more. The same offset had appeared, and been set aside, on
mismatched inputs -- which, since it survived a change of model, said it came
from the code rather than the model.

Narrowed down in steps, each of which overturned the reading before it:

* DIALS' own z residual was -0.047, ours -0.123: part is in the data, most not.
* On the same reflections the two programs' predictions and observations
  appeared to agree to 0.001 and 0.011 images -- which could not be right if
  their residuals differed by 0.075. It was a median over a mixture.
* Split by direction, DIALS was symmetric (-0.043 entering, -0.050 leaving) and
  ours was not (-0.050 entering, -0.260 leaving).
* Our prediction against DIALS': entering exact to 0.0003 images, leaving 0.227
  late with a spread of 0.34. The earlier median of 0.0007 had landed on the
  entering half.
* Along the scan: leaving reflections exact to image 900, then up to 0.96
  images late.

**The cause.** `ewald_intersections` answers in whatever 2 pi interval its
arithmetic lands in, and returned a leaving root of -259.9 degrees for a
reflection at 100.1 on a scan from 0 to 180. `converge_root` looked the crystal
up at `z_from_phi` of that -- image -2600, which `setting_at` clamps to image
zero -- and so used the crystal from the START of the scan for a reflection in
its middle. Invisible where the crystal at image zero is nearly the crystal at
the reflection, which is the first half; entering roots happened to come back
inside the scan and were never touched. It was there in the first scan-varying
version and every fix to the turn handling since.

**The fix, and a first version that was wrong.** The seed is moved into the
scan before the crystal is looked up. Wrapping into [start, start + 2 pi) cured
34005 predictions and broke 91 correct ones: a root a little before the scan
start went to the far end of the window, the same mistake at the other end. For
a scan of less than a turn the window is now centred on the scan's middle; a
longer scan keeps the first turn, which `emit_turns` expects.

**What was affected.** Prediction, and through it integration -- a leaving
reflection in the second half had its shoebox placed up to a frame late along
the scan, so its intensity and centre were taken from the wrong images.
Refinement was NOT affected: it looks the crystal up at the observed image,
which is always inside the scan, and handles the wrap itself.

**The test is physics rather than agreement.** Rotate A(z) h to the predicted
phi, with z the predicted image, and it must land on the Ewald sphere. On a
crystal turning half a degree through a scan the committed predictor put 34005
predictions off it; the start-anchored wrap 184; the centred one none, worst
2.3e-6. No earlier test could have caught this, because none used a crystal that
moved enough for its start to differ from its middle.

### Also found, not fixed

`setting_at` evaluates `A_at_scan_points` as B-spline control points. DIALS
writes the VALUES of A at each image boundary and interpolates them linearly;
reading them linearly makes entering predictions agree with DIALS to 0.0000
images where the spline gives 0.0003. Small, but a genuine difference of
convention -- and it is not changed here because which convention is right
depends on what this package's own refiner writes into that field, which has to
be settled first.

## The sinusoid in the z residual: what it is not, and one thing it does

After the leaving-reflection fix, the z residual against image number still
oscillated -- about +/-0.1 images over the 1800 image scan -- with a mean of
-0.07, where scan-varying refinement ought to have absorbed anything smooth.

**It is not in this integrator.** DIALS shows the same curve, and on the same
reflections ours follows DIALS' to about 0.01 images.

**It is not a failure of refinement either.** DIALS refines against the SPOT
FINDER's centres, and against those its own residual is flat, -0.028 to +0.008.
The spline absorbed everything it was shown. The curve is in the difference
between two definitions of where a spot is along the scan: for the same
reflection, with the prediction identical to five places, the integrator's z
centre less the spot finder's runs -0.006 to -0.077 to +0.018 along the scan,
while on the detector the two agree to about 0.01 px.

It is concentrated on NARROW rocking curves -- -0.097 at |zeta| of 0.9 to 1.0,
nothing below 0.4 -- and about equal for entering and leaving.

### A hypothesis that was wrong

That the rocking curve is asymmetric, the spot finder seeing only the peak and
the integrator the tails. It predicts the difference growing with the WIDTH of
the curve, and the data have it growing with the narrowness.

### The pixel plant

`tools/plant_narrow_spots.cc` plants spots at known sub-image positions, each
image integrating the rocking curve over its own oscillation -- which is what
matters for a spot thinner than an image -- and measures three centres against
the truth: the spot finder's (raw counts over pixels above threshold, a frame at
k + 0.5, a single-frame spot at its frame's middle, as `dials_spots.cc` does)
and the integrator's clipped and unclipped ones.

**Sub-image quantisation is large and averages to nothing.** A spot thinner
than an image is pulled toward its frame's centre, by +/-0.2 images at a sigma of
0.15 and +/-0.06 at 0.3, and all three estimators share it. Over reflections at
random positions within their frames it cancels, to 1e-4 for every estimator at
every width. So it cannot make a systematic, and is not the cause.

**But the integrator's centre follows its foreground window, and the spot
finder's does not.** With the window centred 0.1 images from the truth:

                    spot finder    integrator
    sigma 0.15         +0.0002       +0.0998     follows the window entirely
    sigma 0.30         -0.0007       +0.0205
    sigma 1.00         -0.0002       +0.0089     barely

Largest for thin spots, and the spot finder untouched: the signature the real
data have. It also means that **for a thin spot the integrator's z centre
reports where its window is, not where the spot is** -- and since the window is
placed on the prediction, `xyzres.px` in z is pulled toward zero for thin spots
whatever the true error. The z residual understates the prediction error, and
understates it most for the spots that ought to measure it best.

### Where that leaves it

For the integrator's residual to reach -0.1 on thin spots, its window would
have to sit about 0.1 images from the prediction there. In the only range with
saved shoeboxes, images 0 to 180, it sits on the prediction to +0.003 for thin
spots -- and there the effect is small too, -0.006, so that is consistent without
being decisive. Two ways the window could drift from the prediction are ruled
out: the oscillation is uniform to 1e-11 degrees, so the mask's image-to-phi
mapping and `z_from_phi` cannot diverge that way.

Settling it needs saved shoeboxes from images 450 to 600, where the effect is
largest: the measurement is the foreground window's centre against the
prediction there.

## The outliers along the module edges were a flag, not an intensity

`dials.scale` on this package's output rejected 5096 reflections, and 92 per
cent of them lay within three pixels of a module edge -- 3.4 per cent of an
Eiger 16M's area, a 27-fold concentration. They sat on the modules either side
of the edge, evenly split, none in the gaps, twice as many at the 38 pixel gaps
as the 12: spots straddling an edge, part of each lost to it. Chip joins inside
modules were not enriched at all. DIALS' own scaling showed nothing of the kind.

Two explanations were wrong before the right one:

* **That our recovered intensities were wrong.** Judged against each
  reflection's own clean symmetry equivalents -- independent of DIALS -- ours
  come to 0.967 of them where 10 to 30 per cent was lost, DIALS' to 0.777. Ours
  are the better intensities.
* **That our variances were too small.** They are LARGER than DIALS' for these
  reflections, 24 per cent in variance, which should mean fewer rejections.

**What differs is which reflections reach scaling.** Read from DIALS' source:
its Flags enum has ForegroundIncludesBadPixels (bit 14) and
FailedDuringSummation (bit 19), and on a real integration it sets both on 96 per
cent of gap-crossing reflections and IntegratedSum on only 4.4. `dials.scale`'s
combined intensity selects on `get_flags(integrated, all=True)` -- IntegratedSum
AND IntegratedPrf -- so only 62 of DIALS' 1416 gap-crossing reflections reach it.
This package set IntegratedSum on all of them, and 1415 did. The combined
intensity leans on the sum for strong reflections, so a truncated sum flagged as
good is what scaling saw, and rejected.

DIALS' convention is the honest one: a sum over a foreground with pixels
missing is not the reflection's intensity. `summation_flags` now follows it, so
a reflection whose foreground reaches a masked pixel carries
ForegroundIncludesBadPixels and FailedDuringSummation and not IntegratedSum;
one whose background alone does keeps its sum and carries
BackgroundIncludesBadPixels. The profile-fitted intensity keeps its own flag.

**The trade this makes.** Withholding IntegratedSum keeps these reflections out
of `dials.scale`'s default, combined intensity -- including their profile-fitted
intensities, which are the good ones. `dials.scale intensity_choice=profile`
selects on IntegratedPrf alone and lets them in. Whether that is worth it is a
question for the merging statistics, not for this flag.

### And a wrong table found on the way

`mxeq.checks.common.FLAGS` had integrated_sum and integrated_prf at bits 11
and 12 -- DIALS' overlapped_bg and overlapped_fg -- and a bad_shoebox at 16
that DIALS does not have. Nothing read it, which is the only reason no result
was wrong. It is corrected from DIALS' enum and `test_flags.py` holds it to the
C++ constants, so the two cannot drift.

## Reading each frame once a pass

A 3600 frame Eiger 16M run reported

    20224 frames read (3600 wanted by a shoebox, each read 5.62 times)

and decompression was 238 thread-seconds, 29 per cent of the run. The
decompressor was not slow -- 11.8 ms a frame here against 10.1 for the spot
finder on the same data, thresholding included. There were five and a half
times as many frames.

Two passes account for a factor of two. The rest was the windowing: a window
took its boxes, read every frame they touched, integrated, and discarded them.
A box starting in one window and ending past it made that window read on into
the next one's frames, which the next read again for its own boxes -- and
near-axis reflections run forty to eighty frames deep, so every boundary
repeated them.

Boxes now outlive a chunk of frames. Each chunk opens the boxes starting in it,
reads its frames once into every open box, and closes those whose last frame it
held. A frame is read once a pass however deep the boxes crossing it are. The
chunk is short, 64 frames by default, because what is open at once is now a
chunk's boxes plus those running on from before; `--max-boxes` caps the boxes
opened in a chunk and shortens the chunk when it bites, never splitting the
boxes of one frame, since a box not yet opened would miss the chunk's frames.

Forcing the old version into many windows on a 300 frame slice:

    max-boxes    windowed                 chunked
    3000         1734 frames, 4.87 each   600 frames, 2.00 each
    1500         2966 frames, 8.33 each   600 frames, 2.00 each

Everything from summation is bit-identical between the two -- flags,
intensities, variances, background, centres -- and the profile-fitted values
agree to 7.5e-15, which is profile learning adding the same reflections in a
different order. For the 16M run, 20224 frames becomes 7200.

Reading was also 3.6 seconds against 12.1 with the same 600 frames at the
default settings, which is filling fewer boxes at once.

## Profile fitting was the face, not the fit

Profile fitting was 53 per cent of the 16M run. Timed on 3000 real boxes,
`profile_on_pixels` was 0.772 ms a box and `fit_on_pixels` 0.027: the fit is
nothing, carrying the profile onto the pixels is everything.

Two things in it, and only one mattered:

    change                              speed    boxes differing
    blend each image's planes once      1.3 x    0 of 3000
    ... and the face from pixel corners 5.5 x    147 of 3000

**The voxel loop** ran subdivisions times planes for every voxel, where which
planes a voxel overlaps depends only on its image and which cell a subdivision
lands in only on its pixel. Blending each image's planes into one slice once,
and giving each pixel the short list of cells its subdivisions reach, is exact
and worth 1.3 times.

**The face** called `epsilon_of` for every subdivision -- 8100 calls for an 18
by 18 box. eps1 and eps2 are smooth over a pixel, so computing them at the
(nx + 1)(ny + 1) corners and interpolating is 22 times fewer calls. That is
where the time was. But it is not exact: good to about 1e-7 degrees, which
moved a subdivision across a cell boundary in 147 of 3000 real boxes and a
fitted intensity by at most 0.012 of its own sigma. Immaterial, and not the
same answer.

**So a subdivision within a thousandth of a cell of a boundary -- about 160
times the interpolation error -- is done exactly.** Some two in a thousand, 16
calls a box. The result is the direct version's: largest difference 2.8e-17, no
box differing, fitted intensities agreeing to 1e-15 of sigma.

The direct version stays, as `profile_on_pixels_direct`, and is the
specification: the test holds the fast one to it on 1500 real boxes with a
SHAPED profile, since with a flat one a subdivision in the wrong cell gives the
same value and nothing could see it. With the boundary guard removed, the test
fails.

4.8 times faster in isolation. End to end on a 300 frame slice, with both this
and the frame reading:

                      before     after
    frames read        1734        600
    reading frames    8.07 s     3.77 s
    profile fitting  28.08 s     5.78 s
    total            53.63 s    26.48 s

with summation bit-identical and profile fitting agreeing to 9.3e-15.

One reading of these numbers was wrong: reading looked slower a frame after
the change, 6.3 ms against 4.65, and the guess was the chunk being a
synchronisation point. It is flat from 32 to 256 frames a chunk. The old figure
was flattered -- a frame read for the fifth time comes from the page cache, and
the average was over those.


## Deterministic profile learning, and the race it removed

Two runs at the same thread count differed by 3.6e-11 in the profile-fitted
intensities, which was put down to addition in an order the scheduler chose.
Rewriting it as fixed blocks turned up something worse first.

Each thread took a partial-sum lane through `thread_local` state. The threads in
`in_parallel` are new on every call, but the calling thread runs the same loop
in every call and so lives through all of them: a lane it chose once outlived
the call that chose it, while every later call's new threads were numbered from
zero again. A probe recording which thread used which lane found two threads
sharing lane 0 in 32 calls of a thirty image run. The calling thread had done
all of three small early calls alone, on lane 0, and kept it.

That is two threads adding into the same partial reference profiles at once: a
data race, undefined behaviour, and lost contributions to the learned profiles.
ThreadSanitizer, run on that very case, reported nothing -- it reports a race
when both threads write, and in a small run the two sharers seldom both learned
a reflection in one call. A sanitizer that sees nothing has watched one
execution, not all of them. The frame reader's timing lane had the same flaw,
harmless to the result and a race all the same.

`in_parallel_by_worker` now hands each call's threads a number, 0 for the caller
and 1 to n - 1 for the rest, stable within the call and meaningless across
calls; the reader's timing lane is that number. Profile learning no longer has
lanes: the closing reflections are cut into sixteen blocks by index, each summed
in order by one task into its own partial, and the partials added in block
order. Which thread runs which block changes nothing.

One, two, four twice and eight threads now give byte-identical output. Against
the old single-thread run, profile-fitted values move by 3.7e-15 relative, which
is the regrouping, and summation not at all. The test runs the real program at
one thread and at four, twice, with short chunks for many reductions; against
the old code it fails with "two four-thread runs differ".


## The same geometry for learning

Profile learning's `transform_shoebox` still called `epsilon_of` for every
subdivision after profile fitting had stopped. Measured before changing it: 1.21
ms a box, the face 49 per cent of that. Not most, this time -- it carries counts
ONTO the grid and wrote three arrays for every subdivision and plane, so the
voxel loop was the other half.

Both halves changed. The face is now shared: `pixel_cells` does the corner
interpolation and the boundary guard for both transforms, and `plane_weights`
the eps3 planes, so the two cannot come to disagree about which cell a pixel
reaches. And each voxel writes a reached cell once, with the number of
subdivisions that reached it. One detail of the direct code had to be kept: an
image spanning no eps3 range skips its voxels entirely, but one that spans a
range and meets no plane still counts its foreground toward `outside`.

0.28 ms a box, 4.3 times faster. Against `transform_shoebox_direct`, kept as the
specification, the grid arrays agree to 1.4e-13 on values up to 36; a subdivision
in the wrong cell would be about 0.06. `outside`, being 1 - inside / total with
inside now added once per reached cell rather than per subdivision, differs by
up to 1.8e-12 of rounding. The test fails with the boundary guard off -- and so
then does the fitting test, the guard being shared -- and with the validity rule
broken in the fast transform alone.

On one thread, warm, 120 images: learning 7.17 seconds to 1.64, the run 17.6 to
12.2. A first comparison said 26.5 to 12.0, and was the "before" run reading its
images from a cold cache. The same 2980 reflections are learned, flags and
summation are identical, profile-fitted values move by 5.2e-15, the thread count
still changes no byte, and the tests and an integration are clean under
AddressSanitizer and UndefinedBehaviorSanitizer.


## The scan points were two things, and nothing said which

Open for a while as "the scan-point convention", and settled by reading the code
rather than measuring: the WRITER was already right. `mxi_refine` evaluates its
spline at the N + 1 image boundaries, and samples read from a file are marked
`A_points_are_samples` and written back untouched. But the flag was consulted
only by the writer, and that made three bugs from one missing rule.

* **Reading.** `A_at` put every crystal's points through the cubic B-spline,
  samples included: smoothed a second time, and n points stretched over n + 1
  segments. Predictions from DIALS' own model on 1800 images of insulin were
  0.00033 images from DIALS' predictions, spread 0.00052. Interpolated linearly
  between image boundaries, as DIALS does: 0.00000, spread 0.00002, over 121567
  reflections.
* **Refining again.** Refinement replaced the points with control points only
  when their NUMBER differed, and never cleared the flag. Refining a
  scan-varying model a second time wrote its five control points untouched as
  if they were samples: five scan points for three hundred images, which DIALS
  cannot read. The reader now refuses any count but N + 1, with a message.
* **The static pass.** A scan-varying crystal predicts from its points, so a
  static refinement of A beneath them moved nothing: given a scan-varying
  model, the static pass refined the detector and the beam alone, starting from
  0.303 px it had not earned. A crystal refined statically is now static, as in
  DIALS, and the same pass starts at 0.309 / 0.245 / 0.271, exactly where a
  static crystal fresh from indexing starts.

Our own round trip -- the predictions `mxi_refine` makes in memory against
those from reading its `refined.expt` back -- is twice as close, 0.00027 images
rms against 0.00052. Not exact, because a line between two image boundaries is
not the cubic the refinement fitted between them: the resolution of the format,
DIALS' as much as ours.

On a 300 image integration the change moves predictions by at most 0.0044
images, leaves every summed intensity as it was, and moves profile-fitted ones
by a median ratio of 0.999998. The model predicting them is now the one refined.
Four tests, one for each rule, each failing against the old code.


## What the z offset follows is strength

On a 3600 image Eiger 16M sweep the z residual repeats every 1800 images --
every 180 degrees of rotation, so it follows the crystal's orientation and not
time -- around a mean of -0.071 images. The 300 image sweep a DIALS log came
from shows the offset, -0.145 in xyzres and -0.115 in the integrator's own
centre, and its first 30 degrees of the cycle as a drift.

The decomposition that mattered was over the SAME reflections, and split by
strength:

    I/sigma      spot - cal    integ - cal    integ - spot
    3 - 10         +0.102        -0.058         -0.164
    10 - 20        +0.042        -0.088         -0.122
    20 - 50        -0.122        -0.123         +0.007
    50 - 100       -0.172        -0.148         +0.024
    100 -          -0.181        -0.155         +0.013

For strong spots the two centroid methods AGREE, and both put the spot about
0.15 images before the prediction. For weak ones they part, the spot finder late
and the integrator early. The integrator's centres against the prediction are
skewed, -1.60, a long tail toward earlier images.

The reading: the rocking curve has a tail toward earlier images; for a weak spot
the spot finder sees only the pixels above its threshold, which is the peak, and
for a strong one the whole curve, near its mean. Refinement fits both, so the
model sits between, and every strong spot looks early by the difference. An
asymmetry that varies with orientation gives a residual with the crystal's
two-fold symmetry about the spindle.

The test: refine against the integrator's centres, which measure the whole
profile, and integrate again. The offset over I/sigma of ten or more went from
-0.115 images to -0.008; the drift along the scan, -0.085 to -0.143, went flat
at -0.008 to -0.011; the dependence on strength halved, to +0.016 at the weak
end and -0.038 at the strong; refinement's own z RMSD fell from 0.241 images to
0.220. The scan-varying model absorbs it, as it should, once it is given
centres that mean what integration means.

This overturns the explanation recorded above, that the integrator's centre
follows its foreground window. The pixel plant shows that effect is real, and it
is not this: the integrator agrees with the spot finder for exactly the strong
spots that carry the offset.


## Post-refinement, and the rows it may use

`mxi_integrate --postrefine` integrates, refines against the centres it
measured, and integrates again. The first measurement of the idea -- refining
on every integrated row with `mxi_refine` -- took the z offset from -0.115
images to -0.008. It had included about six thousand rows that were not summed
or had no centre of mass, and a row without a centre carries its PREDICTION as
its observed position: a residual of exactly zero, pulling the refinement back
toward the model it started from. Restricted to the 14959 summed rows with a
centre, the scan-varying z RMSD is 0.177 images where it had been 0.220, and the
offset in the second integration -0.006, flat along the scan at -0.004 to
-0.010. The test of that selection fails with the centre-of-mass check removed.

The refinement is the one mxi_refine runs, not a copy of it: the static then
scan-varying procedure moved into the library as `refine_in_two_passes`, and
mxi_refine's files are byte-identical through it. The program composes two of
its own runs around it, with the models written between, because integration is
still one body inside `run_program`; making it a function is refactoring of its
own.

## Memory written once, and a missing frame an error

Graeme compared ferritin on a Ryzen 9950X with an RTX 4060 against an M4 Max, 13
November 2026: integration 106 s against 49, and almost all the difference in
three parts that move memory -- reading frames 31 s against 6, profile fitting's
packing, opening shoeboxes -- where the background fit, pure arithmetic, was
faster on the Ryzen. The desktop's memory gives some 45 GB/s, the M4 Max's
several hundred; and each pass over ferritin's 6.2 million shoeboxes is some 200
GB.

/usr/bin/time counted 38 million page faults. Each frame was decompressed into a
new vector, 36 or 72 MB, past what glibc's malloc keeps: a new mapping from the
kernel each frame, zero-filled page by page, then returned. Each worker now
keeps its compressed frame and its pixels for the run, as `series::Frame` was
made for and spot finding already did: 7 million faults, reading frames 24 s.

Building a box zeroed its values, and integration zeroed them again, before
filling every one from the frames: some 320 GB of zeros a run. The values are
now `Voxels`, a vector whose resize does not clear, and `build_shoebox` leaves
them uncleared when asked (`zero_pixels`), as integration asks. Every voxel of a
box lies on the panel and on a frame of the scan -- `integration_bbox` clamps it
to both -- so every voxel is written before it is read; the planted series
integrates to the same columns, exactly, through the CPU and the emulated
device, before and after. The background is still cleared: a box whose
background fit fails keeps the zeros, and profile learning and saved shoeboxes
read them.

Which makes a missing frame -- an unallocated chunk, a frame the writer never
received -- something integration can no longer carry: its boxes would hold
whatever their memory held. It had been counted, the boxes spanning it
integrating low with a slice of zeros, and a warning printed at the end. Graeme
has never seen an NXmx file with one, over a great deal of data, and would
rather mxi said so: integration now stops at the first, naming the image and the
file. Spot finding still finds spots around one (docs/spots.md).

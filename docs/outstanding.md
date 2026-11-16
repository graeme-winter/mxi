# What is outstanding

STATUS: the one list of open work, gathered from every document. An item's
evidence is in the document named; when an item is done, it moves to "Closed"
at the end and the document it concerns says what was found. Numbers do not
change when items close. Items are grouped by where the work is, and
marked **correctness** where a number is known to be wrong, or **capability**
where something is missing. Every open item was checked against the code and the
history on 20 October: four closed, nine rewritten to what is now true.

## Integration

* **80.** **output -- refinement's summary when sweeps are refined apart.** Each
  sweep's run (refine_apart) writes its own summary, but the joined file's
  mxi refine step keeps only "apart" and "groups": none of the per-sweep cells,
  RMSDs, cycles or cell over the scan. mxi report then draws the cell over the
  scan from the file's crystals (correct, since 26 October), but the rest of
  refinement's section is empty for any multi-sweep run. The sweeps' records
  should be joined into the summary, their experiments renumbered as the
  joined list's. Seen 26 October.
* **78.** **Against a refined structure (`mxeq truth`, 21 October).** Graeme's
  small molecule, four sweeps, refined to R1 0.026: over its 12259 scaled
  observations R(F^2) 0.066, z's robust variance 0.81 but 1.9 per cent beyond
  5 sigma. Scaling rejected 77, 74 of them beyond 5 sigma, and kept 154 more.
  Drawn (`--plots`), most of the outliers are shadows: (a) in l-cyst_01 and
  l-cyst_03 (the beam at pixel 190, 866, near the detector's edge) a cluster
  beside the beam, both signs, images 550-780 -- the backstop's edge, as on
  ferritin (docs/backstop.md); (b) in l-cyst_04 (the beam at 729, 866, the
  detector centred) low outliers along a vertical line from the top to the beam
  -- the backstop's arm -- and a diagonal, the rotation axis (Graeme), along
  its whole rotation. Near the axis: the decile of smallest |zeta| has 6.7 per
  cent beyond 5 sigma, against about 1 elsewhere. Measured as (Fo - Fc) / Fc,
  strong reflections read low more often than high (2.9-4.2 per cent more
  than 10 per cent low, 1.1-2.4 high); the high ones are not scaling's (the
  same inverse scale, 0.98-1.00, in every group) but shadowed backgrounds --
  34 per cent of them below 0.8 of the background usual at their resolution,
  against 15 per cent of the rest, 30 per cent within 300 px of the beam
  against 5.5. Shadow on the background reads high, on the spot low;
  near the axis, `mxi integrate --min-zeta` (0.05 by default, as DIALS): 0.2
  would take a fifth of the outliers beyond 5 sigma, 0.3 a quarter, no unique
  reflection lost (docs/integration.md) -- confirmed by integrating with 0.2:
  42 outliers gone, the rest unchanged, the sigmas better calibrated;
  masking the backstop and its arm (mxi_mask, 22 October): R(F^2) 0.0657 to
  0.0627, l-cyst_04 2.9 to 2.4 per cent beyond 5 sigma, the other three little
  changed -- their outliers are not near the beam. Their shoeboxes, from the
  whole scaled file, showed why: near-axis streaks running out of their
  foreground (what --min-zeta 0.2 takes), a first-image partial (item 1), and
  in images 600-800 of l-cyst_03 (550-700 of l-cyst_01) every spot displaced
  together, y by -0.3 to -0.4 px and the rotation by up to half an image, a
  fifth to a quarter of them more than a pixel off, the intensities' spread
  against Fc^2 wide (0.60-0.96 at image 750, against 0.99-1.01 at 800).
  Refining again with or without the crystal tie, or with control points every
  2 deg, leaves the y shift: not the crystal turning but all its spots moving,
  as a crystal partly leaving the beam -- or the beam moving -- would make
  them; refinement's beam and detector are static over a sweep. Leaving those
  images out, with |zeta| below 0.2: R(F^2) 0.0627 to 0.0566, 219 beyond 5
  sigma to 122, 10 unique reflections of 2606 lost. Graeme's model of the centre
  of diffraction turning off the axis, two parameters a lattice, is now
  `mxi refine --sample-offset` (stage 1): on this data 5 to 20 um, l-cyst_03
  the most, the RMSDs as before and the shift in images 650-800 untouched -- a
  disturbance over 20 deg of a 170 deg sweep is not a circle. Next, if wanted:
  the offset varying along the scan (stage 2), or image ranges left out;
  Graeme's run with all three -- the mask, --min-zeta 0.2, --sample-offset
  (offsets 4-18 um) -- 23 October: R(F^2) 0.0618 and 186 beyond 5 sigma,
  against 0.0657 and 228 at first; the sigmas calibrated (robust variance of z
  1.02); l-cyst_04 2.9 to 1.6 per cent. The offset neutral here (the mask and
  the cut alone predict the same R), the crystal well centred; l-cyst_03's
  images 650-800 unchanged, the largest left (leaving them out: R 0.0566).
  Left out with `mxi scale --exclude 6:600:800 --exclude 0:550-700`: R(F^2)
  0.0618 to 0.0567, as predicted, 10 unique reflections of 2606 lost; the
  sigmas against each reflection's own equivalents 0.65 to 0.90 (robust
  variance) -- against Fc^2 1.02 to 1.63 only as the model's share of the
  disagreement grew, 55 to 65 per cent. The error model then took its split
  form; regression and individual fits within 0.03 of each other (item 69); (c) the lowest resolution,
  1.6-6.7 A, 10 per cent beyond 5 sigma, 61 per cent of it shared within
  reflections (the model's: Fc^2 a little low at low angle, no bonding
  density) and 39 the processing's; (d) a partial on a sweep's first image,
  partiality 0.50, measuring 0.36 of its full intensity (item 1); and the
  error model's shape, item 69.
* **76.** **question -- mxi correlation's per-data-set B against DIALS's.**
  mxi correlation now normalises each data set (docs/correlation.md), which
  brought the cows to DIALS's one cluster; but its B values, the isotropic
  equivalent of mxi's anisotropic ML fit, are 16-20 A^2 where
  dials.correlation_matrix prints 11-16, the ranking different (mxi has the
  most extreme data set highest, DIALS's log lowest -- though DIALS's log
  repeats its first data set's values for its twelfth, so its numbering may
  be off). The conventions differ (DIALS's exp(B d*^2 / 4), mxi's B from
  exp(-B d*^2 / 2)), as do the models (isotropic with mmtbx's protein terms;
  anisotropic, a light atom). To settle: the same model both ways, data set
  by data set. 20 October.
* **75.** **validation -- the real-space grid search is not yet shown better than
  the FFT on data.** `mxi index --real-space-grid` (Gildea et al. 2014) finds
  exactly the FFT's basis on insulin's four sweeps and on a four-sweep small
  molecule, and a planted lattice in a test; but on both data sets the FFT
  succeeds anyway, so its advantage -- sparse, stills-like or multi-lattice
  data, where the FFT's peaks are weak or confused -- is the paper's, not yet
  measured here. Wanted: such a data set, for the regression set. Its cost:
  directions x cell lengths x spots, 16 s on one core for 15477 spots and
  three lengths. 19 October.
* **73.** **A small molecule, mxi against DIALS (Graeme, 18 October).** Four
  sweeps, a 5.4 x 8.1 x 12.0 A cell; mxi's merging a little worse (I/sigma 28
  against 44, inner R_merge 0.033 against 0.024, the error model's a 2.0
  against 1.05). Fixed: refinement -- the scan-varying pass re-admitted the
  static pass's outliers (a quarter to a third of the reflections), which
  dragged its first cycles off: RMSDs 2-5 times DIALS's and a cell edge
  running 5.32 to 6.51 A within a sweep. Now it starts from the static pass's
  reflections, the crystal tied weakly to the static one, and a crystal that
  changes more than a crystal can is a WARNING: RMSDs 0.15/0.19 px, 0.12
  images and like, as DIALS's. Open: (a) sweep 1 still 0.30/0.49 px, its
  static fit poor from indexing; (b) 23 reflections 6.7-12 A integrated but
  neither scaled nor excluded by mxi scale, unsaid, so the merge starts at
  6.73 A against DIALS's 12.03; (c) sigma_m 0.15-0.21 deg against DIALS's
  0.078 -- to see again now the rotation residuals are DIALS's (item 63).
  Graeme, after the fix: SHELXT and SHELXL now give fairly comparable
  structures from the two, DIALS's merging statistics still a little better.
* **62.** **parked -- Graeme's 6S0R set** (P 1 21 1, 50.2 183.9 110.0 A, beta
    94.2; 2026-10-14). Strong reflections out by about a tenth of their
    intensity -- raw calibration 79 for the strongest sixth, 1.2 to 1.6 for
    the weakest -- in DIALS too: dials.scale's error model a = 3.36, b = 0.016,
    its anomalous slope 0.158; mxi's a = 4.9 to 5.3, slope 0.32, its control
    0.32. Graeme's suspicion: egregious spot overlaps (every one of mxi's
    1759021 boxes meets another). Three threads: (i) mxi's sigma_m 0.565
    degrees against DIALS's 0.316 -- the known overestimate, item 63 --
    though integrating with DIALS's changed the error model little (a 4.86);
    (ii) the error model's form, item 64; (iii) mxi only, the shell 2.40 to
    2.34 A at CC half 0.03 in every run between neighbours of 0.55 to 0.7,
    and one intensity bin of the error model's fit 40 times its neighbours'
    mean square -- a handful of wild observations outlier rejection does not
    catch; DIALS shows no dip there. A diagnostic listing a shell's worst
    observations, where they lie on the detector, would find them.
* **69.** **decision -- the error model's fit: dials.scale's individual, or
    regression.** Found 15 October: dials.scale's default is the fit in turn,
    not regression (docs/scaling.md); --error-model-fit individual brings the
    36 crystals near dials.scale (correlations 0.979 / 0.900 against 0.982 /
    0.915), at the cost of weak sigmas four to six times too large, as
    dials.scale's. Earlier text: **the error model's b, twice dials.scale's on
    many crystals.** Graeme's 36 insulin crystals (three species, 12 each): after
    partials were combined and partiality kept off the profile-fitted
    intensities (docs/scaling.md), mxi scale's b is 0.056 where dials.scale's
    is 0.026 on the same observations, so I / sigma is capped at 3.8
    rather than 9.9. The intensities agree with each other nearly as well as
    dials.scale's (unweighted correlations between crystals 0.991 and 0.956
    within and between species, against 0.994 and 0.967); the sigmas do not,
    and every sigma-weighted statistic follows. Next: the two fits on the same
    scaled intensities -- which observations each uses, its bins, whether
    the species' real differences enter mxi's as error.
    Absolute evidence, 21 October (`mxeq truth`, a small molecule refined
    to R1 0.026 with SHELXL): against Fc^2, mxi's scaled sigmas are about 1.4
    times too large for the weakest observations (robust variance of z 0.5)
    and 1.6 times too small for the strongest (2.45), the model fitted a 1.94,
    b 0.016 -- too much of a, too little of b. The truth-based calibration by
    intensity is the measure to choose the fit by.
* **63.** **decision -- sigma_m: the rocking curve's width, and how far the
    box must reach, are two numbers.** mxsim's note (15 October): the basic
    estimator, one sample an image holding foreground counted once, as
    DIALS's default, measures how far a spot's foreground spans, which grows
    with brightness -- on NE-CAT insulin 0.040 degrees for the weakest sixth to
    0.179 for the strongest. Weighted by each image's counts (DIALS's
    `extended`; `mxi integrate --sigma-m-estimator extended`) it is 0.0835 on
    insulin here, with mxsim's 0.0826, its second moments' 0.083 to 0.090 and
    DIALS's 0.089. But integrated with it insulin merges worse: R merge 0.035 to
    0.039, the low shell 0.026 to 0.032, <I/sigma> 17.5 to 16.4 (as item 3's
    0.086 did). mxi profile says why: at that width the rotation profile's
    core is near Gaussian but its tails are heavy -- 0.934, 0.982 and 0.996 of
    the counts within 2, 3 and 4 sigma, where a Gaussian holds 0.954, 0.997
    and 0.99994 -- so a box of 3 sigma loses 1.8 per cent of every spot's
    counts, unevenly; basic's inflated sigma_m happens to set the box where
    0.999 are held. Proposed: sigma_m from the extended estimator, the box's
    extent in rotation set to the n sigma that holds 0.997 of the counts (4
    here, 0.33 degrees). basic stays the default until decided. sigma_b is
    not a discrepancy: it is the total angle about s1, so a per-axis width
    times root 2, as DIALS's.
* **1.** **correctness -- Partials come out high.** Judged by symmetry equivalents,
   observations with partiality below 0.9 are 10.5 per cent high, and those in
   the first two and last five images 4 to 5 per cent: partiality looks
   underestimated at the ends of the scan, so dividing by it overshoots.
   `docs/scaling.md`, the CC half of the lowest shell.
  `mxeq equivalents` measures it from the data: each observation against its
  reflection's clean equivalents, each cause among observations otherwise clean.
  On the 300 image sweep, partiality 0.8 to 0.9 +9.5 per cent, 0.9 to 0.99 +2.4;
  and no fully recorded, unmasked observation lies in the first or last two
  images, so the excess at the scan's ends is the partials', not its own cause.
  The partials suggest sigma_m itself: recomputing their partiality for a trial
  sigma_m, `mxeq equivalents` finds their median bias zero at 0.086 degrees
  (profile fitted) and 0.082 (summed), against the 0.1286 integrated with --
  recovered exactly from their own partialities -- and DIALS's 0.089 (item 3). A
  sigma_m half again too wide makes a cut box's rocking curve look more partial
  than it is, and dividing by that overshoots: items 1 and 3 are likely one. To
  test, `mxi integrate --sigma-m 0.086` and the tool again.
  Done (October 2026): at 0.086 degrees merging is worse -- R merge 0.035 to
  0.038, merged I/sigma 18.6 to 16.8 -- and the partials' excess mixed, +13.6
  to +7.7 per cent at partiality 0.8 to 0.9, +1.7 to +7.5 at 0.9 to 0.99: not
  sigma_m too wide. And small: some 70 observations of 13300, all at the
  scan's ends -- on a ten-turn sweep, nothing.
  Not repeated on ferritin: there partials are a few per cent low, and suggest
  the sigma_m integrated with (item 3). So insulin's, not the integrator's in
  general -- a question for more data sets.
  Measured before 15 October, when partiality came to divide only the summed
  intensity and a reflection split by integration's blocks to be combined
  before scaling (docs/scaling.md): to measure again with `mxeq equivalents`
  before anything else is done (review, 20 October).
* **2.** **correctness -- Reflections cut by a gap come out a few per cent high** (by gaps masked on purpose: +2 at a tenth cut, +6 at a third; a third less at --grid-points 6, now the default); with real gaps, mixed. First recorded as gap-crossing reflections about 3 per cent low, 3.4 by
   symmetry equivalents in scaling; the suspect is profile learning from
   reflections that are themselves cut. `docs/integration.md`, open questions.
  `mxeq equivalents` on the 300 image sweep: 80 to 95 per cent of the profile on
  valid pixels, fully recorded, -3.1 to -3.7 per cent.
  Measured cleanly (October 2026) by gaps of our own: insulin's master copied
  with stripes four pixels wide masked across it, the same images and models
  integrated with and without, each cut reflection against itself uncut --
  the control 1.0000. Cut reflections come out HIGH, more the more is cut:
  measured 0.90 to 0.95, +2.1 to +2.6 per cent; 0.80 to 0.90, +2.1 to +2.8;
  0.70 to 0.80, +4.5 to +5.2; 0.60 to 0.70, +6.3 to +6.9. A reference profile
  broader than the spot it fits overstates the tail a gap removes, and the
  fit's scale over what is left comes out high. Not the learning from cut
  reflections: learning from uncut ones only changed nothing. Partly the
  grid's smoothing: at --grid-points 6 the bias is a third less (0.60 to
  0.70, +4.5 to +5.9; 0.90 to 0.95, +1.4 to +1.7), at 8 little more; 6 costs
  nothing in time (8.6 s and 8.7 s) and merges as well (R merge and R pim
  the same, CC half 0.986 to 0.987); the device's grids go to 8. The rest
  likely the reference an average of spots of different widths, heavier in
  its tails than any one -- inherent to one shape a region. Summing the
  valid pixels and fitting only the missing part is the same estimate,
  algebraically. --grid-points 6 is now the default (Graeme). The real gaps'
  mixed signs -- low at 0.80 to 0.95, high
  below 0.80 -- are something more, the detector's own at its gaps perhaps:
  to look at with a module boundary's pixels.
* **4.** **correctness -- The background is about one per cent high at low
   resolution.** A guard ring around the foreground would likely fix it.
   `docs/integration.md`.
  Measured on bare background (insulin's crystal turned 3 degrees): mean I
  -0.5 to -1.0 per cent of the background under the foreground from 55 to 2.25
  A, at 4 to 6 standard errors, nothing at high resolution. With no spot
  there, not a spot's tails: a guard ring would not mend it. Likeliest the
  background's curvature near the beam, which a constant model fitted around
  the foreground cannot follow; a plane or a radial term to test it. DIALS's
  default model is constant too.
  Measured again (13 October 2026), and the low-resolution shape was the
  test's: the crystal turned alone sat among the real crystal's spots, which
  it could not mask. Turned and real integrated together, two lattices, the
  real spots masked as in any run: the turned boxes meeting no real reflection
  (13959) are -0.36 to -0.57 per cent at every resolution, +-0.15 to 0.24 --
  not curvature, which would follow resolution -- and those whose foreground
  holds part of a real spot +3 to +9, which had made the high shells positive.
  Not the estimator: flat Poisson backgrounds through glm_background, 0.1 to 2
  counts a pixel, 300 to 1000 pixels, within +0.17 per cent, nothing at
  insulin's pixel counts. Partly the real spots' tails past their masked 3
  sigma foregrounds -- thermal diffuse scattering among them, which every run
  has: -0.58 per cent for the turned boxes nearest a real reflection, -0.28
  +-0.12 for the farthest fifth. Some 0.3 per cent not yet explained. A
  constant fraction of the background, so for a weak reflection -0.4 per cent
  of I_bg under it: about -0.07 sigma at insulin's 300 counts.
* **5.** **correctness -- A dependence of the z centre on strength remains** after
   `--postrefine`, +0.018 images for the weakest to -0.036 for the strongest:
   the integrator's centre on an asymmetric rocking curve.
   `docs/integration.md`.
* **6.** **correctness -- For a thin spot `xyzres.px` in z reports the foreground
   window.** `docs/integration.md`.
* **7.** **capability -- No overload flag.** Overlapping reflections are now
   found and each one's neighbours masked out of its background, flagged
   overlapped_bg and overlapped_fg (`docs/integration.md`, "Several lattices,
   and neighbours masked"); overloads are not flagged. `docs/integration.md`.
* **8.** **decision -- Whether `--postrefine` should be the default**, and what it does
   to scaled intensities: it removes the z offset, at twice the integration
   time. `docs/integration.md`.
* **9.** **correctness -- A reflection predicted just off the panel keeps a
   calculated position of zero** after refinement; `update_predictions`
   reports those rows, and what to write for them is open. `src/refine.hh`.
* **10.** **structure -- Integration is one body inside `run_program`**, so
    `--postrefine` composes two runs of the program. Making it a function --
    one public function per task -- is the refactoring that would remove that.
* **11.** **speed -- Reading frames and profile fitting are a third of the time
    each** on a 16M sweep. `docs/gpu.md`.
    From item 48 (closed): reading on 32 threads is bound by memory traffic,
    some 130 GB of decompressed pixels and 140 GB of box voxels a run; counts
    kept as 16 bits, not floats, would halve the latter -- untried.
    13 November, ferritin on a Ryzen 9950X with an RTX 4060 against an M4 Max
    (integration 106 s against 49): the desktop's memory some 45 GB/s, each
    pass over the 6.2 million boxes some 200 GB. Fixed: a frame buffer a frame
    (38 million page faults to 7), the boxes' values zeroed twice (opening 17 s
    to 12.6); integration 95 s. Left, the structural part: boxes from arenas
    reused chunk to chunk; built where the device reads them, so profile
    fitting's packing -- 18.5 s copying 200 GB into pinned memory, then 15 s
    over the 4060's eight PCIe lanes -- goes; counts as 16 bits and masks as
    bits, shrinking filling and the upload; the background's zeroing, which a
    failed background fit still relies on. Perhaps 60 to 70 s there.
    `docs/gpu.md`.
* **47.** **paused -- Background and summation on a device.** 3.0 s of the
  M4 Max's 14.6 s integration, the largest CPU phase after reading. On one
  thread here, 40 per cent of it is the GLM's iterations and 60 the passes over
  a box's voxels -- gathering the background pixels into a list, summing the
  foreground, two centroid passes -- memory more than arithmetic. Two things
  stand in the way: a box's summation is used at once, by learning the
  reference profiles, so a device's would need the one-pass loop pipelined by a
  chunk to overlap reading; and it would put `intensity.sum`, the background and
  the centroids into single precision, the GLM's 1e-10 becoming 1e-6. The first
  step proposed: `integrate_shoebox` cheaper on the CPU, byte-identical -- the
  GLM over the box's own pixels, fewer passes -- which helps every path and says
  what is left for a device.

## Several lattices

`docs/multi-lattice.md` for the design and the measurements.

* **58.** **difference -- Worst-fitting spots stay indexed.** DIALS leaves its
    worst-fitting out of the indexed set (433 spots of 21299 on the two
    crystals); mxi only out of its fit. Graeme: fine to leave them out, the
    problem being overdetermined, but after the final refinement.

## Refinement

* **12.** **correctness, to re-measure -- Outlier rejection milder than DIALS'**:
    11714 reflections kept against 9580 on the same sweep, RMSD 0.30 px against
    0.19 -- measured before the scan-varying pass came to keep the static
    pass's outliers out (18 October), which on a small molecule brought RMSDs
    to DIALS's. To measure again on that sweep (review, 20 October).
* **14.** **capability -- Refinement does not constrain the cell to the lattice's
    symmetry**: 77.93 77.90 77.89 A after `mxi symmetry`, where DIALS reports
    77.898. `mxi scale`'s cell from two-theta is constrained (cubic: 77.99264(4)
    A on 36 crystals), as dials.two_theta_refine's; the crystal models carried
    through refinement are not. `docs/symmetry.md`.
* **15.** **correctness, untested -- Two conventions with no data to settle them**:
    the order of goniometer composition, and multi-panel detectors
    (`docs/conventions.md`). The third, a scan not beginning at image one, is
    settled: z anchored to the scan's first image, as dxtbx (`docs/multi-sweep.md`,
    step 3), and the spot finder reads only the scan's frames (`docs/spots.md`).
## Symmetry

* **66.** **capability -- cosym refusing a poor fit.** Crystals indexed
    together no longer go to cosym (their orientations agree); crystals
    indexed apart still do, and on a lattice all but of higher symmetry its
    best guess can be wrong -- the small-molecule set's cosines 0.42 and 0.40
    before. Below some cosine it should leave the experiment as it is and
    say so.

* **59.** **capability -- cosym for many partial data sets.** Whole sweeps
    give every entry of cosym's correlation matrix, and its leading
    eigenvectors are the fit; many small wedges, as the paper's examples, will
    want the correlations with too few reflections in common left out, and the
    coordinates fitted around them by L-BFGS. `docs/multi-lattice.md`.
* **18.** **difference -- The <I>/<sigma> limit takes the last shell above
  the threshold**, where dials.symmetry fits a curve: 1.85 A against 2.46 on
  one sweep. The CC half limit is now dials.estimate_resolution's tanh fit.
* **19.** **untested -- The monoclinic setting's name.** The setting is chosen
    with beta nearest 90 and then cctbx's order (src/symmetry.cc), which is
    DIALS's choice -- I 1 2/m 1 where C 1 2/m 1 would have beta far from 90 --
    but no centred monoclinic case has been run to see the name reported
    follow it. Wanted: such a data set, against dials.symmetry.
* **20.** **capability -- No obliquity (delta) column** in the table of subgroups.
* **90.** **capability -- small molecules: mxi triage's resolution window.**
    The twin tests take 10 to 3.5 A, xtriage's choice for a macromolecule; on
    a small molecule's data to 1 A that leaves a handful of reflections --
    ten, on Graeme's R -3 c crystal (13 November). The window should follow
    the data where the cell is small, or `--small-molecule` choose it.
    Parked by Graeme, 13 November.

## Scaling

* **54.** **correctness -- Weak reflections' sigma, the last of it.** The
    fitted variance's background term carried through the fit's weights, and
    a pixel's least variance 0.001 counts rather than 0.05 (since 13 October
    the variance's only, the weights' 0.05 again: item 61): insulin's weakest
    scaled sextile 0.66 to 0.95, the two crystals' 0.26 to 0.42 and 0.58 to
    0.80, and with scaling's variances from the model 0.79 and 1.01. Left:
    the very weakest of the weakest crystal, 0.79. `docs/integration.md`,
    "The fitted variance". The CUDA and Metal kernels carry the change,
    unbuilt here.
    Evidence since (20 October): on Graeme's twelve cows, mxi's sigmas for the
    weakest sixth of reflections calibrate at 1.045 before the error model and
    1.03 after, where DIALS's on the same reflections calibrate at 0.16 -- its
    sigmas 2.5 times too large. Close once the weakest crystal's 0.79 is
    checked again.
* **22.** **capability -- No free-set validation**, the check for overfitting.
## New programs

* **28.** **mxi export: mmCIF.** The unmerged MTZ is written, as dials.export's
    (`docs/export.md`); mmCIF is not. Planned, 6 November: `docs/mmcif.md`,
    the new dialect, merged and unmerged; parked, Graeme doing more homework.
* **91.** **capability -- small molecules: mxi export to SHELX.** MTZ only;
    a small molecule's programs want HKLF 4 -- h, k, l, I, sigma(I) in
    3I4,2F8.2, scaled to fit the field -- and an .ins or .p4p with the cell
    and the space group. Parked by Graeme, 13 November, with
    `mxi symmetry --small-molecule` done.
* **82.** **capability -- mxi triage, the rest of xtriage.** Begun with the
    intensity statistics and the twin laws (`docs/triage.md`); to come: the
    Britton plot and the maximum-likelihood twin fraction; translational NCS
    from the Patterson (its p-value); the Wilson statistics reported -- the ML
    isotropic and anisotropic B, the mean intensity against an expected curve;
    the data's health -- low-resolution completeness, ice rings from the
    intensities, anomalous measurability; and Matthews, given a residue count.
* **83.** **correctness -- mxi triage's centric <I^2>/<I>^2 against
    xtriage's.** On the same insulin data (mxi's scaled.mtz given to xtriage):
    3.27 here, 2.85 there, where the centric N(z) agreed to the third figure and
    xtriage removed no outliers -- a few of the strongest centric reflections,
    not the bulk. xtriage merged scaled.mtz itself; mxi triage merges
    scaled.rflx with its own selection (partiality 0.4): the merging is the next
    place to look. Seen 6 November.
* **84.** **mxi triage's Wilson fit before the twin tests** takes the
    reflections as P1's, all acentric, epsilon 1 (it is `mxi symmetry`'s,
    laue.hh); xtriage's knows each one's. Little to the scale and B, but a fit
    that knows them is the right one.
## The spot finder


* **34.** **untested -- Two paths of the CUDA backend**: its 32-bit
  instantiation, which wants 32-bit data, and its `stage2_direct` kernel
  (`MXI_GPU_STAGE2=direct`). `stage0_tile` has run on real data on an RTX
  4060, output byte-identical to the direct kernel's, and slower. The paths it
  runs agree with Metal exactly. `docs/spots.md`.
* **35.** **speed -- Where the spot finder's time goes.** `mxi find --timing`,
  on 3600 frames of 4148 x 4362 pixels, 10 GB compressed, with 16 threads:
  * **Metal, 13.9 s:** reading from HDF5 39 per cent of the threads' time, 24 ms a
    frame from an external drive -- 10 GB in 13.9 s is 0.72 GB/s, in the range of
    a USB 3.2 drive; a second run with the file in the page cache would say
    whether it is the drive or HDF5's lock -- thresholding 32, decompressing 16.
  * **CUDA, RTX 4060, 32.0 s: the kernels are the limit.** 8.9 ms a frame
    against Metal's 3.9. With 8 threads the wall is the same, 32.5 s, and only
    the waiting falls -- reading back 138 thread-seconds to 43, the rest 137 to
    45 -- while the device's work stays about 110 s: the GPU runs some 3.4
    frames' kernels at once and that is its rate. Of that work, at 8 threads:
    stage 2 (11 x 11) 38 per cent, stage 0 (7 x 7) 27, stage 1 (5 x 5) 23, the
    upload 11 (3.2 ms for 36 MB, 11 GB/s). Stage 0 by tile is slower on this
    card, 37.4 thread-seconds against 32.4, so direct stays the default there,
    the reverse of Apple silicon. Faster means the windowed sums cost less:
    running sums independent of the window's size, or fused stages, measured on
    the card. A fused kernel for that is written (`MXI_GPU_FUSED=1`,
    `docs/spots.md`), verified against the CPU tile by tile and compiled for the
    4060, not yet run: `ctest -R dext_gpu`, then `cmp` and `--timing` against the
    three kernels on real data. HDF5's lock shows too: reading takes 4.6 thread-seconds on 8
    threads and 22.5 on 16, for the same data. `docs/spots.md`.

* **43.** **try -- A device for each stage**, `docs/gpu.md`. Profile fitting,
  `mxi integrate --gpu`, single precision, is done for Metal: on an M4 Max, the
  16M sweep's integration 14.6 s against the CPU's 20.0, overlapped with reading
  frames and packed straight into the GPU's memory; verified on the CPU through
  `--gpu-emulate` (a median of 9e-6 sigma from the double fit) and a pretended
  device. CUDA is to rerun -- slower than 32 CPU threads before the pinned
  upload, the new kernel and the overlap. Background and summation are item 47;
  then the refinement target and decompression, each judged against every CPU
  thread on its machine, and recorded if it loses.
* **44.** **speed -- Indexing's peak search walks the whole grid**: 1.52 s against
  the transform's 0.94 on one thread.

* **49.** **population -- `mxi import` across data sets.** Right on two masters
  from two writers, and against dials.import on insulin the same in all but the
  file's own size and the exposure time (count_time here, 0 there; to settle on
  a matched pair). For each new data set: dials.import and mxi import on the same
  master, `mxeq compare-expt`, and what differs made right or recorded in
  `docs/import.md`. Untested: several modules, a moving detector, beam direction
  or polarisation from the file, materials but silicon.

## The interface and its viewer

* **85.** **capability -- moving or reshaping an untrusted region.** The
    viewer draws, chooses, deletes and undoes rectangles, circles and polygons
    (`docs/ui.md`, `docs/mask.md`); napari's shapes layer also moved a region,
    moved its vertices, and drew ellipses and freehand polygons. Here a region
    is deleted and drawn again.
* **86.** **The resolution under the cursor** is from a multi-panel
    detector's first panel only (`docs/ui.md`).

## Infrastructure
* **92.** **capability -- the Python suite on Windows.** Written for POSIX:
    shell stand-ins, chmod, /bin/sh, /tmp. The Windows workflow runs the C++
    suite and mxi.exe, and installs the wheel and runs it; the Python tests
    are not run there (`docs/windows.md`). Left from item 89.
* **93.** **capability -- CUDA on Windows.** The Windows build is the CPU's
    only; the spot finder's and profile fitting's CUDA paths are built and
    tested on Linux. Left from item 89.
* **97.** **speed -- builds tuned for one CPU (Grace, x86-64-v3).**
    Parked by Graeme, 16 November, until he has a GH200 of his own to
    optimise on properly; the Pi 4 a novelty, the GH200 production. The
    release builds are portable since the fix after
    1.3.2 (mimalloc for ARMv8.0,
    scripts/check_arm64_portable.py in the arm64 jobs), LSE still chosen at
    run time by GCC's outline atomics. What a -mcpu=neoverse-v2 build might
    add for Grace: little from vectors (its SVE2 is 128 bits, NEON's width),
    atomics inline, scheduling, dot product and fp16 where the compiler
    finds use -- a guess of 5 to 15% on the serial CPU parts (indexing's
    choice of basis, refinement's trial residuals), less overall. The way to
    find out: an MXI_CPU option passing -mcpu, empty by default; one extra
    artifact built so, checked against its own target rather than ARMv8.0;
    both timed with --timing on the GH200; kept as a second artifact only if
    it gains some 10% overall. The same question for x86: a whole-program
    x86-64-v3 build against the SSE4.1 baseline with AVX2 dispatched.
* **96.** **scale -- the calibration's robust columns, where boxes are empty.**
    Noted by Graeme, 15 November: a pathological set, nothing made. (1.4826
    MAD)^2 assumes the deviations vary continuously; where over half a shell's
    boxes hold no photons, profile fitting gives them all one deviation and the
    robust variance collapses -- ins10_1 past 1.8 A, robust 0.03 to 0.7 against
    final mean squares of 0.99 to 1.05, background 0.03 to 0.12 counts a pixel.
    Simulated: 52% of boxes empty, robust 0.000, mean square 1.05. If it comes
    to matter: a trimmed mean square (z^2 without the largest 1% of |z|, over
    its normal expectation, 0.925) resists the wild few and not identical
    values; the table's heading, mxi report's plot and legend, and a test of a
    shell mostly empty. docs/scaling.md, The calibration.
* **95.** **speed -- writing and reading .rflx files.** Parked by Graeme, 15
    November: diminishing returns. Some 7 s of a 63 s run on the two-socket
    Xeon with the L40S, 2.5 s on the M4 Max, all on one thread. Measured
    here: a million-row, forty-column table written in 0.37 s, its columns'
    bitshuffle-LZ4 compression alone 0.12 -- a third. Refinement's writing
    0.82 s for 160000 rows on the Xeon points at something besides the table
    (the scan-varying experiment list's JSON, or the summary). If it ever
    matters: time the parts first; then chunks compressed in parallel and
    written raw (H5Dwrite_chunk, the bytes the filter would write), reading
    likewise. Not: keeping steps' output in memory between them, tried and
    reverted (its revert's commit message, 15 November, says why).
* **94.** **speed -- x86's remaining calls for fmax and fmin.** GCC calls the C
    library for std::fmax and std::fmin -- their NaN rules are not maxsd's --
    where ARM has an instruction: indexing's choice of basis made 1.9 billion
    such calls on a user's data (6 of 11 s on a Xeon), and uses
    `max_finite` now. 122 calls to fmax and 43 to fmin remain in the binary;
    which are hot wants `perf record` on a real data set on an x86 machine
    (Graeme's Ryzen), and each one changed only where its values cannot be
    NaN, mxi's mark of a missing value. 13 November.
* **31.** **The C++ suite is slow**: mxi_tests about five minutes on one core
    here, most of it a handful of older refinement and indexing tests and, since
    October, the spot finder's tests joined to it. ctest runs it as one test.
* **87.** **mxi-ui's window tests do not run in the release builds**
    (`MXI_NO_WINDOW_TESTS`, since v1.1.4-rc1's macOS test step seemed to
    stall): they run where there is a display -- xvfb here, a desktop -- so
    before a release, on a desktop, `MXI_BUILD=build pytest -rs
    python/tests/test_ui_window.py python/tests/test_ui_viewer.py` -- the
    viewer's tests make their data with mxi, and without the build directory
    skip, every one, as on 7 November (`-rs` says why any is skipped).
## Closed

* **89.** **Closed, 12 November: a Windows build, released in 1.3.0.** A zip
    of `mxi.exe` alone and a `win_amd64` wheel with the interface, built by
    `.github/workflows/windows.yml` -- clang-cl, vcpkg's static libraries --
    which the release workflow calls; the C++ suite passing there, the zip's
    DLLs checked to be Windows' own. Graeme ran both on Windows: results
    matching, the progress bars right once the console was put in UTF-8, the
    wheel's mxi-ui and mxi-view working in Python 3.14. What remains is items
    92 (the Python suite on Windows) and 93 (CUDA on Windows).

* **88.** **Closed, 6 November: overlaps fitted together, the default.** On
    ins2_27 the overlapped measurements went from +0.92 sigma from their
    equivalents, rms 3.79, to +0.07 and 1.06 (docs/integration.md); the summed
    intensities corrected as well. What remains of it: 1.4 per cent of them
    still beyond 3 sigma, where a Gaussian has 0.3 -- groups the fit does not
    yet describe, to look at if they matter.

* **81.** **release -- the workflow's actions on Node.js 24.** Closed 3 November,
  for 1.1.3's candidate: actions/checkout@v5, upload-artifact@v6 and
  download-artifact@v7, the first of each to run on Node.js 24 by default (the
  versions before had preliminary support only), per their release notes. The
  release job's download -- path and merge-multiple, every artifact -- is
  unchanged by download-artifact's later versions, whose changes concern one
  artifact downloaded by ID.

* **79.** Decided 21 October, Graeme: **keep both orders of experiments.**
  Joint indexing writes the sweeps, then their lattices after them all (four
  sweeps of one crystal: 0-3, then 4-7); `--no-joint` writes each sweep
  followed by its lattice (sweeps even, lattices odd). Both make sense for
  what each is; every program follows an experiment's own image set, and
  mxeq truth names data sets by their images (`docs/multi-sweep.md`).

Closed in the review of 20 October, against the code and the history:

* **17.** **Normalisation by resolution shells.** mxi symmetry has normalised by an
  anisotropic maximum-likelihood Wilson model, dials.symmetry's ml_aniso, since
  fffaa80 (src/laue.cc), shells only where the fit fails; mxi correlation uses
  the same since 20 October.
* **55.** **Summation 2 to 3 per cent below DIALS's**, understood: the foreground's
  extent, a constant fraction which scaling takes up -- an error only if it
  varies with the geometry, which nothing has shown.
* **48.** **Integration's threads idle while reading.** The box cap was the cause
  (now 6000 a thread); the fill converts a row at a time; boxes left unzeroed
  tried and reverted. What is left, reading bound by memory traffic, is in 11.
* **32.** **The device port designed but not written**, overtaken: profile fitting
  runs on a device (`mxi integrate --gpu`, Metal and CUDA); what remains is in 43
  and 47.
* **Diamond's Eiger masters** (5 October): an offset without offset_units, in
  metres, had been taken as millimetres -- fixed, as nxmx reads it; their count
  limit and bit depth are links into the _meta.h5 beside the master, a link
  that cannot be followed had been taken for absent, and the fallback 65534 was
  itself a marker and a 16-bit limit on 32-bit data -- now the lower of the
  detector's limit and 2^bits - 3, a broken link said so, and 2147483647 with
  neither. And a scan not beginning at image one had every angle wrong by its
  start, fixed with several sweeps. (A note that had been left among the open
  items.)

* **77.** Closed 20 October. **Each sweep's integration said it wrote its
  internal files** ("... to integrated.rflx.sweep0.expt and
  integrated.rflx.sweep0.refl", scratch made and removed inside the program).
  Each run of several -- a sweep's, a lattice's -- is given `--sweep-run`, and
  says "Integrated 31295 reflections"; only the whole's line names a file.
  Test: test_integrate_sweeps.

* **74.** Closed 19 October. **mxi index could not be given a known cell.**
  `--known-cell a,b,c,alpha,beta,gamma` with `--known-space-group NAME`: only
  candidate bases whose reduced cell matches the known one's reduced primitive
  cell (10 per cent, 5 deg, as dials.index) are eligible; none, a refusal that
  says so; a match indexing under half what the best basis does, a WARNING.
  On insulin, 78 A I-centred or 67.4 A primitive: the same basis as without.

* **72.** Closed 17 October, confirmed by Graeme on his 16 data sets. **Data
  sets processed together scaled much worse than processed apart** -- inner
  R_merge 0.125 and b 0.125, against 0.049 and 0.050. `mxi index --no-joint`
  joined its sweeps with identical beams merged into one, 14 of the 16
  sharing a header wavelength, and mxi refine fitted that one beam for all of
  them. Now --no-joint keeps each sweep's beam and detector, and mxi refine
  refines independent experiments apart (`--joint` for one fit). Ruled out on
  the way, each by an identical comparison: the import, integration (one file,
  two files, two detectors), symmetry and scaling. Also fixed on the way: a
  list's images were its document's first image set's, so a split or combined
  file integrated one data set from another's images.

* **71.** Closed 17 October. **mxi scale's scale wandered, through zero, where
  the data did not hold it, and said nothing.** Reproduced on 7.5 degree wedges
  of insulin scaled alone: one diverged (scale -0.71 to 0.41, R_meas 1.9e6).
  Each sweep's scale is now tied weakly to its own level (`--scale-restraint`,
  1), and a scale at or below zero is a WARNING (docs/scaling.md, "The scale
  tied to its level"). Left open, and not the tie's: another wedge's scale
  varies 0.52 to 1.25 whatever the tie -- held by its data -- and is better on
  profile-fitted intensities alone (0.77 to 1.13), a question for the
  profile-summation mix.

* **30.** Closed 16 October. **What was still duplicated between the spot finder
  and the rest.** Integrated in three commits: the tree and the build one, the
  spot finder's table, experiment list, arguments, HDF5 handles and namespaces
  the library's, the GPU code's device set-up one layer for Metal and one for
  CUDA, its tests in `mxi_tests` -- the spots byte for byte throughout.
* **33.** Closed 16 October. **Three documents described the spot finder.** Now
  `docs/spots.md`, the reference, and `docs/spots_notes.md`, the notes for
  working on it; how it came in is the history's.

* **70.** Closed 15 October. **mxi symmetry takes its input's cell as
  primitive.** Input already in a centred setting is now taken to its
  primitive cell first -- the centring from the crystal's Hall symbol, the
  standard change of basis for I, F, C, A, B and R, any reflection the
  centring forbids dropped and counted, and said. Insulin symmetrised in I 2 3
  and a cluster of Graeme's 36 crystals both come back I 2 3 (P 2 3 before);
  integrated input unchanged. Test: tests/test_symmetry_centred_input.py,
  failing on the old build.

* **26.** The scale model written into the scaled experiments, mxi_physical
    (`docs/scaling.md`, "The scale model, written").

* **64.** The error model's form: split, a background's variance and a
    signal's each their own a (`docs/scaling.md`, "The error model, split").
* **65.** Merged sigmas at high multiplicity: the old fit's doing. With the fit
    by regression the ten-turn insulin's random halves are 1.034, its slope
    1.133 the signal above them.

* **13.** e.s.d.s for the cell: from two-theta in mxi scale, as
    dials.two_theta_refine (`docs/scaling.md`, "The cell from two-theta");
    refinement's covariance e.s.d.s, which came first, are gone.
* **3.** sigma_m too wide on insulin: not so. At 0.086 rather than 0.128
    degrees insulin merged worse, R merge 0.035 to 0.038 (October); the
    partials' small excess, item 1, is a few dozen observations at a scan's
    ends.

* **61.** Strong reflections on a near-empty background: the fit's weights
    on 0.05 again, its variance the fit's own on 0.001 (`docs/integration.md`,
    "The weights apart from the variance"). Graeme's small-molecule set as it
    was: R merge 0.037, 0.029 at low resolution.

* **27.** mxi report: an HTML report of the processing a .rflx records, each
    step's figures, tables and plots, after dials.scale's (`docs/output.md`).

* **21.** One sweep only, in scaling: several sweeps, and several crystals,
    each a model of its own (`docs/multi-sweep.md`, `docs/multi-lattice.md`).

* **60.** Merging's weights from each observation's own noise: each variance
    now from the model's intensity, on integration's variance with no
    signal. The weighted means' bias on the two crystals -2.4 to within 0.1
    merged sigma. `docs/scaling.md`, "Variances from the model".

* **56.** `--gpu` with several lattices: a fit batch a lattice, each on its
    own profile grid. Emulated, one pass the same as each lattice alone, bit
    for bit, and as many fitted as the CPU. `docs/gpu.md`.

* **57.** Sweeps carried through (dials/dials#1029): each sweep's own
    experiment, crystal-less, carried through every program by default, its
    unindexed spots its own; `mxi index --no-keep-sweeps` the lattices alone,
    `mxi convert` the pair without them (the default since 14 October). A missed lattice
    recovered beside refined ones. `docs/multi-lattice.md`, stage 2.

* **16.** Several crystals on one side of the indexing ambiguity: cosym in
    `mxi symmetry`, the same coset as dials.cosym on two crystals of insulin.
    `docs/symmetry.md`, `docs/multi-lattice.md`.

* **25.** dials.merge on `scaled.refl`: it takes it, run by Graeme.
* **38.** Integration decompressing every frame twice: one pass over the
  images now, each reflection fitted once its scan blocks are final, the table
  byte-identical to two passes'. A sparse profile cell borrows its own scan
  block's average rather than the whole scan's, which one pass needs, and a
  scan block is 10 degrees by default. `docs/integration.md`.
* **39.** Writing a reflection table at about a second a million rows: a
  column is copied, not appended a byte at a time, and written straight to the
  file rather than built into one string first. 630930 rows, 261 MB: 0.875 s to
  0.14-0.19, of which the disk is 0.06; reading 0.29 to 0.24. Every table
  byte-identical. `src/refl.cc`.
* **36.** Prediction slow on some sweeps and not others: it enumerated the
  resolution sphere to the Ewald limit, 0.48 A, where the detector reached 1.58,
  and converged every crossing over a full turn before asking whether the scan
  reached it -- a 30 degree sweep paid for 360, and a scan-varying crystal paid
  most. Both are pruned first now, with a margin for the crystal's motion
  bounded from its settings: 8.1 s to 0.09 on the 300 image sweep, the same
  predictions, integrated.refl byte-identical. `src/predict.cc`.
* **45.** Scaling's error model and gathering most of scaling, threads or not:
  a persistent thread pool, the error model's deviations summed once a fit and
  b by Brent's method, and gathering's column lookups out of its loop, its
  symmetry in parallel and its groups by hash. On the MacBook, 16M sweep,
  --d-min-auto: 5.3 s to 2.9 before gathering was changed, and the chain from
  images to scaled data 48 s to 43. `docs/scaling.md`.
* **42.** One pass over the images, measured on the 16M sweep on a MacBook:
  integration 19.4 s against two passes' 28.4, decompressing 41 thread-seconds
  against 80, at most 79584 boxes held and 2.66 GB (estimated at 49000 and 1.1).
  `docs/integration.md`.
* **46.** Reading frames outside HDF5's lock: **tried and removed.** On 32
  threads, integrating 1800 frames, fetching through HDF5 was 43.6
  thread-seconds, 13 times the wall. Reading each chunk with pread, its place
  from H5Dget_chunk_info_by_coord, gave the same bytes and every table
  byte-identical -- and made a MacBook's integration from an external drive 10 s
  slower, 19.7 to 30.0 s, its user time barely changed: waiting, perhaps sixteen
  threads defeating the drive's read-ahead. Reading through HDF5 was fast enough
  there, and a second path to keep was not worth it; removed (commits 0619381
  and c7e1879, reverted).
* **41.** `mxi scale --threads`, measured: on the MacBook's 16M sweep with
  `--d-min-auto`, 5.3 s, then 2.9 once the thread pool, the error model's search
  and gathering were fixed (item 45). Byte-identical on any count.
* **24.** An anomalous option in scaling: `mxi scale --anomalous` scales an
  acentric reflection's Friedel mates as separate groups. Merged, a +-10 per
  cent anomalous difference inflated the error model's a from 1.36 to 3.24 and
  had a fifth of the observations rejected as outliers. `docs/scaling.md`.
* **37.** `mxi symmetry` on dense data: on ferritin with 32 threads, 17 s on one
  core to 7.0 s, its elements scored and its subgroups named in parallel, mates
  found by hash (and item 40 with it). Byte-identical. `docs/symmetry.md`.
* **50.** The integrator filled a bad pixel (0xfffe in 16 bits, 0xfffffffe in
  32) as a count of 65534: only the tile join's marker, max(), was taken for one,
  where the spot finder masks both. Fixed 2 October 2026 (`src/fill_row.hh`);
  insulin, with no bad pixels, byte-identical. Data with them -- ferritin's, it
  may be -- integrate differently: its few far-high summed observations (a mean
  of +9.6 per cent against a median of +0.5) are worth rerunning
  `mxeq equivalents` on. Overloads are still not masked: the trusted range is
  not applied in integration.
* **51.** **correctness -- A third of a data set not profile fitted.** On
  Graeme's 8BXT (2.8 million reflections to 1.18 A, sigma_m 0.047 degrees,
  Metal), summation 2.56 million and profile fitting 1.87 million -- 67 per cent,
  the same at low resolution as high -- and scaling, taking profile-fitted
  observations only, merged 1.39 million against DIALS's 1.91. Reference
  profiles were learned from only 12129 reflections, 216 of 324 regions
  borrowing. Not reproduced here: insulin loses 4.8 per cent, the same on the
  device's path and with sigma_m forced to 0.047. The integrator now says why
  each reflection was not fitted; that run on 8BXT is the next step.
  The report, run on 8BXT: 900607 'nothing of the profile on its measured
  foreground' and 31761 under 0.60, the same on the CPU and Metal. That reason
  is now split -- the profile zero over the whole foreground (code 4), or none
  of the foreground measured (code 5) -- each reflection's reason written as
  profile.failure, and `mxeq failures` bins them. Not the cause, tried here:
  sigma_m or sigma_b forced small on insulin, the device's path, phi wrapping,
  empty scan blocks in one pass.
* **52.** **correctness -- DIALS's inner shell a shade better in CC1/2 and
  I/sigma**, on ferritin and before; `mxi find --subtract-background` made it
  slightly worse there, the RMSDs slightly better. `mxeq unique` compares the two
  scaled data sets one unique reflection at a time, splitting I/sigma into
  intensity, multiplicity and per-observation sigma, and comparing the
  observations' scatter, which CC1/2 follows. A suspect: overloads. mxi's
  integrator does not apply the trusted range, so the strongest reflections --
  the inner shell's -- are integrated with saturated pixels and kept; DIALS
  flags them overloaded, and the tool reports how many DIALS's table flags and
  scaling used. Insulin's own inner shell has the lower CC1/2 of its first two
  (0.983 against 0.997 over five shells).
  On ferritin, DIALS against mxi (`mxeq unique`, ten shells): overall scales
  18 per cent apart, which is arbitrary; scale removed, the observations
  scatter about as much in each. Outside 2.2 A mxi's CC1/2 is the higher (1.46
  to 1.51 A: 0.397 against 0.328), and DIALS's sigmas are some 2.3 times its
  own scatter there (chi2/nu 0.19, mxi's 0.83 to 1.1), so its I/sigma reads low
  -- if its stored variance is the one dials.merge uses. In the inner shell
  DIALS's CC1/2 is the higher (0.9998 against 0.9992) and the two data sets'
  merged intensities correlate at only 0.971, against 0.99 and more elsewhere:
  a minority of the strongest reflections differ greatly. Neither table flags
  overloads; but DIALS masks pixels over the trusted range and profile fits
  around them, where mxi counts them at their capped value. The suspect, and
  the fix: the trusted range applied in integration.
  Not the trusted range after all: the inner shell's far-apart reflections are
  the very lowest-resolution ones, near the beam. On ferritin their observations
  pair across the beam: those on one side integrate well (backgrounds 4 to 57),
  those on the other read 0 with no valid pixels (masked, and left out); but
  some beside the beam, in the backstop's shadow, have valid pixels that see
  almost nothing (backgrounds 0.28) and integrate to about zero -- fitted,
  scaled, and dragging the merged mean down: (2,2,2) 144 against DIALS's 2812.
  Reproduced here: insulin's (1,1,0) is measured once, in the shadow, and
  scaled at -0.04. `mxeq observations` lists a reflection's observations.
  And it is not only integration's. `mxeq observations` on the scaled table,
(3,1,1): of the 27 observations outside the module gap, 13 at a scaled 934 to
1078, 7 at 146 to 588, 5 at 23 to 99 -- all flagged outliers -- and two at 10.5
and 10.7 kept, whose mean merged. Intensity over background is some 40 for
every one: the shadow's edge attenuates signal and background alike, so a
reflection's background against that expected at its radius is its
transmission. Scaling's rejection, a 1/sigma^2-weighted mean of the others,
sinks towards the attenuated, whose sigmas are small (item 53). `--d-max D`, in
mxi integrate and mxi scale, leaves the reflections out to test it: on insulin,
8 beyond 30 A, the inner shell's CC1/2 0.977 to 0.979. The plan is
`docs/backstop.md`.
  The background's dispersion, Graeme's metric, measured on ferritin: it does
  not find the attenuation. (3,1,1)'s unharmed observations, in the flare, have
  the highest (3.5 to 5.4); the attenuated lower (1.3 to 2.5) -- the shadow's
  edge is wider than a box, so each attenuated box is a dimmed copy of an
  unharmed one, and nothing inside it can tell. What can: a user's mask of the
  backstop, `--d-max`, or in scaling the physics that attenuation only lowers
  an intensity (`docs/backstop.md`, "What can, then").
  **Paused, 5 October 2026** (`docs/backstop.md`, "Where it stands"): the cause
  found and tools built to find and filter it, but on ferritin the filters
  raised Rmerge slightly and DIALS's and mxi's data refined about the same
  before any of it (mxi very slightly better, not significantly). No absolute
  measure of processing quality yet decides it; none of the filters is a
  default.
* **53.** **correctness -- Scaling's outlier rejection outvoted by small
  sigmas.** Each observation is compared with its group's mean weighted by
  1/sigma^2, so a few observations with tiny sigmas -- shadowed by a backstop,
  integrating to about nothing -- outweigh many consistent strong ones, which
  are then rejected one by one: on ferritin's (3,1,1), 25 rejected, the 13 at
  some 1000 among them, and two near 10 kept (item 52). Whatever makes small, wrong
  observations triggers it. Wanted: a test the many consistent cannot lose to
  the few -- a median's, or a weight that a near-zero observation's tiny
  variance cannot make overwhelming. The plan is `docs/backstop.md`, part 2.
  Prototyped in `mxeq attenuation`, attenuation judged from the equivalents and
  the background together; paused with item 52.
* **23.** A resolution estimate: dials.estimate_resolution's tanh fit through
  CC half, at 0.3, and its significance limit, with the "Suggested" column.
  `docs/scaling.md`.
* **29.** The Metal backend of the spot finder: it works. 1800 images of 3108 x
  3262 pixels thresholded in 3.6 seconds, 502 a second, on a Mac with 16
  threads; 146118 spots. `docs/spots.md`.

## Found in DIALS

Not work here, but worth taking upstream, each measured and in the document
named.

* **dials.scale's error model is biased low** by (n-1)/n: its normalised
  deviations are multiplied by sqrt((n-1)/n) (`calc_deltahl`) where the exact
  variance of a difference from a mean the observation is part of is needed. A
  planted a of 1.3 comes back 1.158 in groups of eight and 0.662 in pairs.
  `docs/scaling.md`.
* **Its anomalous slope and dI/s(dI) inherit that bias**: 1.890 and 1.476 on a
  sweep with no measurable anomalous signal, where the same data give 0.955 and
  0.787 with the exact variance. `docs/scaling.md`.
* **dials.scale's scale-factor variance is not a propagation**: the variance is
  multiplied by 1 + sigma_g / g after the error model, rather than
  I^2 var(g) / g^2 added before it. `docs/scaling.md`.
* **Refinement against the spot finder's centres leaves a z offset** that
  depends on strength and on orientation -- about 0.1 images, period 180
  degrees -- because those centres depend on strength. DIALS' integration shows
  it too. `docs/integration.md` and `docs/integration_history.md`.

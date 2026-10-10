# Scaling

STATUS: `mxi_scale` scales one sweep, and several of one crystal together
(5 October 2026). Not yet: free-set validation. `docs/outstanding.md` has the
whole list.

**Several sweeps** each have a model of their own -- scale, decay and
absorption from each sweep's own width of rotation, as dials.scale's physical
model has one a dataset -- sharing the merged intensities; an observation's id
chooses its sweep's parameters. The one overall scale and the one overall B the
merged intensities can absorb are fixed over every sweep's points together, not
each sweep's apart, so the sweeps' relative scale is kept: it is what scaling
them together is for. The report gives each sweep's scale and relative B. A
sweep with no observations is refused (`docs/multi-sweep.md`).

`mxi_scale` puts every observation of a sweep on one scale, refines an error
model, and reports merging statistics -- the physical model of Beilsten-Edmands
et al. (2020), Acta Cryst. D76, 385-399, with cubic B-splines for the scale and
decay and the paper's spherical harmonics for the absorption surface.

## Running it

    mxi_scale symmetrized.rflx      # scaled.rflx

or from and to the DIALS pair, as `docs/rflx.md` describes:

    mxi_scale symmetrized.expt symmetrized.refl -o scaled.refl --output-expt scaled.expt

The space group is the crystal's, as `mxi_symmetry` wrote it (`docs/symmetry.md`).
`--space-group` and `--change-of-basis` override it for data that have not been
through `mxi_symmetry`; `--d-min`, `--d-max` (a low resolution limit: beside a
backstop, see `docs/backstop.md`), `--no-absorption`, `--profile-only` and
`--shells N` (20) do what they say. `mxi_scale --help` is the authority.

## The model

Inverse scale g = C(r) . T(t, d) . S(s0, s1), as the paper's physical model.

* **Scale C(phi).** A cubic B-spline in rotation angle, where DIALS uses a
  Gaussian-weighted average of the nearest three parameters. Control points 10
  to 20 degrees apart, as the paper finds, and fewer on a narrow sweep: DIALS
  used six on 30 degrees, and most were poorly determined. It is the crystal model's
  clamped uniform B-spline, `spline_weights` in `src/derivatives.cc`.
* **Decay T = exp(B(t) / 2d^2).** B(t) a cubic B-spline in t, image number for
  now as a stand-in for dose, with DIALS' weak restraint of sum B_i^2 toward
  zero. The zero-dose work would belong here: t as dose, and the extrapolation a
  use of the fitted B(t).
* **Absorption S = 1 + sum P_lm [Y_lm(s1) + Y_lm(s0)] / 2** in the crystal frame,
  as the paper's equation 7: l_max = 4 by default, 24 parameters, odd terms
  included since they absorb miscentring, with a restraint of w sum P_lm^2 toward
  zero. `--absorption-level` chooses the degree and w as dials.scale's does: low,
  the default, for about 1 per cent relative absorption -- degree 4, w 5e5;
  medium, about 5 per cent -- 6, 5e4; high, over 25 per cent, long wavelengths or
  heavy absorbers -- 6, 5e3. (w was 1000 until 7 October 2026, 500 times weaker
  than dials.scale's default, for no recorded reason. Forced on the 30 degree
  sweep with `--l-max 4`, the weaker the restraint the better the internal
  statistics -- I/sigma 20.2 at 1000, 19.8 at 5e3, 19.1 at 5e5 -- as more freedom
  always gives; which is right is a question for a wide sweep and an absolute
  measure.) Off for narrow sweeps -- below 60 degrees -- as DIALS turns it off,
  since there is not the angular coverage to determine it. `--l-max L` sets the
  degree, dials.scale's `lmax`: L (L + 2) terms, 48 at 6; 0 for none; asked
  for, it is used whatever the sweep's width, and it contradicts
  `--no-absorption`. More terms fit better and can fit noise: on the 30 degree
  sweep degree 4 and 6 both take Rmeas from 0.041 to 0.039, and degree 6 the
  error model's a to 0.90 -- the signature of a model absorbing noise, which a
  narrow sweep cannot rule out. Free-set validation, not yet built (item 22), is
  what would say which degree a sweep supports.

One overall scale is degenerate with the merged intensities and must be fixed:
the mean of C, or one control point.

### Fitting

The target is the paper's equation 2, sum w (I - g <I>)^2 plus restraints,
with <I_h> in closed form (equation 3) for the current g. At about 70
parameters the normal matrix is tiny, so full-matrix Levenberg-Marquardt with
analytic derivatives runs throughout, where DIALS uses L-BFGS until the last
cycle. The Jacobian is the variable-projection one, in Kaufman's form, which
lets <I_h> follow the parameters; holding <I_h> fixed gives the right gradient
but too much curvature, and fits of fifty steps ended still creeping. The
cost is building J^T J, linear in observations; its inversion gives the
parameters' uncertainties, propagated to the scale factors' variances (below).

Around the fit, as the paper's figure 2:

1. **Reflection selection.** A random subset of symmetry-equivalent groups, at
   least 2000 groups and 50000 reflections (section 4.4), for optimisation; the
   model is applied to everything.
2. **Outlier rejection.** A reflection's normalised deviation from the weighted
   mean of its group, excluding itself, above 6 sigma (Evans 2006). Repeated as
   the model improves, retesting everything.
3. **Intensity combination.** I = w I_prf + (1 - w) I_sum with w = 1 / (1 +
   (I_sum / I_mid)^3), I_mid chosen from logarithmically spaced values by Rmeas.
4. **Error model.** sigma'^2 = a^2 [sigma^2 + (b I)^2]. a from the slope of the
   central normal probability plot (|x| < 1.5) of the normalised deviations; b
   by minimising the paper's equation 17 over logarithmically spaced intensity
   bins with a fixed; alternated to convergence from a = 1, b = 0.02, on groups
   with <I> above 25 and <I / sigma^2> above 0.85. The deviations take the EXACT
   variance of a difference from a mean the observation is part of, not the
   paper's eqn 12 prefactor, and sigma^2 already carries the scale's uncertainty
   (both below).

Not yet built: **free-set validation**, Rmeas on a tenth of the groups held out,
the check for overfitting. dials.scale warns, on the 300 image sweep, that 64
per cent of its eleven parameters have sigma over half their value.

## What it writes

A scaled, unmerged reflection table with dials.scale's columns --
`inverse_scale_factor` and its variance, `intensity.scale.value` and variance --
and its flags (scaled, bit 26; an outlier in scaling, bit 23; excluded, bit 24)
ADDED to those integration set, and dials.merge takes it -- as the DIALS pair,
written by `-o scaled.refl --output-expt scaled.expt` or from `scaled.rflx` by
`mxi_convert scaled.rflx`.
The intensity's variance already carries the scale's uncertainty, as dials.scale's
does, so nothing downstream should add `inverse_scale_factor_variance` to it
again. The models, reindexed, go with it, into `scaled.rflx` (`scaled.expt` in the
pair). It prints the model, the error
model, merging statistics in twenty shells and dials.scale's summary. HTML
reports are for a later mxi_report, MTZ and mmCIF for a later mxi_export.

## Against dials.scale, on the 300 image sweep

Integrated here and scaled here, against dials.scale on DIALS' own integration,
overall:

    ours   20007 obs  7183 uniq  mult 2.79  71.4 %  <I> 131.7  <I/sI> 18.9
           Rmerge 0.034  Rmeas 0.041  Rpim 0.022  CC half 0.987
    DIALS  19737 obs  7268 uniq  mult 2.72  71.0 %  <I> 130.0  <I/sI> 31.4
           Rmerge 0.038  Rmeas 0.045  Rpim 0.024  CC half 0.999

* **The error model.** a = 1.017, b = 0.024 here; dials.scale reported 0.531 and
  0.0246. dials.scale normalises its deviations with eqn 12's sqrt((n-1)/n)
  (`calc_deltahl`), which leaves their spread at (n-1)/n and a low by that
  factor -- a planted 1.3 comes back 1.158 in groups of eight and 0.662 in
  pairs -- and this sweep's multiplicity is 2.7. The deviations here take the
  exact variance of a difference from a mean the observation is part of.
  That one factor accounts for most of the difference in <I/sI>: 18.9 scaled by
  1.017 / 0.531 is about 36.
* **The fit.** Levenberg-Marquardt with the variable-projection Jacobian in
  Kaufman's form converges in three or four steps. Holding <I> fixed instead --
  right gradient, curvature overstated -- left three fits of fifty steps each
  still creeping. With the curvature right, a constant offset in B, degenerate
  with the merged intensities but for small differences of d within a group,
  wandered to -2 A^2; B is now centred on its mean, as the scale is on its.
* **CC half in the lowest shell,** 0.975 against 0.999: a few groups, and
  without the three widest 0.9990. What they share is two things scaling has
  found in the integration, both judged by symmetry equivalents: observations
  with partiality below 0.9 are 10.5 per cent high, as are, by 4 to 5 per cent,
  those in the first two and last five images -- partials over-corrected -- and
  those with less than 95 per cent of the profile measured, near a module gap,
  3.4 per cent low. These are integration's to fix.

## The scale's uncertainty, before the error model

The covariance of the parameters is the inverse of the normal matrix of the
variable-projection Jacobian and the restraints, under the two constraints the
model's normalisation fixes -- the scale's mean at one, the relative B's at
zero -- and scaled by the goodness of fit, whose degrees of freedom count every
merged intensity as a parameter. Tested against the thing it claims: the
geometry fixed and the noise drawn forty times, the spread of the fitted g at
five places across the scan over the variance predicted is 1.023, each between
0.89 and 1.19.

Each observation's var(g) goes into its variance as I^2 var(g) / g^2 BEFORE the
error model is refined and applied, so that a and b correct what remains after
it. dials.scale does the reverse, and applies it as a factor (1 + sigma_g / g)
on the variance after the error model: linear in the fractional error, and the
same at every intensity. On the 300 image sweep the scale is well determined --
sigma(g)/g 0.22 per cent on average, 0.68 at most, three times as large at the
ends of the scan -- and b moves only from 0.0237 to 0.0236: the term goes as
I^2, as (b I)^2 does, and a b fitted without it spreads the uncertainty of the
worst-determined parts of a scan over every observation.

## The summary, and what the error model does to it

mxi_scale ends with dials.scale's summary -- overall, lowest shell, highest --
with iotbx.merging_statistics' definitions, which dials.scale reports: R factors
with Friedel mates merged and apart, anomalous completeness against the complete
acentric set, anomalous multiplicity over the groups with mates apart, the
correlation of I+ - I- between random halves, the slope of the normal
probability plot of dI / sigma(dI) over |x| < 0.9, dF/F as
sqrt(2 <(F+ - F-)^2> / <F+^2 + F-^2>), and dI/s(dI) as mean |dI| over mean
sigma(dI). On the 300 image sweep, overall:

                              here     dials.scale
    Completeness              71.4     71.0
    Rmerge(I), (I+/-)         0.034    0.038, 0.032
                              0.031
    Rpim(I), (I+/-)           0.022    0.024, 0.029
                              0.027
    Anomalous completeness    51.2     50.2
    Anomalous multiplicity    1.7      1.7
    Anomalous slope           0.955    1.890
    dI/s(dI)                  0.787    1.476

What depends on the intensities alone agrees. The two that differ are ratios to
sigma, and dials.scale's sigmas are shrunk by its a of 0.531: ours scaled by
1.017 / 0.531 are 1.83 and 1.51. An anomalous slope of 1.89 reads as anomalous
signal; this sweep's is 0.955 -- none measurable, as insulin's sulphur at
0.95 A would lead one to expect -- and the 1.89 is the error model's bias.

## The resolution limit

As `dials.estimate_resolution` finds it, read from its source
(`dials/util/resolution_analysis.py`): CC half in 50 bins of equal count of
unique reflections, Wilson outliers (E^2 of 16 or more) left out; a tanh in
d*^2, CC half = (1 - tanh((d*^2 - s0) / r)) / 2, fitted by weighted least
squares with sigma 1/sqrt(n - 3) from r = 0.2 and s0 = 0.4; and the limit where
the fitted values cross 0.3. The second limit is where CC half stops being
significant: a bin is significant where CC half exceeds t / sqrt(n - 2 + t^2),
t Student's at the upper 0.1 on n - 2 degrees of freedom, and a logistic
1 - expit(r (d*^2 - res)) through which bins are gives 1/sqrt(res). The summary
gains dials.scale's "Suggested" column: the same statistics cut at the CC half
limit -- unless `--d-min` was given, when the cut is already chosen and the
column is left out; the estimate itself is still printed.

`--d-min-auto` makes the cut itself: everything is scaled once, the limit is
estimated, and the sweep is scaled again to it -- again, not filtered, since the
outliers, the error model and the fit all depend on what is in. The limit is
rounded to a hundredth of an angstrom and reported as the `--d-min` it is
equivalent to, and the table is that run's, byte for byte
(`python/tests/test_d_min_auto.py`). It cuts only if the fitted CC half comes down
to the limit within the data: where it never does, as on the 300 image sweep at
0.3, the estimate is only the last bin with pairs, a cut there would drop good
data, and nothing is applied. `--cc-half-limit C` sets the CC half both the
estimate and the cut use (0.3); `--d-min` and `--d-min-auto` together are
refused.

One difference, deliberately: only bins with pairs enough to fit (n > 3) decide
whether CC half is above the limit everywhere. A detector's corners hold
reflections seen once, with no halves and a CC half of zero, and under
dials.estimate_resolution's rule a sweep whose CC half stays near one to its
last bin was given no limit at all. On the 300 image sweep that is the case: the
limit is the last bin with pairs, 1.73 A, where the data are 83.5 per cent
complete with CC half 0.987, and dials.scale suggested 1.63, the edge. The
significance limit is 1.68.

On a sweep of 1082994 reflections, dials.estimate_resolution run on this
program's own scaled.refl and this program agree exactly: 1.25 A by CC half and
1.17 by significance.

## Threads

`--threads N` (the machine's by default). The fits' target and normal
equations, outlier rejection, the error model's deviations, the scales'
uncertainties and the absorption harmonics run in parallel; the merging
statistics, whose random half-splits depend on order, do not. The work is cut
into 64 blocks whatever the count and combined in block order, so the answer is
the same on any number of threads: scaled.refl is byte-identical on one and four,
and a test holds a fit, outlier rejection, the error model and the covariance to
the bit across counts, failing if the blocks follow the threads.

Only the normal equations' order of summation differs from the serial version's,
by rounding: on the 300 image sweep the scale factors move by 3.4e-11 relative
and their variances by 3.7e-9, the flags and the error model not at all. The
error model's deviations are computed a group at a time, each group's spread in
its own order, and are bit for bit what one thread gives; the normal quantiles
of its probability plot are computed once rather than every round.

## The error model's search, made cheaper

The error model was most of scaling even with threads: 1.35 to 1.49 s of each
pass on the MacBook with 16, against 0.19 to 0.22 for the three fits. Three
changes, the answer moving only in its last digits. The thread pool keeps its
workers rather than making them every call (`src/parallel.cc`). A deviation's
variance is a^2 (P + b^2 Q) with P and Q summed once a fit -- the weights and the
merged means do not change within it -- so an evaluation is a square root an
observation, where every group's spread was summed again. And b is found by
Brent's method to 1e-8, where a golden section of sixty steps went to 1e-13 and
the rounds stop at a change of 1e-6. On one thread on the 300 image sweep the
three error models went from 0.61 s to 0.14; the error model (a 1.0092, b
0.0244), Rmerge, Rmeas, Rpim, CC half, I/sigma, completeness and the anomalous
correlation are the same to every digit printed.

On the MacBook, the 16M sweep with --d-min-auto: 5.3 s to 2.9, and the chain
from images to scaled data 48 s to 43. Gathering the observations was then the
largest phase, 0.40 to 0.45 s a pass, serial: eight lookups of a column by name
a reflection, the group's symmetry operators three times a row, and an ordered
map. The columns are looked up once, a row's symmetry is worked out in parallel,
and the groups numbered in row order through a hash -- scaled.refl
byte-identical, and a third of the time on one thread.

## Friedel mates apart, --anomalous

By default an acentric reflection's I(+) and I(-) are one group, as they are
in dials.scale. With a strong anomalous signal they genuinely differ, and the
difference is then scaling's to explain -- which it does in the worst way.
Planted, with noise at a = 1.3, b = 0.03, four observations of each mate and
3000 reflections:

| Friedel difference | merged: a | merged: outliers | apart: a | apart: outliers |
| --- | --- | --- | --- | --- |
| none | 1.35 | 397 | 1.36 | 338 |
| +-5 % | 1.63 | 1688 | 1.37 | 334 |
| +-10 % | 3.24 | 4509 | 1.36 | 323 |
| +-20 % | 9.80 | 8102 | 1.36 | 330 |

of 24000 observations. Merged, the error model takes the difference as noise --
through a, not b, every sigma inflated -- and outlier rejection throws away the
observations with the largest anomalous differences: a fifth of them at +-10 per
cent. Kept apart, the planted model comes back whatever the signal.

`--anomalous` makes an acentric reflection's I(-) a group of its own, so
scaling, the error model and outlier rejection all see the mates separately,
as dials.scale's `anomalous=True`. Centric reflections stay one group:
`friedel_plus` calls every centric reflection I(+), their mates equal by
symmetry. The merging statistics and the resolution estimate keep the Friedel
pairs -- `ScaleData::pair`, numbered as the groups are without the flag -- since
they split each pair into I(+) and I(-) themselves: the merging table reports the
same reflections either way, its anomalous columns what such data wants read.
The report says how many groups were scaled from how many reflections. Without
the flag, every table is byte-identical to before it.

## Decisions

* **gemmi** for space groups, pinned at a release as a submodule, behind
  `src/symmetry.hh`; only its symmetry source is compiled.
* **Reports** of merging statistics go in a later mxi_report; **MTZ and
  mmCIF** in a later mxi_export. `mxi_scale` prints its tables, as the other
  programs do.

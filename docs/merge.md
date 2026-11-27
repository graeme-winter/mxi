# mxi merge: a merged MTZ

`mxi merge scaled.rflx` writes `merged.mtz`, as `dials.merge` writes one, from
mxi scale's output. Wanted (Graeme, 26 November) for the solvent analysis of
the A71EV2AZ crystals -- one merged file a crystal, against a model -- and in
general, for refinement.

Read from DIALS' source the same day: `dials/command_line/merge.py`,
`dials/algorithms/merging/merge.py` and `french_wilson.py`, and cctbx's
`french_wilson.py` for its tables.

## What it writes

The columns, in dials.merge's order: `FreeR_flag`; `IMEAN SIGIMEAN` and `N`,
the multiplicity; `F SIGF`; with Friedel mates apart (the default, as
dials.merge's `anomalous = True`) `I(+) SIGI(+) I(-) SIGI(-)`, `N(+) N(-)`,
`F(+) SIGF(+) F(-) SIGF(-)` and `DANO SIGDANO`. `--no-anomalous` leaves the
anomalous columns out, `--no-truncate` the amplitudes, `--free-fraction 0`
the flags.

The observations: as dials' `filter_reflection_table` with the scaled
intensities -- not an outlier or excluded, a positive inverse scale factor and
variance, partiality at least 0.4 -- within `--d-min` and `--d-max`. The
scaled intensity is `intensity.scale.value` over `inverse_scale_factor`, as in
DIALS: the column holds the corrected intensity before scaling.

The merge: cctbx's `merge_equivalents` with `use_internal_variance = False`,
dials.merge's choice -- the inverse-variance mean, sigma 1/sqrt(sum w) --
into gemmi's (CCP4's) asymmetric unit. Systematic absences are kept, as
dials.merge keeps them. A centric reflection's anomalous columns are I(+)
alone, as dials.merge's `match_bijvoet_mates` puts singles there. The cell is
the median of each parameter over the crystals merged (dials.merge's
`determine_best_unit_cell`).

## The merging statistics

Each merge prints the summary of the merging statistics -- Overall, Low and
High, the anomalous figures, the totals -- as mxi scale prints it, by the same
code (merging_statistics, print_merging_summary) from the same observations:
build_scale_data's, each with the intensity, variance and inverse scale factor
scaling wrote, the rows scaling excluded left out -- beyond its resolution
limit, say, which never entered its data -- and its outliers kept and marked, as
they were in scaling. So the summary is scaling's, line for line, CC half's
random halves included; only scaling's Suggested column, where it suggests a
cut, has no place here. Insulin unmerged and 9uoa in P 41 21 2 scaled to 1.76 A
both give mxi scale's summary to the digit (Graeme, 26 November: since we are
merging it should at least reproduce the merging statistics -- this table, not
the long one). The figure "left out" in the line above it counts the same rows:
on 9uoa, 83,145, those beyond 1.76 A and the outliers. --shells N as mxi
scale's, which sets the Low and High shells. Within --d-min and --d-max where
they are given, by the cell the merge uses: the summary is of what was merged,
its totals the counts on the line above it (27 November: it had gone on to every
row scaling had).

## Amplitudes

French and Wilson (1978) as dials.merge's own implementation: the expected
intensity the standardised median (median over ln 2) in bins of d*^3 of equal
count -- at most 60, at least 20 reflections a bin -- interpolated linearly at
each reflection; h = I/sigma - |sigma / Sigma| (acentric) or
|sigma / (2 Sigma)| (centric); the posterior moments interpolated in cctbx's
tables below h of 3 (acentric) or 4 (centric), `src/french_wilson_tables.hh`
generated from cctbx's file; above, sqrt(h sigma) (acentric) or French and
Wilson's expansion with the x^6 term (centric). Refused where I/sigma is below
-3.7 or h below -4. With Friedel mates apart, French and Wilson is taken to
I(+) and I(-), F is the mean of F(+) and F(-) by their sigmas, and DANO is
F(+) - F(-) with sigma sqrt(sigma+^2 + sigma-^2); without, it is taken to
IMEAN -- both as dials.merge's `truncate`.

Merged far beyond the data's limit, many weak reflections are refused (h
below -4 where a shell's median intensity is near nought): 9uoa merged to
1.51 A, its limit about 1.76, lost 8,278 anomalous entries so.

## FreeR flags -- where it departs from dials.merge

CCP4's convention, as dials.merge writes them (`format = "ccp4"`): 0 to
1/fraction - 1, 0 the test set, a twentieth by default. But not cctbx's random
draw: each reflection's flag is a hash of its index taken in a canonical form
under the lattice's rotations and inversion. So reflections a twin law would
relate share a flag, as cctbx's `use_lattice_symmetry`; and the same index has
the same flag in every data set of one lattice -- the crystals of a series,
merged one by one, share one free set. There is no cap of 2,000 free
reflections (cctbx's `max_free`).

## Each crystal alone

`--each` merges each crystal alone, `merged_0.mtz`, `merged_1.mtz` ... (or
the stem of `-o`), each with its own median cell -- the crystals' scaled
intensities, on the one scale mxi scale put them.

## Checked

Insulin and 9uoa read back with gemmi: IMEAN the weighted mean of I(+) and
I(-) to rounding, N = N(+) + N(-), F^2 / IMEAN 0.998 for strong reflections,
every F positive, 4.8 per cent in the free set. Not yet compared column by
column with dials.merge's output on the same data: `mxeq mtz` against
`dials.merge`'s `merged.mtz` is the next check.

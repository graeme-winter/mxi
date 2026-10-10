# Symmetry

STATUS: `mxi_symmetry` works on one sweep, and on several of one crystal
indexed together (5 October 2026). Not yet: sweeps indexed apart, and so the
indexing ambiguity between them; sweeps on much different scales, which would
want each normalised before they are pooled. `docs/outstanding.md` has the
whole list.

**Several sweeps** are pooled, as dials.symmetry takes them: every sweep's
observations, each placed in the rotation by its own sweep's scan, merged in P1
together, and every experiment reindexed alike. Each must have a crystal, and
their cells must agree to 2 per cent -- the lattice is the first's -- which
joint indexing gives them; otherwise refused, with the reason
(`docs/multi-sweep.md`).

`mxi_symmetry` finds the Laue group and space group of one sweep's integrated
data and writes the data reindexed into them, as dials.symmetry does.

**The cell is made the group's**, as dials.symmetry leaves it: a = b = c and
every angle 90 degrees for cubic, a = b for tetragonal, alpha = gamma = 90 for
monoclinic, and so on. The real-space metric is averaged over the group's
rotations, (1/n) sum R^T G R, as cctbx's `average_unit_cell` does -- which needs
no table of crystal systems and holds in any setting -- and the crystal's
orientation kept: A = U B, U kept and B made anew from the averaged cell, at
every scan point too. On insulin, 77.905 A on every edge and 90 degrees, to
1e-13 at all 301 points. `reindex` does it, so `mxi_scale --change-of-basis`
does too.

## Running it

    mxi_symmetry integrated.rflx      # symmetrized.rflx

or from and to the DIALS pair, as `docs/rflx.md` describes:

    mxi_symmetry integrated.expt integrated.refl \
        --output-expt symmetrized.expt --output-refl symmetrized.refl

`--max-delta D` sets the lattice's tolerance in degrees of obliquity (2). It
prints what it merged and to what resolution, each symmetry element's score,
every subgroup's, the space groups judged by their absences, and the choice.

## How it works, and against dials.symmetry

1. **The lattice's symmetry**, by gemmi's implementation of Le Page's method,
   within 2 degrees; its subgroups, from the closures of every pair of
   rotations; each named in its reference setting by a search over short
   lattice vectors along the group's axes -- and for a group of one axis those
   it reverses or that lie perpendicular to it -- since gemmi names a group
   only in a tabulated setting.
2. **Each element and each subgroup scored** as Evans (2011) A1 and A2, read
   from dials.symmetry's source: elements counted as it counts them, 17 for
   m-3m; E(CC; S) and sigma(CC) estimated as it does. Intensities merged in P1
   and normalised as dials.symmetry's ml_aniso normalises them: an anisotropic
   Wilson model, E(I) = k f2(s) exp(-q.b) with q the six products of the
   indices, fitted by Wilson's maximum likelihood over cctbx's window, d*^2 from
   0.008 to 0.690, and the intensities divided by k and the exponential -- the
   atoms' own fall-off, f2, left in, so that the strong reflections still weigh
   most in a correlation. f2 is carbon's, nitrogen's and oxygen's mean, and
   nothing else of cctbx's protein model -- no solvent ripple, no content from
   a Matthews coefficient -- so a weak prior, near right for a small molecule
   too (Graeme's choice of the options, October 2026). It was quasi-
   normalisation by resolution shells, which flattened every resolution to one
   and gave the noisiest pairs the weight of the best: on a P 4_1 crystal, 43.6,
   43.6, 212 A, the identity's CC 0.776 where dials.symmetry has 0.95 on the
   same data, the 4-fold's 0.656, and P 1 chosen. Now 0.842 and 0.841, P 4/m by
   0.791 against 0.154, and P 4_1. The B fitted, 52, 54 and 84 A^2 along a*,
   b*, c*, is larger than dials.symmetry's 4 to 8, its protein model taking more
   of the fall-off as the atoms', so mxi's CCs lie between the shells' and
   dials.symmetry's. Insulin I 2 3, the small molecule P 4 2 2, as before. The
   Wilson outliers are still found from E^2 by shells, as dials.symmetry finds
   them.
3. **The space group** from the absences, among the chiral groups with that
   Patterson group: consistent if what it forbids beyond its centring has mean
   I/sigma of 3 or less, and of those the one explaining most; a tie to the
   lowest number, the others reported as indistinguishable.

On the 300 image sweep, integrated here:

                          here              dials.symmetry
    Patterson group       I m -3            I m -3
      likelihood          1.000             0.879
      NetZcc              9.42              6.79
      CC, CC-             0.99, 0.04        0.89, 0.18
    reindex operator      b+c,a+c,a+b       b+c,a+c,a+b
    space group           I 2 3             I 2 3
                          (I 21 3 indistinguishable by absences under I)

The candidates are the same Patterson groups with the same multiplicities as
dials.symmetry lists; monoclinic ones are named C 1 2/m 1, the reference
setting, where dials.symmetry gives I 1 2/m 1. mxi_scale on symmetrized.expt
with no options gives merging statistics identical, and scaled.refl
byte-identical, to mxi_scale given the space group and change of basis by hand.
The cell is written reindexed but not constrained: 77.93 77.90 77.89 A where
dials.symmetry reports 77.898.

## A dataset measured to the corner: two simplifications undone

On a sweep dials.symmetry called I m -3, mxi_symmetry chose P -1: the true
three-folds scored CC 0.27 where dials.symmetry has 0.97. The lattice's rotations
were right -- they keep the cell's metric to 0.2 per cent, transposed they are
167 per cent off -- and the difference was in the data scored:

* **No resolution limit.** dials.symmetry scores to a limit from the data, the
  finer of CC half above 0.6 and <I>/<sigma> above 4 (1.68 A there); this scored
  to the detector's corner, 176220 reflections with mates merged where
  dials.symmetry kept 173252 with mates apart, most of them beyond the
  diffraction. Quasi-normalised -- as it then was -- noise weighed as much as
  signal in a correlation.
  Now the same limit, with dials.symmetry's other filters: observations with
  I/sigma below -5, and Wilson outliers of E^2 16 or more.
* **Friedel mates merged.** The identity then compared each reflection with
  itself and gave CC 1 exactly, and E(CC; S), the CC a true element is expected
  to reach, was set from that. With mates apart the identity compares I(h) with
  I(-h) and measures it: 0.987 on the 300 image sweep, where dials.symmetry has
  0.979.

A test plants m-3 in strong reflections to 3.5 A and pure noise beyond: without
the limit it finds P -1, as that sweep did, and with it I m -3. The 300 image
sweep still gives I m -3, b+c,a+c,a+b and I 2 3. The limits differ from
dials.symmetry's -- 1.85 A from <I>/<sigma> there, 2.46 in dials.symmetry, which
fits a curve where this takes the last shell above the threshold.

After the fix that sweep gives I m -3 at likelihood 0.998, from 1.71 A where
dials.symmetry used 1.68, E(CC; S) 0.871 against 0.879, and I 2 3.

## The indexing ambiguity

On that sweep dials.symmetry reindexed by a+b,a+c,-b-c and this by b+c,a+c,a+b.
In the conventional cell dials.symmetry's axes are these as (c, b, -a): a
four-fold about b, which is in the lattice's m-3m and not in the crystal's m-3.
The two tables are on opposite sides of the indexing ambiguity m-3 has in a
cubic lattice. For one sweep either is right and scales the same; symmetrized.refl
is not index for index the same as dials.symmetry's, and sweeps -- or a sweep
and a reference -- must be put on the same side before they are merged. That
wants a reference, or dials.cosym's method.

## The resolution limit

The CC half limit is the scaling's own (`docs/scaling.md`): the tanh fit of
dials.estimate_resolution, at 0.6 as dials.symmetry sets it -- 2.14 A on the
300 image sweep. The <I>/<sigma> limit, above 4, is still the last of 20 shells
above it, where dials.symmetry fits a curve: 1.85 A there, against 2.46.


## Threads

Scoring the symmetry elements and naming the subgroups run on every thread,
`--threads N` to limit them: each element is scored into its own slot and each
subgroup named into its own, the table printed after in order, so the answer
is the same bits on any number of threads (`tests/test_laue.cc`). A reflection's
mates under an element are found by hash on the packed Miller index, and the
merge in P1 sums by hash and then sorts as the ordered map it was gave its
order -- each index's sums gathered in observation order either way. Before,
all of it ran on one thread with ordered-map lookups: on a dense ferritin data
set with 32 threads, 17 s on one core.

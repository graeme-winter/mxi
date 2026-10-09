#pragma once

// Choosing the Laue group: each symmetry element of the lattice scored by the
// correlation of the reflections it relates, and each subgroup by the product
// of its elements' likelihoods -- Evans (2011), Acta Cryst. D67, 282-292,
// appendices A1 and A2, as dials.symmetry implements them.

#include <cstddef>
#include <string>
#include <vector>

#include "expt.hh"
#include "refl.hh"
#include "symmetry.hh"

namespace mxi {

//: Intensities merged in P1, Friedel mates APART, one per reflection. Apart
//: so that the identity element compares I(h) with I(-h), and measures what
//: equivalent reflections achieve -- 0.931 on one dataset in dials.symmetry's
//: hands -- where with mates merged it compared each reflection with itself,
//: gave 1 exactly, and set the expected CC of every true element too high.
struct P1Intensities {
  std::vector<Miller> hkl;
  std::vector<double> i, sigma, d;
  std::size_t size() const { return hkl.size(); }
};

//: What merging in P1 kept and why, as dials.symmetry reports it.
struct P1Selection {
  std::size_t observations = 0,
              negative = 0; //: negative: I/sigma below -5, removed
  double d_min_cc_half = 0.0, d_min_i_over_sigma = 0.0, d_min = 0.0;
  std::size_t kept = 0; //: merged reflections to d_min
};

//: One sweep's integrated reflections merged in P1 by inverse variance, as
//: dials.symmetry takes them: observations as scaling takes them (profile
//: fitted, corrected by lp / qe / partiality, partiality at 0.4 or more), less
//: those with I/sigma below -5, to a resolution limit from the data -- the
//: finer of CC half above 0.6 and <I>/<sigma> above 4. Without the limit, a
//: sweep measured to the detector's corner had most of its reflections beyond
//: the diffraction, noise that pulled every element's CC toward zero and left
//: the true three-folds at 0.27.
P1Intensities merge_in_p1(const ExperimentList &experiments,
                          const Table &reflections,
                          P1Selection *report = nullptr);

//: The resolution to which <I>/<sigma> stays above the threshold, in shells of
//: equal count from low resolution: the d_min of the last shell before the
//: first below it. All of the data if none is.
double laue_resolution_limit(const P1Intensities &data,
                             double min_i_over_sigma = 4.0);
void select_resolution(P1Intensities &data, double d_min);

//: Divided by <I> in shells of equal count by resolution -- the
//: quasi-normalisation E^2, where dials.symmetry fits an anisotropic
//: maximum-likelihood model -- so that correlations compare like with like.
//: Wilson outliers, E^2 of 16 or more, are then removed, as dials.symmetry
//: removes them; returns how many.
//: The anisotropic Wilson model the intensities were normalised by: their
//: expectation k f2(s) exp(-q.b), q = (h^2, k^2, l^2, 2hk, 2hl, 2kl), fitted by
//: Wilson's maximum likelihood; f2 the mean of carbon's, nitrogen's and
//: oxygen's squared scattering factors.
struct WilsonFit {
  bool fitted = false;    //: false: too few reflections, shells used instead
  double log_scale = 0.0; //: ln k
  double b[6] = {0, 0, 0, 0, 0, 0};
  int iterations = 0;
  std::size_t used = 0; //: reflections in the window fitted
};

//: The intensities normalised as dials.symmetry's ml_aniso normalises them:
//: divided by the scale and the anisotropic fall-off of a Wilson model fitted
//: over cctbx's window, d*^2 from 0.008 to 0.690, its atoms' own fall-off --
//: f2(s) -- left in, as ml_normalise_aniso leaves it. Not the shells it was,
//: which flattened every resolution to a mean of one and gave the noisiest
//: pairs the weight of the strongest: on a P 4_1 crystal the identity's CC
//: was 0.79 where dials.symmetry's is 0.95, and the space group came out P 1.
//: Without cctbx's protein-specific terms -- the solvent's ripple, the content
//: from a Matthews coefficient -- so a weak prior, as near right for a small
//: molecule as for a protein. Wilson outliers, E^2 of 16 or more by shells,
//: removed as before; their number returned.
std::size_t normalise(P1Intensities &data, std::size_t per_shell = 200,
                      WilsonFit *fit = nullptr);

struct ElementScore {
  Rotation rotation;
  int order = 1;
  std::size_t pairs = 0;
  double cc = 0.0, sigma_cc = 0.0, z = 0.0;
  double p_given_present = 0.0, p_given_absent = 0.0, likelihood = 0.0;
  //: For pooling into a group's CC: sums of x, y, xx, yy, xy over the pairs.
  double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
};

struct GroupScore {
  std::vector<Rotation> rotations;
  std::vector<bool> contains; //: per element
  double likelihood = 0.0, z_for = 0.0, z_against = 0.0, z_net = 0.0;
  double cc_for = 0.0, cc_against = 0.0;
};

struct LaueScores {
  double cc_true = 0.0, cc_sig_fac = 0.0, e_cc_true = 0.0, cc_identity = 0.0;
  std::vector<ElementScore> elements;
  std::vector<GroupScore> groups; //: most likely first
  //: Wall seconds, for --timing: the E(CC) and sigma(CC) estimates, the
  //: elements, the subgroups.
  double t_estimates = 0.0, t_elements = 0.0, t_groups = 0.0;
};

//: The distinct symmetry elements of a lattice's rotations, as dials.symmetry
//: counts them: each rotation axis once, a rotation and its inverse together,
//: and the square of a four-fold as a two-fold of its own. 17 for m-3m.
std::vector<Rotation> symmetry_elements(const std::vector<Rotation> &lattice);
int rotation_order(const Rotation &r);

//: Score every element and every subgroup of the lattice's rotations.
LaueScores score_laue_groups(const P1Intensities &normalised,
                             const std::vector<Rotation> &lattice,
                             unsigned seed = 1);

//: Evans (2011) A1: p(CC; S), a Cauchy centred on E(CC; S), and p(CC; !S),
//: the same averaged over a true CC with density (1 - x^2)^(1/2) on [0, 1],
//: both truncated to [-1, 1].
double p_cc_given_present(double cc, double sigma_cc, double expected);
double p_cc_given_absent(double cc, double sigma_cc);

//: One candidate space group judged by its absences.
struct AbsenceTest {
  SpaceGroup group;
  //: Reflections measured that it forbids beyond its lattice centring, which
  //: every candidate shares, and their mean I/sigma.
  std::size_t tested = 0;
  double mean_i_over_sigma = 0.0;
  bool consistent = true;
};

struct SpaceGroupChoice {
  SpaceGroup chosen;
  std::vector<AbsenceTest> candidates;
  //: Consistent candidates the absences cannot tell from the one chosen --
  //: I 2 3 and I 21 3 under I centring, or an enantiomorphic pair.
  std::vector<std::string> indistinguishable;
};

//: Of the space groups whose Patterson group is `patterson`, the one the
//: absences support: consistent -- what it forbids beyond its centring has mean
//: I/sigma of 3 or less, or it forbids nothing more -- and among those, the one
//: explaining the most absences; a tie to the lowest number. `hkl` are in the
//: Patterson group's setting.
SpaceGroupChoice choose_space_group(const std::vector<Miller> &hkl,
                                    const std::vector<double> &intensity,
                                    const std::vector<double> &sigma,
                                    const SpaceGroup &patterson);

} // namespace mxi

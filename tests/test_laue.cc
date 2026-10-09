#include <cmath>
#include <map>
#include <random>

#include <gemmi/it92.hpp>

#include "../src/laue.hh"
#include "../src/parallel.hh"
#include "check.hh"

namespace mxi {

TEST(both_densities_of_cc_integrate_to_one) {
  // p(CC; S) and p(CC; !S) are densities in CC on [-1, 1]: a truncated Cauchy,
  // and the same averaged over the true CC. Each must integrate to one.
  for (double sigma : {0.1, 0.3}) {
    double present = 0.0, absent = 0.0;
    const int n = 4000;
    for (int k = 0; k < n; ++k) {
      const double cc = -1.0 + (k + 0.5) * 2.0 / n;
      present += p_cc_given_present(cc, sigma, 0.9) * 2.0 / n;
      absent += p_cc_given_absent(cc, sigma) * 2.0 / n;
    }
    check::close(present, 1.0, 1e-3, "p(CC; S) integrates to one");
    check::close(absent, 1.0, 2e-3, "p(CC; !S) integrates to one");
  }
}

TEST(symmetry_elements_are_counted_as_dials_counts_them) {
  const UnitCell cubic{67.42, 67.46, 67.46, 109.44, 109.47, 109.46};
  check::equal(
      static_cast<long long>(symmetry_elements(lattice_symmetry(cubic)).size()),
      17,
      "17 for m-3m: the identity, 3 four-folds, their 3 squares, 4 "
      "three-folds, 6 two-folds");
  const UnitCell tetragonal{50.0, 50.0, 80.0, 90.0, 90.0, 90.0};
  check::equal(static_cast<long long>(
                   symmetry_elements(lattice_symmetry(tetragonal)).size()),
               7, "7 for 4/mmm");
  const UnitCell triclinic{51.0, 62.0, 73.0, 81.0, 76.0, 69.0};
  check::equal(static_cast<long long>(
                   symmetry_elements(lattice_symmetry(triclinic)).size()),
               1, "1 for -1");
}

namespace {

//: Intensities in the insulin cell's primitive basis whose true symmetry is
//: `truth`, in the conventional setting b+c,a+c,a+b: exponential E^2 for each
//: unique reflection of it, five per cent noise on each P1 one.
P1Intensities planted(const char *truth, unsigned seed) {
  const UnitCell cell{67.42, 67.46, 67.46, 109.44, 109.47, 109.46};
  const SpaceGroup group = SpaceGroup::from_name(truth);
  const ChangeOfBasis cb = ChangeOfBasis::parse("b+c,a+c,a+b");
  std::mt19937 rng(seed);
  std::exponential_distribution<double> wilson(1.0);
  std::normal_distribution<double> noise(0.0, 1.0);
  std::map<Miller, double> value;
  P1Intensities out;
  for (int h = -14; h <= 14; ++h)
    for (int k = -14; k <= 14; ++k)
      for (int l = 0; l <= 14; ++l) {
        const Miller m{h, k, l};
        if (m == Miller{0, 0, 0} || (l == 0 && (k < 0 || (k == 0 && h < 0))))
          continue; // one of each Friedel pair
        const double d = d_spacing(cell, m);
        if (d < 3.0 || d > 30.0)
          continue;
        const Miller u = group.unique(cb.apply(m));
        if (!value.count(u))
          value[u] = wilson(rng);
        const double i = value[u] * (1.0 + 0.05 * noise(rng));
        out.hkl.push_back(m);
        out.i.push_back(i);
        out.sigma.push_back(0.05 * value[u] + 0.01);
        out.d.push_back(d);
      }
  return out;
}

std::string winner(const P1Intensities &data, double *likelihood) {
  const UnitCell cell{67.42, 67.46, 67.46, 109.44, 109.47, 109.46};
  const LaueScores s = score_laue_groups(data, lattice_symmetry(cell));
  *likelihood = s.groups.front().likelihood;
  const auto set = reference_setting(s.groups.front().rotations, cell);
  return set ? set->group.name() : "?";
}

} // namespace

TEST(the_laue_group_planted_is_the_laue_group_found) {
  double p = 0.0;
  const std::string cubic = winner(planted("I 2 3", 3), &p);
  check::is_true(cubic == "I m -3", "planted m-3, found " + cubic);
  check::is_true(p > 0.9, "with likelihood " + std::to_string(p));
  // Lower symmetry in the same cubic metric: the lattice allows m-3m, the
  // intensities only mmm. The group found must not be the higher one.
  const std::string ortho = winner(planted("I 2 2 2", 4), &p);
  check::is_true(ortho == "I m m m",
                 "planted mmm in a cubic metric, found " + ortho);
  check::is_true(p > 0.9, "with likelihood " + std::to_string(p));
}

namespace {

//: The failure seen on a dataset measured to the detector's corner: true m-3
//: symmetry in the strong low-resolution reflections, and a majority of pure
//: noise beyond. Friedel mates both present, as observations come.
P1Intensities noisy_beyond(double strong_to, unsigned seed) {
  const UnitCell cell{67.42, 67.46, 67.46, 109.44, 109.47, 109.46};
  const SpaceGroup group = SpaceGroup::from_name("I 2 3");
  const ChangeOfBasis cb = ChangeOfBasis::parse("b+c,a+c,a+b");
  std::mt19937 rng(seed);
  std::exponential_distribution<double> wilson(1.0);
  std::normal_distribution<double> noise(0.0, 1.0);
  std::map<Miller, double> value;
  P1Intensities out;
  for (int h = -18; h <= 18; ++h)
    for (int k = -18; k <= 18; ++k)
      for (int l = -18; l <= 18; ++l) {
        const Miller m{h, k, l};
        if (m == Miller{0, 0, 0})
          continue;
        const double d = d_spacing(cell, m);
        if (d < 2.2 || d > 30.0)
          continue;
        const Miller u = group.unique(cb.apply(m));
        if (!value.count(u))
          value[u] = wilson(rng);
        // Strong to `strong_to`; beyond it the signal is a twentieth of the
        // noise.
        const double signal = d >= strong_to ? 1.0 : 0.05;
        const double sigma = d >= strong_to ? 0.05 : 1.0;
        out.hkl.push_back(m);
        out.i.push_back(signal * value[u] + sigma * noise(rng));
        out.sigma.push_back(sigma);
        out.d.push_back(d);
      }
  return out;
}

} // namespace

TEST(noise_beyond_the_diffraction_does_not_hide_the_symmetry) {
  // As dials.symmetry does: a resolution limit from the data, before scoring.
  P1Intensities data = noisy_beyond(3.5, 6);
  const UnitCell cell{67.42, 67.46, 67.46, 109.44, 109.47, 109.46};
  const double d_min = laue_resolution_limit(data);
  check::is_true(d_min > 3.0 && d_min < 4.2,
                 "the limit found: " + std::to_string(d_min));
  select_resolution(data, d_min);
  normalise(data);
  const LaueScores s = score_laue_groups(data, lattice_symmetry(cell));
  const auto set = reference_setting(s.groups.front().rotations, cell);
  const std::string found = set ? set->group.name() : "?";
  check::is_true(found == "I m -3", "planted m-3 under noise, found " + found);
  check::is_true(s.cc_identity < 0.999,
                 "and the identity, Friedel mates apart, is measured: " +
                     std::to_string(s.cc_identity));
}

TEST(the_laue_scores_are_the_same_on_any_number_of_threads) {
  // The symmetry elements are scored on threads of their own, each writing
  // only its own score: every element's sums and every group's likelihood the
  // same bits on one thread as on eight, in the same order.
  const UnitCell cell{67.42, 67.46, 67.46, 109.44, 109.47, 109.46};
  const P1Intensities data = planted("I 2 3", 3);
  const std::vector<Rotation> lattice = lattice_symmetry(cell);
  set_parallel_threads(1);
  const LaueScores one = score_laue_groups(data, lattice);
  set_parallel_threads(8);
  const LaueScores eight = score_laue_groups(data, lattice);
  set_parallel_threads(0);
  check::equal(static_cast<long long>(one.elements.size()),
               static_cast<long long>(eight.elements.size()),
               "the same elements");
  check::is_true(one.elements.size() > 4, "a cubic lattice's many elements");
  for (std::size_t k = 0; k < one.elements.size(); ++k) {
    const ElementScore &a = one.elements[k], &b = eight.elements[k];
    check::is_true(a.rotation == b.rotation && a.pairs == b.pairs &&
                       a.sx == b.sx && a.sy == b.sy && a.sxx == b.sxx &&
                       a.syy == b.syy && a.sxy == b.sxy && a.cc == b.cc &&
                       a.likelihood == b.likelihood,
                   "element " + std::to_string(k) + " the same");
  }
  check::equal(static_cast<long long>(one.groups.size()),
               static_cast<long long>(eight.groups.size()),
               "the same subgroups");
  for (std::size_t k = 0; k < one.groups.size(); ++k)
    check::is_true(one.groups[k].likelihood == eight.groups[k].likelihood &&
                       one.groups[k].rotations == eight.groups[k].rotations,
                   "subgroup " + std::to_string(k) +
                       " the same, in the same place");
}

} // namespace mxi

namespace mxi {

TEST(the_wilson_model_recovers_an_anisotropic_b) {
  // Merged intensities of a 43.6, 43.6, 212 A cell, as Graeme's P 4_1 crystal,
  // drawn from Wilson's distribution about a light atom's fall-off and an
  // anisotropic B of 50, 55 and 85 A^2 along a*, b*, c*: the fit -- Wilson's
  // maximum likelihood, the B in the exponent of the indices -- gives them
  // back, and normalising removes them, leaving the atoms' own fall-off in.
  const double a = 43.6, c = 212.0;
  const double B[3] = {50.0, 55.0, 85.0};
  const double b[3] = {B[0] / (2 * a * a), B[1] / (2 * a * a),
                       B[2] / (2 * c * c)};
  std::mt19937 rng(11);
  std::exponential_distribution<double> wilson(1.0);
  P1Intensities data;
  for (int h = -14; h <= 14; ++h)
    for (int k = -14; k <= 14; ++k)
      for (int l = -70; l <= 70; ++l) {
        if (h == 0 && k == 0 && l == 0)
          continue;
        const double ds2 = (h * h + k * k) / (a * a) + l * l / (c * c);
        if (ds2 > 1.0 / (3.0 * 3.0))
          continue;
        const double d = 1.0 / std::sqrt(ds2);
        // The light atom's f^2 the fit assumes: carbon's, nitrogen's and
        // oxygen's, mean of the squares.
        double f2 = 0.0;
        for (gemmi::El el : {gemmi::El::C, gemmi::El::N, gemmi::El::O}) {
          const double f =
              gemmi::IT92<double>::get(el, 0).calculate_sf(ds2 / 4.0);
          f2 += f * f / 3.0;
        }
        const double mean =
            1000.0 * f2 *
            std::exp(-(b[0] * h * h + b[1] * k * k + b[2] * l * l));
        data.hkl.push_back({h, k, l});
        data.i.push_back(mean * wilson(rng));
        data.sigma.push_back(1.0);
        data.d.push_back(d);
      }
  WilsonFit fit;
  normalise(data, 200, &fit);
  check::is_true(fit.fitted, "the model fitted");
  // Within 2 A^2: the intensities are one draw each from Wilson's
  // distribution, about 50000 of them.
  check::close(2 * fit.b[0] * a * a, B[0], 2.0, "B along a*");
  check::close(2 * fit.b[1] * a * a, B[1], 2.0, "B along b*");
  check::close(2 * fit.b[2] * c * c, B[2], 2.0, "B along c*");
  // And none between a* and b*, in the same units: within 1 A^2.
  check::close(2 * fit.b[3] * a * a, 0.0, 1.0, "no cross term");
}

} // namespace mxi

// mxi_symmetry: the Laue group and space group of one sweep's integrated
// data, and the data reindexed into it -- as dials.symmetry does. The lattice's
// metric symmetry by Le Page's method; each element and each subgroup scored
// as Evans (2011); screw axes from the absences.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "args.hh"
#include "expt.hh"
#include "laue.hh"
#include "log_mirror.hh"
#include "parallel.hh"
#include "refl.hh"
#include "symmetry.hh"
#include "timing.hh"

namespace mxi {

namespace {

void usage() {
  std::printf(
      "usage: mxi_symmetry [options] INTEGRATED_EXPT INTEGRATED_REFL\n"
      "  --threads N       N threads for scoring and naming the subgroups; "
      "every\n"
      "                    core by default, the answer the same on any number\n"
      "  --max-delta D     the lattice's symmetry to D degrees of obliquity "
      "(2)\n"
      "  --output-expt PATH   the models, reindexed (symmetrized.expt)\n"
      "  --output-refl PATH   the reflections, reindexed (symmetrized.refl)\n"
      "  --timing          where the time goes\n");
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {
      "--max-delta", "--threads", "--output-expt", "--output-refl", "--timing"};
  std::set<std::string> takes_value = known;
  takes_value.erase("--timing");
  const Arguments args = parse_arguments(argc, argv, known, takes_value);
  if (args.help) {
    usage();
    return 0;
  }
  if (!args.ok) {
    std::fprintf(stderr, "mxi_symmetry: %s\n", args.error.c_str());
    return 2;
  }
  set_parallel_threads(static_cast<std::size_t>(args.number("--threads", 0.0)));
  if (args.positional.size() != 2) {
    std::fprintf(stderr,
                 "mxi_symmetry: expected an .expt and a .refl, got %zu files\n",
                 args.positional.size());
    usage();
    return 2;
  }
  Timing timing(args.has("--timing"));
  try {
    double mark = Timing::now();
    const auto phase = [&](const char *name) {
      const double t = Timing::now();
      timing.add(name, t - mark);
      mark = t;
    };
    ExperimentList experiments = read_experiments(args.positional[0]);
    Table reflections = read_reflections(args.positional[1]);
    phase("reading");
    // Several sweeps of one crystal pooled, as dials.symmetry takes them: the
    // lattice is the first's, so every sweep must have a crystal and the same
    // cell -- sweeps indexed apart may not, and may not even share a setting,
    // which joint indexing gives them. Pooled as they are, unscaled: sweeps on
    // much different scales would want each normalised first, not yet done.
    if (experiments.size() == 0)
      throw std::runtime_error("no experiments");
    for (std::size_t i = 0; i < experiments.size(); ++i)
      if (!experiments[i].crystal)
        throw std::runtime_error("experiment " + std::to_string(i) +
                                 " has no crystal: index it first");
    const UnitCell cell = experiments[0].crystal->cell();
    for (std::size_t i = 1; i < experiments.size(); ++i) {
      const UnitCell other = experiments[i].crystal->cell();
      const double worst = std::max(
          {std::fabs(other.a / cell.a - 1.0), std::fabs(other.b / cell.b - 1.0),
           std::fabs(other.c / cell.c - 1.0),
           std::fabs(other.alpha - cell.alpha) / 90.0,
           std::fabs(other.beta - cell.beta) / 90.0,
           std::fabs(other.gamma - cell.gamma) / 90.0});
      if (worst > 0.02)
        throw std::runtime_error(
            "experiment " + std::to_string(i) +
            "'s cell differs from the first's by " +
            std::to_string(static_cast<int>(std::round(100.0 * worst))) +
            " per cent: index the sweeps together, so that they share one "
            "cell and one setting");
    }
    if (experiments.size() > 1)
      std::printf("%zu sweeps, pooled\n", experiments.size());
    std::printf("Cell %.3f %.3f %.3f A, %.3f %.3f %.3f deg\n", cell.a, cell.b,
                cell.c, cell.alpha, cell.beta, cell.gamma);

    P1Selection selection;
    const P1Intensities merged =
        merge_in_p1(experiments, reflections, &selection);
    phase("merging in P1, and the resolution limit");
    std::printf("%zu observations; %zu removed with I/sigma below -5\n",
                selection.observations, selection.negative);
    std::printf("Resolution from CC half above 0.6: %.2f A; from <I>/<sigma> "
                "above 4: %.2f A; "
                "the finer, %.2f A, used\n",
                selection.d_min_cc_half, selection.d_min_i_over_sigma,
                selection.d_min);
    P1Intensities normalised = merged;
    WilsonFit wilson_fit;
    const std::size_t wilson = normalise(normalised, 200, &wilson_fit);
    if (wilson_fit.fitted) {
      // B along each reciprocal axis, in A^2: the exponent's b times twice the
      // cell edge squared -- for a cell near orthogonal, what dials.symmetry
      // reports as B_cart's diagonal.
      std::printf(
          "Normalised by an anisotropic Wilson model, %zu reflections "
          "fitted in %d steps: B %.2f, %.2f, %.2f A^2 along a*, b*, c*\n",
          wilson_fit.used, wilson_fit.iterations,
          2.0 * wilson_fit.b[0] * cell.a * cell.a,
          2.0 * wilson_fit.b[1] * cell.b * cell.b,
          2.0 * wilson_fit.b[2] * cell.c * cell.c);
    } else {
      std::printf("Too few reflections for a Wilson model (%zu): normalised "
                  "in shells instead\n",
                  wilson_fit.used);
    }
    phase("normalising");
    std::printf("%zu Wilson outliers removed, E^2 of 16 or more\n", wilson);
    const std::vector<Rotation> lattice =
        lattice_symmetry(cell, args.number("--max-delta", 2.0));
    phase("the lattice's symmetry");
    const LaueScores scores = score_laue_groups(normalised, lattice);
    phase("scoring");
    timing.add("E(CC) and sigma(CC)", scores.t_estimates, 1);
    timing.add("the elements", scores.t_elements, 1);
    timing.add("the subgroups", scores.t_groups, 1);
    std::printf("%zu reflections merged in P1, Friedel mates apart; the "
                "lattice has %zu rotations, "
                "%zu symmetry "
                "elements, %zu subgroups\n",
                merged.size(), lattice.size(), scores.elements.size(),
                scores.groups.size());
    std::printf("E(CC; S) %.3f, from the intensities %.3f and from the "
                "identity %.3f; sigma(CC) "
                "factor %.3f\n",
                scores.cc_true, scores.e_cc_true, scores.cc_identity,
                scores.cc_sig_fac);

    std::printf("\nScoring each symmetry element\n");
    std::printf("  %-4s %5s %7s %6s %7s %10s\n", "", "order", "pairs", "CC",
                "Z-CC", "likelihood");
    for (std::size_t k = 0; k < scores.elements.size(); ++k) {
      const ElementScore &e = scores.elements[k];
      std::printf("  %-4c %5d %7zu %6.3f %7.2f %10.3f\n",
                  static_cast<char>('a' + k), e.order, e.pairs, e.cc, e.z,
                  e.likelihood);
    }

    std::printf("\nScoring all possible subgroups\n");
    std::printf("  %-14s %3s %10s %7s %6s %6s %5s %5s  %-20s %s\n",
                "Patterson group", "", "likelihood", "NetZcc", "Zcc+", "Zcc-",
                "CC", "CC-", "reindex", "elements");
    // Every subgroup's reference setting and name, each on its own thread --
    // naming them one after another was most of a small run -- then the table
    // printed in order.
    std::vector<std::optional<Setting>> settings(scores.groups.size());
    for_each_index(scores.groups.size(), [&](std::size_t k) {
      settings[k] = reference_setting(scores.groups[k].rotations, cell);
    });
    std::optional<Setting> best;
    for (std::size_t k = 0; k < scores.groups.size(); ++k) {
      const GroupScore &g = scores.groups[k];
      const std::optional<Setting> &s = settings[k];
      if (k == 0)
        best = s;
      std::string letters;
      for (std::size_t j = 0; j < g.contains.size(); ++j)
        letters += g.contains[j] ? static_cast<char>('a' + j) : '.';
      std::printf(
          "  %-14s %3s %10.3f %7.2f %6.2f %6.2f %5.2f %5.2f  %-20s %s\n",
          s ? s->group.name().c_str() : "?", k == 0 ? "***" : "", g.likelihood,
          g.z_net, g.z_for, g.z_against, g.cc_for, g.cc_against,
          s ? s->cb_text.c_str() : "?", letters.c_str());
    }
    if (!best)
      throw std::runtime_error(
          "the most likely group could not be put in a reference setting");
    phase("naming the subgroups, and printing");

    // Screw axes from the absences, in the chosen group's own setting.
    std::vector<Miller> hkl;
    for (const Miller &h : merged.hkl)
      hkl.push_back(best->cb.apply(h));
    const SpaceGroupChoice choice =
        choose_space_group(hkl, merged.i, merged.sigma, best->group);
    phase("the space group, by absences");
    std::printf(
        "\nSpace groups with Patterson group %s, judged by their absences\n",
        best->group.name().c_str());
    std::printf("  %-14s %8s %10s  %s\n", "space group", "tested", "<I/sigma>",
                "");
    for (const AbsenceTest &t : choice.candidates)
      std::printf("  %-14s %8zu %10.2f  %s\n", t.group.name().c_str(), t.tested,
                  t.mean_i_over_sigma,
                  t.consistent ? "consistent" : "inconsistent");
    std::printf("\nRecommended space group: %s\n",
                choice.chosen.name().c_str());
    for (const std::string &n : choice.indistinguishable)
      std::printf("  indistinguishable by its absences from %s\n", n.c_str());
    std::printf("Reindex operator: %s\n", best->cb_text.c_str());

    reindex(experiments, reflections, best->cb, choice.chosen);
    const UnitCell after = experiments[0].crystal->cell();
    std::printf("Cell %.3f %.3f %.3f A, %.3f %.3f %.3f deg\n", after.a, after.b,
                after.c, after.alpha, after.beta, after.gamma);
    const std::string out_expt =
        args.value("--output-expt", "symmetrized.expt");
    const std::string out_refl =
        args.value("--output-refl", "symmetrized.refl");
    write_experiments(out_expt, experiments);
    write_reflections(out_refl, reflections);
    std::printf("\nWrote %s and %s\n", out_expt.c_str(), out_refl.c_str());
    phase("reindexing and writing");
    timing.report(stdout);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "mxi_symmetry: %s\n", error.what());
    return 1;
  }
  return 0;
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_symmetry.log in the working directory, as DIALS writes
  // dials.symmetry.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_symmetry.log");
  return mxi::run_program(argc, argv);
}

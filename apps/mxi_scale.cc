// mxi_scale: put every observation of one sweep on one scale, refine an error
// model, and report merging statistics. The physical model of Beilsten-Edmands
// et al. (2020), with B-splines for the scale and decay; see
// docs/scaling.md.

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "args.hh"
#include "expt.hh"
#include "log_mirror.hh"
#include "parallel.hh"
#include "refl.hh"
#include "resolution.hh"
#include "rflx.hh"
#include "scale.hh"
#include "symmetry.hh"
#include "timing.hh"

namespace mxi {

namespace {

void usage() {
  std::printf(
      "usage: mxi_scale [options] INTEGRATED_EXPT INTEGRATED_REFL\n"
      "  --space-group NAME  the space group, \"I 2 3\" or \"197\"; default "
      "the\n"
      "                    crystal's own\n"
      "  --change-of-basis CB   to that group's setting first, in DIALS'\n"
      "                    notation: \"b+c,a+c,a+b\" from the primitive cell "
      "of\n"
      "                    a body-centred cubic lattice\n"
      "  --d-min D         leave out reflections beyond D A; the summary then "
      "has\n"
      "                    no Suggested column\n"
      "  --d-max D         leave out reflections with d above D A: a low\n"
      "                    resolution limit\n"
      "  --d-min-auto      scale everything, estimate the limit from CC half, "
      "and\n"
      "                    scale again to it, if CC half falls to the limit\n"
      "                    within the data at all\n"
      "  --cc-half-limit C   the CC half the limit is set at (0.3)\n"
      "  --anomalous       Friedel mates apart: I(+) and I(-) of an acentric\n"
      "                    reflection scaled, given their error model and\n"
      "                    rejected as outliers as separate groups, for data\n"
      "                    with a strong anomalous signal. The merging table "
      "is\n"
      "                    the same groups either way\n"
      "  --no-absorption   no absorption surface, whatever the sweep\n"
      "  --absorption-level LEVEL\n"
      "                    as dials.scale's: low (the default) for ~1%% "
      "relative\n"
      "                    absorption -- degree 4, restraint 5e5; medium for "
      "~5%%\n"
      "                    -- 6, 5e4; high for >25%%, long wavelengths or "
      "heavy\n"
      "                    absorbers -- 6, 5e3\n"
      "  --l-max L         the absorption surface to degree L, lmax (lmax + "
      "2)\n"
      "                    terms: 4 by default from 60 degrees (24), 6 for 48\n"
      "  --profile-only    profile-fitted intensities alone, not a mix with\n"
      "                    summation chosen by Rmeas\n"
      "  --shells N        resolution shells in the table (20)\n"
      "  -o PATH           where to write: one .rflx (scaled.rflx), or with a\n"
      "                    .refl the DIALS pair, the .refl that\n"
      "  --output-expt PATH   the DIALS pair's experiment list (scaled.expt)\n"
      "  --threads N       threads to use (0: the machine's); the answer is "
      "the\n"
      "                    same for any number\n"
      "  --timing          where the time goes\n");
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {
      "--anomalous",       "--absorption-level",
      "--l-max",           "--d-min-auto",
      "--cc-half-limit",   "--threads",
      "--timing",          "--space-group",
      "--change-of-basis", "--d-min",
      "--no-absorption",   "--profile-only",
      "--shells",          "-o",
      "--output-expt",     "--d-max"};
  const std::set<std::string> takes_value = {"--l-max",
                                             "--absorption-level",
                                             "--cc-half-limit",
                                             "--threads",
                                             "--space-group",
                                             "--change-of-basis",
                                             "--d-min",
                                             "--shells",
                                             "-o",
                                             "--output-expt",
                                             "--d-max"};
  Arguments args = parse_arguments(argc, argv, known, takes_value);
  // One .rflx stands for the experiment list and the reflections
  // (docs/rflx.md).
  args.positional = rflx::as_pair(args.positional);
  if (args.help) {
    usage();
    return 0;
  }
  if (!args.ok) {
    std::fprintf(stderr, "mxi_scale: %s\n", args.error.c_str());
    return 2;
  }
  const std::string level = args.value("--absorption-level", "low");
  if (level != "low" && level != "medium" && level != "high") {
    std::fprintf(stderr,
                 "mxi_scale: --absorption-level is low, medium or high, not "
                 "'%s'\n",
                 level.c_str());
    return 2;
  }
  if (args.has("--absorption-level") && args.has("--no-absorption")) {
    std::fprintf(stderr, "mxi_scale: --absorption-level and --no-absorption "
                         "contradict each other\n");
    return 2;
  }
  if (args.has("--l-max")) {
    const double l = args.number("--l-max", -1.0);
    if (!(l >= 0.0 && l <= 12.0 && l == std::floor(l))) {
      std::fprintf(
          stderr,
          "mxi_scale: --l-max is a whole number from 0 to 12, not '%s'\n",
          args.value("--l-max", "").c_str());
      return 2;
    }
    if (args.has("--no-absorption")) {
      std::fprintf(
          stderr,
          "mxi_scale: --l-max and --no-absorption contradict each other\n");
      return 2;
    }
  }
  if (args.has("--d-min") && args.has("--d-min-auto")) {
    std::fprintf(
        stderr,
        "mxi_scale: --d-min and --d-min-auto are two answers to one question; "
        "give one\n");
    return 2;
  }
  if (args.positional.size() != 2) {
    std::fprintf(stderr,
                 "mxi_scale: expected an .expt and a .refl, got %zu files\n",
                 args.positional.size());
    usage();
    return 2;
  }
  Timing timing(args.has("--timing"));
  set_parallel_threads(static_cast<std::size_t>(args.number("--threads", 0.0)));
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
    // One sweep or several of one crystal, each scaled by a model of its own
    // sharing the merged intensities; each must have a crystal, and they
    // share the first's cell and space group, as symmetry left them.
    if (experiments.size() == 0)
      throw std::runtime_error("no experiments");
    for (std::size_t i = 0; i < experiments.size(); ++i)
      if (!experiments[i].crystal)
        throw std::runtime_error("experiment " + std::to_string(i) +
                                 " has no crystal: index it first");
    const SpaceGroup group =
        args.has("--space-group")
            ? SpaceGroup::from_name(args.value("--space-group", ""))
            : SpaceGroup::from_hall(experiments[0].crystal->space_group_hall);
    if (args.has("--change-of-basis")) {
      const std::string cb = args.value("--change-of-basis", "");
      reindex(experiments, reflections, ChangeOfBasis::parse(cb), group);
      std::printf("Reindexed by %s\n", cb.c_str());
    } else {
      for (Experiment &e : experiments)
        e.crystal->space_group_hall = group.hall();
    }
    const UnitCell cell = experiments[0].crystal->cell();
    std::printf("Space group %s, Laue class %s; cell %.3f %.3f %.3f A, %.3f "
                "%.3f %.3f deg\n",
                group.name().c_str(), group.laue().c_str(), cell.a, cell.b,
                cell.c, cell.alpha, cell.beta, cell.gamma);

    ScaleRunOptions options;
    options.combine = !args.has("--profile-only");
    options.absorption = !args.has("--no-absorption");
    options.lmax = static_cast<int>(args.number("--l-max", -1.0));
    // dials.scale's absorption levels: the degree a wide sweep takes and the
    // restraint on its harmonics, sum P_lm^2.
    options.level_lmax = level == "low" ? 4 : 6;
    options.fit.absorption_restraint =
        level == "low" ? 5e5 : (level == "medium" ? 5e4 : 5e3);
    options.anomalous = args.has("--anomalous");
    options.d_min = args.number("--d-min", 0.0);
    options.d_max = args.number("--d-max", 0.0);
    const double cc_half_limit = args.number("--cc-half-limit", 0.3);
    phase("reindexing");
    // --d-min-auto: everything scaled once, for the estimate alone, and then
    // scaled again to the limit it finds -- again rather than filtered, since
    // the outliers, the error model and the fit all depend on what is in. Only
    // if CC half comes down to the limit within the data: where it never does,
    // the estimate is just the last bin with pairs enough to fit, and a cut
    // there would drop data that are good.
    if (args.has("--d-min-auto")) {
      const ScaleRun first =
          scale_sweep(experiments, reflections, group, options);
      phase("scaling everything, for the limit");
      for (const auto &[name, seconds] : first.timing)
        timing.add(name, seconds, 1);
      const ResolutionEstimate first_res = estimate_resolution(
          cc_half_bins(first.data, first.g, *experiments[0].crystal, 50, 10,
                       0.1),
          cc_half_limit);
      phase("the limit from it");
      if (first_res.reached) {
        // To a hundredth of an angstrom, which is all the estimate is good
        // for, so that what was chosen can be said and repeated: this is what
        // --d-min with that number does, exactly.
        options.d_min = std::round(first_res.d_min_cc_half * 100.0) / 100.0;
        std::printf(
            "\n--d-min-auto: CC half falls to %.2f at %.2f A with everything "
            "scaled; scaling again, as --d-min %.2f would\n",
            cc_half_limit, first_res.d_min_cc_half, options.d_min);
      } else {
        std::printf(
            "\n--d-min-auto: CC half stays above %.2f to the edge of the data; "
            "no limit applied\n",
            cc_half_limit);
      }
    }
    const ScaleRun run = scale_sweep(experiments, reflections, group, options);
    phase("scaling");
    for (const auto &[name, seconds] : run.timing)
      timing.add(name, seconds, 1);
    const ScaleData &data = run.data;
    if (data.size() == 0)
      throw std::runtime_error("no observations fit to scale");

    const ScaleModelShape &shape = run.model.shape();
    std::printf("\n%zu observations of %zu reflections, of %zu rows\n",
                data.size(), data.unique.size(), reflections.nrows);
    if (run.model.sweeps() == 1)
      std::printf("Scaling model: %zu scale, %zu decay and %zu absorption "
                  "parameters, %zu in "
                  "all; fitted on %zu observations\n",
                  shape.scale_points, shape.decay_points,
                  harmonic_count(shape.lmax), run.model.size(), run.fitted_on);
    else
      std::printf("Scaling model: %zu sweeps, a model each, %zu parameters in "
                  "all; fitted on %zu observations\n",
                  run.model.sweeps(), run.model.size(), run.fitted_on);
    {
      std::size_t harmonics = 0;
      for (std::size_t w = 0; w < run.model.sweeps(); ++w)
        harmonics += harmonic_count(run.model.shape(w).lmax);
      if (harmonics > 0)
        std::printf("  absorption level %s: degree %d, each harmonic "
                    "restrained by %g\n",
                    level.c_str(), run.model.shape(0).lmax,
                    options.fit.absorption_restraint);
    }
    if (!data.pair.empty())
      std::printf(
          "Friedel mates kept apart (--anomalous): %zu groups scaled, from %zu "
          "symmetry-unique reflections\n",
          data.unique.size(), data.pair_unique.size());
    for (std::size_t k = 0; k < run.fits.size(); ++k)
      std::printf("  fit %zu: %d steps, target %.6g to %.6g%s\n", k + 1,
                  run.fits[k].iterations, run.fits[k].target_start,
                  run.fits[k].target_end,
                  run.fits[k].converged ? "" : ", not converged");
    // Each sweep's scale and relative B: for several, side by side, which is
    // where their relative scales show.
    for (std::size_t w = 0; w < run.model.sweeps(); ++w) {
      const ScaleModelShape &own = run.model.shape(w);
      double cmin = HUGE_VAL, cmax = 0.0, bmin = HUGE_VAL, bmax = -HUGE_VAL;
      for (std::size_t i = 0; i < own.scale_points; ++i) {
        const double c = run.model.parameters[run.model.first_scale(w) + i];
        cmin = std::fmin(cmin, c);
        cmax = std::fmax(cmax, c);
      }
      for (std::size_t i = 0; i < own.decay_points; ++i) {
        const double b = run.model.parameters[run.model.first_decay(w) + i];
        bmin = std::fmin(bmin, b);
        bmax = std::fmax(bmax, b);
      }
      if (run.model.sweeps() > 1)
        std::printf("  sweep %zu: %zu scale, %zu decay, %zu absorption "
                    "parameters;",
                    w, own.scale_points, own.decay_points,
                    harmonic_count(own.lmax));
      std::printf("  scale %.4f to %.4f", cmin, cmax);
      if (own.decay_points > 0)
        std::printf("; relative B %.3f to %.3f A^2", bmin, bmax);
      std::printf("\n");
    }
    if (run.i_mid == 0.0)
      std::printf("Intensities: profile fitted\n");
    else if (std::isinf(run.i_mid))
      std::printf("Intensities: summation\n");
    else
      std::printf(
          "Intensities: profile fitted, crossing to summation at I = %.0f\n",
          run.i_mid);
    if (run.covariance.ok) {
      double big = 0.0, total = 0.0;
      for (std::size_t k = 0; k < run.g_variance.size(); ++k) {
        const double f = std::sqrt(run.g_variance[k]) / run.g[k];
        big = std::fmax(big, f);
        total += f;
      }
      std::printf("Scale uncertainties: sigma(g)/g %.4f on average, %.4f at "
                  "most; goodness of "
                  "fit %.3f on %zu degrees of freedom; carried into each "
                  "variance before the "
                  "error model\n",
                  total / static_cast<double>(run.g_variance.size()), big,
                  run.covariance.goodness_of_fit,
                  run.covariance.degrees_of_freedom);
    } else {
      std::printf("Scale uncertainties: the normal matrix could not be "
                  "inverted; none carried\n");
    }
    std::printf("Error model: a = %.4f, b = %.4f, from %zu observations\n",
                run.error_model.a, run.error_model.b, run.error_model.used);
    std::printf("%zu outliers\n", run.outliers);

    MergingShell all;
    const int shells = static_cast<int>(args.number("--shells", 20.0));
    const std::vector<MergingShell> table = merging_statistics(
        data, run.g, group, *experiments[0].crystal, shells, &all);
    phase("merging statistics");
    std::printf("\nMerging statistics, Friedel mates merged\n");
    std::printf("  %6s %6s %7s %6s %6s %6s %8s %7s %6s %7s %6s %6s\n", "d_max",
                "d_min", "#obs", "#uniq", "mult.", "%comp", "<I>", "<I/sI>",
                "r_mrg", "r_meas", "r_pim", "cc1/2");
    const auto row = [](const MergingShell &m) {
      std::printf("  %6.2f %6.2f %7zu %6zu %6.2f %6.2f %8.1f %7.1f %6.3f %7.3f "
                  "%6.3f %6.3f\n",
                  m.d_max, m.d_min, m.observations, m.unique, m.multiplicity,
                  100.0 * m.completeness, m.mean_i, m.i_over_sigma, m.rmerge,
                  m.rmeas, m.rpim, m.cc_half);
    };
    for (const MergingShell &m : table)
      row(m);
    row(all);

    // The resolution limit, as dials.estimate_resolution finds it: a tanh in
    // d*^2 through CC half in 50 bins of equal count, where it crosses 0.3;
    // and where CC half stops being significant at the 0.1 level.
    std::size_t wilson = 0;
    const std::vector<ResolutionBin> bins = cc_half_bins(
        data, run.g, *experiments[0].crystal, 50, 10, 0.1, &wilson);
    const ResolutionEstimate res = estimate_resolution(bins, cc_half_limit);
    phase("the resolution limit");
    std::printf("\nRemoving %zu Wilson outliers with E^2 >= 16.0\n", wilson);
    if (res.d_min_cc_half > 0.0)
      std::printf("Resolution cc_half:       %.2f\n", res.d_min_cc_half);
    else
      std::printf("Resolution cc_half:       none found\n");
    if (res.d_min_significance > 0.0)
      std::printf("Resolution cc_half_significance_level:    %.2f\n",
                  res.d_min_significance);
    else
      std::printf("Resolution cc_half_significance_level:    none found\n");

    // "Suggested": the same summary, cut at the CC half limit -- unless a
    // limit was given with --d-min, which is the cut already chosen; a column
    // cut somewhere else beside it would be a second answer to a question
    // already settled. The estimate above is still printed.
    MergingShell suggested;
    const bool cut = res.d_min_cc_half > 0.0 && !args.has("--d-min") &&
                     !args.has("--d-min-auto");
    if (cut) {
      ScaleData to_limit = data;
      for (std::size_t i = 0; i < to_limit.size(); ++i) {
        const Miller &m = to_limit.unique[to_limit.group[i]];
        if (experiments[0].crystal->d_spacing(m[0], m[1], m[2]) <
            res.d_min_cc_half)
          to_limit.outlier[i] = true;
      }
      merging_statistics(to_limit, run.g, group, *experiments[0].crystal,
                         shells, &suggested);
    }
    phase("the suggested cut");

    // The summary dials.scale ends with: overall, the lowest shell and the
    // highest, and the data cut at the suggested limit.
    {
      const MergingShell &low = table.front(), &high = table.back();
      std::printf("\n            -------------Summary of merging "
                  "statistics--------------\n\n");
      if (cut)
        std::printf("%-44s %8s %7s %7s %9s\n", "", "Overall", "Low", "High",
                    "Suggested");
      else
        std::printf("%-44s %8s %7s %7s\n", "", "Overall", "Low", "High");
      const auto line = [&](const char *what, double MergingShell::*f,
                            const char *fmt, double scale) {
        char x[4][32];
        const MergingShell *m[4] = {&all, &low, &high, &suggested};
        for (int k = 0; k < 4; ++k)
          std::snprintf(x[k], sizeof x[k], fmt, scale * (m[k]->*f));
        if (cut)
          std::printf("%-44s %8s %7s %7s %9s\n", what, x[0], x[1], x[2], x[3]);
        else
          std::printf("%-44s %8s %7s %7s\n", what, x[0], x[1], x[2]);
      };
      const auto count = [&](const char *what, std::size_t MergingShell::*f) {
        if (cut)
          std::printf("%-44s %8zu %7zu %7zu %9zu\n", what, all.*f, low.*f,
                      high.*f, suggested.*f);
        else
          std::printf("%-44s %8zu %7zu %7zu\n", what, all.*f, low.*f, high.*f);
      };
      const auto only = [&](const char *what, double v, const char *fmt) {
        char x[32];
        std::snprintf(x, sizeof x, fmt, v);
        std::printf("%-44s %8s\n", what, x);
      };
      line("High resolution limit", &MergingShell::d_min, "%.2f", 1.0);
      line("Low resolution limit", &MergingShell::d_max, "%.2f", 1.0);
      line("Completeness", &MergingShell::completeness, "%.1f", 100.0);
      line("Multiplicity", &MergingShell::multiplicity, "%.1f", 1.0);
      line("I/sigma", &MergingShell::i_over_sigma, "%.1f", 1.0);
      line("Rmerge(I)", &MergingShell::rmerge, "%.3f", 1.0);
      line("Rmerge(I+/-)", &MergingShell::rmerge_anom, "%.3f", 1.0);
      line("Rmeas(I)", &MergingShell::rmeas, "%.3f", 1.0);
      line("Rmeas(I+/-)", &MergingShell::rmeas_anom, "%.3f", 1.0);
      line("Rpim(I)", &MergingShell::rpim, "%.3f", 1.0);
      line("Rpim(I+/-)", &MergingShell::rpim_anom, "%.3f", 1.0);
      line("CC half", &MergingShell::cc_half, "%.3f", 1.0);
      line("Anomalous completeness", &MergingShell::anom_completeness, "%.1f",
           100.0);
      line("Anomalous multiplicity", &MergingShell::anom_multiplicity, "%.1f",
           1.0);
      line("Anomalous correlation", &MergingShell::cc_anom, "%.3f", 1.0);
      only("Anomalous slope", all.anom_slope, "%.3f");
      only("dF/F", all.df_over_f, "%.3f");
      only("dI/s(dI)", all.di_over_sig_di, "%.3f");
      count("Total observations", &MergingShell::observations);
      count("Total unique", &MergingShell::unique);
    }

    // dials.scale's columns and flags, so that dials.merge and dials.export
    // take the table on.
    write_scaling(reflections, data, run.g, run.g_variance);
    const rflx::Outputs out = rflx::outputs(args, "scaled");
    const json::Value document = experiments_to_json(experiments);
    rflx::write_outputs(out, &document, &reflections, "mxi_scale");
    std::printf("\nWrote %s\n", rflx::describe(out).c_str());
    phase("writing");
    timing.report(stdout);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "mxi_scale: %s\n", error.what());
    return 1;
  }
  return 0;
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_scale.log in the working directory, as DIALS writes
  // dials.scale.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_scale.log");
  return mxi::run_program(argc, argv);
}

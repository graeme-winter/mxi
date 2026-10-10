// mxi_refine: refine the experimental model against indexed spot centroids.
//
//   mxi_refine indexed.expt indexed.refl

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
#include "log_mirror.hh"
#include "postrefine.hh"
#include "refine.hh"
#include "rflx.hh"
#include "timing.hh"
#include <algorithm>
#include <vector>

namespace mxi {

namespace {
void usage() {
  std::printf(
      "usage: mxi_refine INDEXED.expt INDEXED.refl [options]\n"
      "  --no-crystal      hold the crystal fixed\n"
      "  --jacobian-threads N  threads for the Jacobian; 0 is one per core "
      "(0)\n"
      "  --no-detector     hold the detector fixed\n"
      "  --beam            refine the beam direction too (off: correlated\n"
      "                    with the detector on a single sweep)\n"
      "  --shared-crystal  several sweeps: one crystal for them all, where by\n"
      "                    default each refines its own (--separate, as was)\n"
      "  --conditional-depth  mean depth given absorption, not eqn (6)\n"
      "  --scan-varying [N]  control points in A across the scan. "
      "Scan-varying\n"
      "                    is the default, one per 10 degrees and at least "
      "five,\n"
      "                    on a scan of 10 degrees or more; N sets the number\n"
      "  --static          a crystal that does not move: one A for the scan\n"
      "  --unit-weights    ignore the centroid variances\n"
      "  --strong-only     build the model from the stronger half only\n"
      "  --analytic        analytical derivatives, not finite differences\n"
      "  --timing          where the time went, by phase\n"
      "  --normal-threads N  threads for the normal equations; 0 is one per\n"
      "                   core (0). A reduction, so a threaded run differs "
      "from\n"
      "                   a serial one in the last bits; 1 to avoid that\n"
      "  --detector-in-scan-varying  keep refining the detector during the\n"
      "                    scan-varying pass; it is degenerate with the cell\n"
      "  --min-volume V    drop reflections whose rotation angle is not\n"
      "                    determined by the data; 0 keeps them all (0.05)\n"
      "  --z-weight W      scale the weight on the rotation-angle residual\n"
      "  --macrocycles N   (3)\n"
      "  --outlier-sigma S (4; 0 disables rejection)\n"
      "  --output-expt P   (refined.expt)\n"
      "  --output-refl P   (refined.refl)\n");
}
} // namespace

namespace {
double now_wall() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
} // namespace

namespace {

// A refinement pass as a table: each macrocycle's kept reflections, the
// outliers rejected after its fit, and its RMSDs. Printed by the program from
// what refine() records; the library used to print these lines itself.
void print_cycles(const mxi::RefineResult &r) {
  std::printf("  %5s %11s %8s %8s %8s %8s\n", "cycle", "reflections",
              "rejected", "RMSD x", "RMSD y", "RMSD z");
  std::printf("  %5s %11s %8s %8s %8s %8s\n", "", "", "", "(px)", "(px)",
              "(images)");
  for (std::size_t c = 0; c < r.cycles.size(); ++c) {
    const mxi::RefineCycle &k = r.cycles[c];
    std::printf("  %5zu %11zu %8zu %8.3f %8.3f %8.3f\n", c + 1, k.reflections,
                k.rejected, k.rmsd_x, k.rmsd_y, k.rmsd_z);
  }
}

double quantile(std::vector<double> v, double q) {
  if (v.empty())
    return 0.0;
  std::sort(v.begin(), v.end());
  const double at = q * static_cast<double>(v.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(at);
  const std::size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (at - static_cast<double>(lo)) * (v[hi] - v[lo]);
}

// Calculated minus observed over the reflections refined on, as
// dials.refine's summary statistics give them -- in pixels and images, the
// units of this program's RMSDs, where DIALS uses millimetres and degrees. A
// median well away from zero is a systematic the RMSD cannot show.
void print_residuals(const mxi::Table &t,
                     const std::vector<std::size_t> &unpredicted) {
  if (!t.has("xyzcal.px") || !t.has("xyzobs.px.value") || !t.has("flags"))
    return;
  std::vector<bool> skip(t.nrows, false);
  for (std::size_t i : unpredicted)
    if (i < t.nrows)
      skip[i] = true;
  std::size_t left_out = 0;
  const mxi::Column &cal = t.at("xyzcal.px");
  const mxi::Column &obs = t.at("xyzobs.px.value");
  const mxi::Column &flags = t.at("flags");
  std::vector<double> d[3];
  for (std::size_t i = 0; i < t.nrows; ++i) {
    if ((flags.ints[i] & mxi::flag::kUsedInRefinement) == 0)
      continue;
    // No prediction was written for it, so its calculated position is the
    // column's default and not a prediction: a residual from it is fiction.
    if (skip[i]) {
      ++left_out;
      continue;
    }
    for (int k = 0; k < 3; ++k)
      d[k].push_back(cal.reals[i * 3 + k] - obs.reals[i * 3 + k]);
  }
  if (d[0].empty())
    return;
  std::printf(
      "\nCalculated minus observed, over the %zu reflections refined on",
      d[0].size());
  if (left_out > 0)
    std::printf(
        " (%zu more could not be predicted with the final model, and are "
        "left out)",
        left_out);
  std::printf(":\n");
  std::printf("  %-12s %9s %9s %9s %9s %9s\n", "", "min", "Q1", "median", "Q3",
              "max");
  const char *names[3] = {"x (px)", "y (px)", "z (images)"};
  for (int k = 0; k < 3; ++k) {
    std::printf("  %-12s %9.4f %9.4f %9.4f %9.4f %9.4f\n", names[k],
                quantile(d[k], 0.0), quantile(d[k], 0.25), quantile(d[k], 0.5),
                quantile(d[k], 0.75), quantile(d[k], 1.0));
  }
}

} // namespace

int run_program(int argc, char **argv) {
  const double t_start = now_wall();
  double t_read = 0.0;
  double t_write = 0.0;
  const std::set<std::string> known = {"--static",
                                       "--no-crystal",
                                       "--no-detector",
                                       "--beam",
                                       "--separate",
                                       "--shared-crystal",
                                       "--macrocycles",
                                       "--outlier-sigma",
                                       "--output-expt",
                                       "--output-refl",
                                       "--conditional-depth",
                                       "--scan-varying",
                                       "--unit-weights",
                                       "--strong-only",
                                       "--z-weight",
                                       "--analytic",
                                       "--min-volume",
                                       "--detector-in-scan-varying",
                                       "--jacobian-threads",
                                       "--timing",
                                       "--normal-threads"};
  const std::set<std::string> takes_value = {
      "--macrocycles",      "--outlier-sigma", "--output-expt",
      "--output-refl",      "--z-weight",      "--min-volume",
      "--jacobian-threads", "--normal-threads"};
  Arguments args =
      parse_arguments(argc, argv, known, takes_value, {"--scan-varying"});
  // One .rflx stands for the experiment list and the reflections
  // (docs/rflx.md).
  args.positional = rflx::as_pair(args.positional);
  // 0 means one per core, 1 means none. Exposed because a threading change
  // that cannot be switched off cannot be measured against its absence.
  g_normal_threads =
      static_cast<std::size_t>(args.number("--normal-threads", 0.0));
  g_jacobian_threads =
      static_cast<std::size_t>(args.number("--jacobian-threads", 0.0));
  if (args.help) {
    usage();
    return 0;
  }
  if (!args.ok) {
    std::fprintf(stderr, "mxi_refine: %s\n", args.error.c_str());
    return 2;
  }
  if (args.has("--static") && args.has("--scan-varying")) {
    std::fprintf(
        stderr,
        "mxi_refine: --static and --scan-varying contradict each other\n");
    return 2;
  }
  if (args.positional.size() != 2) {
    std::fprintf(stderr,
                 "mxi_refine: expected an .expt and a .refl, got %zu file "
                 "arguments\n",
                 args.positional.size());
    usage();
    return 2;
  }

  RefineOptions options;
  options.crystal = !args.has("--no-crystal");
  options.detector = !args.has("--no-detector");
  options.beam = args.has("--beam");
  // Several sweeps refine a crystal each by default, as DIALS refines them:
  // indexing put them in one basis, and one matrix cannot fit sweeps no
  // goniometer quite returns between. --separate was the way to ask for this
  // and is still taken.
  options.shared_crystal = args.has("--shared-crystal");
  options.unit_weights = args.has("--unit-weights");
  options.strong_only = args.has("--strong-only");
  options.analytic = args.has("--analytic");
  options.macrocycles = static_cast<int>(args.number("--macrocycles", 3));
  options.outlier_sigma = args.number("--outlier-sigma", 4.0);
  options.z_weight = args.number("--z-weight", 1.0);
  options.min_volume = args.number("--min-volume", 0.05);
  const bool conditional_depth = args.has("--conditional-depth");
  const std::string out_expt = args.value("--output-expt", "refined.expt");
  const std::string out_refl = args.value("--output-refl", "refined.refl");

  try {
    const double t_read_start = now_wall();
    ExperimentList experiments = read_experiments(args.positional[0]);
    Table reflections = read_reflections(args.positional[1]);
    t_read = now_wall() - t_read_start;
    // Scan-varying by default -- nearly every real crystal moves -- one control
    // point per 10 degrees of each sweep's own scan; static if asked, or on a
    // scan under 10 degrees, too little rotation to tell a moving crystal from
    // noise. Each sweep its own: the first sweep's count for all gave a 120
    // degree sweep the 35 of the 350 degree ones beside it.
    std::vector<std::size_t> points_of;
    for (const Experiment &e : experiments) {
      const double degrees =
          std::abs(e.scan.osc_width) * static_cast<double>(e.scan.num_images());
      std::size_t points = 1;
      if (args.has("--static")) {
        points = 1;
      } else if (args.has("--scan-varying") &&
                 !args.value("--scan-varying", "").empty()) {
        points = static_cast<std::size_t>(args.number("--scan-varying", 1));
      } else if (degrees >= 10.0) {
        points = scan_varying_points(e.scan);
      } else {
        std::printf("Static: the scan is %.1f degrees, under the 10 a "
                    "scan-varying crystal "
                    "needs\n",
                    degrees);
      }
      points_of.push_back(points);
    }
    // One count, as one sweep has: printed as before; several, each.
    std::string points_text = std::to_string(points_of.front());
    if (std::any_of(points_of.begin(), points_of.end(),
                    [&](std::size_t p) { return p != points_of.front(); })) {
      points_text.clear();
      for (std::size_t p : points_of)
        points_text += (points_text.empty() ? "" : ", ") + std::to_string(p);
    }
    if (conditional_depth) {
      for (Experiment &e : experiments) {
        for (Panel &p : e.detector.panels)
          p.parallax_conditional = true;
      }
      std::printf("using the conditional absorption depth\n");
    }
    std::printf("Refining against %zu reflections from %zu experiment%s\n",
                reflections.nrows, experiments.size(),
                experiments.size() == 1 ? "" : "s");

    const TwoPassRefinement two =
        refine_in_two_passes(experiments, reflections, options, points_of,
                             !args.has("--detector-in-scan-varying"));
    if (two.static_pass.n_used > 0) {
      std::printf("\nScan-static: %zu parameters\n",
                  two.static_pass.n_parameters);
      print_cycles(two.static_pass);
    }
    if (two.varied && two.varying_pass.n_used > 0) {
      std::printf("\nScan-varying, %s control points: %zu parameters%s\n",
                  points_text.c_str(), two.varying_pass.n_parameters,
                  two.detector_held
                      ? "; the detector held where the scan-static pass put "
                        "it (--detector-in-scan-varying to refine it too)"
                      : "");
      print_cycles(two.varying_pass);
    }
    RefineResult result = two.final();
    if (result.n_used == 0) {
      std::fprintf(stderr, "mxi_refine: nothing to refine against\n");
      return 1;
    }
    std::printf(
        "\nRefined on %zu of %zu reflections: %zu rejected as outliers, "
        "%zu with an undetermined rotation angle; %d steps\n",
        result.n_used, reflections.nrows, result.n_rejected,
        result.n_ill_conditioned, result.iterations);
    // Said plainly, because a residual averaged over a different set of
    // reflections is not comparable with anything -- including dials.refine,
    // which applies the same cutoff and reports over what is left.
    std::printf("RMSD over those %zu: %.4f px, %.4f px, %.4f images\n",
                result.n_used, result.rmsd_x, result.rmsd_y, result.rmsd_z);

    // s1, rlp and entering follow the refined model, as they do in DIALS.
    // xyzobs.mm deliberately does not: it is what the spot finder measured
    // through the model as imported, and recomputing it here would silently
    // change the observations refinement was just fitted to.
    set_refinement_flags(result, reflections);
    add_reciprocal_columns(experiments, reflections);
    const std::vector<std::size_t> unpredicted =
        update_predictions(experiments, reflections);
    print_residuals(reflections, unpredicted);
    std::printf("\n");
    for (std::size_t i = 0; i < experiments.size(); ++i) {
      if (!experiments[i].crystal)
        continue;
      const UnitCell c = experiments[i].crystal->cell();
      std::printf(
          "Unit cell%s: %.4f %.4f %.4f A, %.3f %.3f %.3f deg; volume %.0f "
          "A^3\n",
          experiments.size() > 1 ? (" [" + std::to_string(i) + "]").c_str()
                                 : "",
          c.a, c.b, c.c, c.alpha, c.beta, c.gamma, c.volume());
    }
    const double t_write_start = now_wall();
    write_experiments(out_expt, experiments);
    write_reflections(out_refl, reflections);
    t_write = now_wall() - t_write_start;
    std::printf("Wrote %s and %s\n", out_expt.c_str(), out_refl.c_str());

    if (args.has("--timing")) {
      Timing timing(true, t_start);
      timing.add("reading", t_read);
      timing.add("building the target rows", g_observations_seconds);
      timing.add("the jacobian", g_jacobian_seconds);
      timing.add("the normal equations", g_normal_seconds);
      timing.add("the solve", g_solve_seconds);
      timing.add("the trial residuals", g_residual_seconds);
      timing.add("outlier rejection", g_outlier_seconds);
      timing.add("writing", t_write);
      timing.report(stdout);
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_refine: %s\n", e.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_refine.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_refine.log");
  return mxi::run_program(argc, argv);
}

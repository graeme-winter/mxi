// mxi_index: index a strong spot list against an imported experiment.
//
//   mxi_index imported.expt strong.refl [options]
//
// Writes indexed.expt and indexed.refl, so the result can be compared against
// DIALS' own with `mxeq check indexed`.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <set>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
#include "fft.hh"
#include "index.hh"
#include "log_mirror.hh"
#include "refine.hh"
#include "rflx.hh"
#include "timing.hh"

namespace mxi {

namespace {

void usage() {
  std::printf(
      "usage: mxi_index IMPORTED.expt STRONG.refl [options]\n"
      "  --d-min D        resolution limit (default: from the data)\n"
      "  --fft-threads N  threads for the transform, when FFTW was built with\n"
      "                   threading; 0 is one per core (0)\n"
      "  --jacobian-threads N  threads for the analytical Jacobian; 0 is one\n"
      "                   per core (0)\n"
      "  --max-cell A     longest cell edge (default: from spot spacing)\n"
      "  --grid N         FFT grid size, power of two (default: from d_min)\n"
      "  --timing         where the time went, by phase\n"
      "  --tolerance T    how far an index may fall from an integer (0.3)\n"
      "  --candidates N   basis vectors taken from the peak list (30)\n"
      "  --output-expt P  (default indexed.expt)\n"
      "  --output-refl P  (default indexed.refl)\n"
      "  --macrocycles N  assign/refine/re-assign cycles (3)\n"
      "  --shared-crystal  several sweeps: one crystal throughout, where by\n"
      "                   default the last cycle refines each sweep's apart\n"
      "  --all-reflections  refine on everything, not the stronger half\n"
      "  --verbose         also print the search: candidate vectors, the fit\n"
      "                    at each tolerance, and the cell before reduction\n"
      "  --quiet           accepted for older scripts; the search is no\n"
      "                    longer printed unless --verbose asks for it\n");
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {"--d-min",
                                       "--max-cell",
                                       "--grid",
                                       "--tolerance",
                                       "--candidates",
                                       "--output-expt",
                                       "--output-refl",
                                       "--quiet",
                                       "--verbose",
                                       "--macrocycles",
                                       "--all-reflections",
                                       "--timing",
                                       "--jacobian-threads",
                                       "--fft-threads",
                                       "--shared-crystal"};
  const std::set<std::string> takes_value = {
      "--d-min",       "--max-cell",    "--grid",
      "--tolerance",   "--candidates",  "--output-expt",
      "--output-refl", "--macrocycles", "--jacobian-threads",
      "--fft-threads"};
  Arguments args = parse_arguments(argc, argv, known, takes_value);
  // One .rflx stands for the experiment list and the reflections
  // (docs/rflx.md).
  args.positional = rflx::as_pair(args.positional);
  // 0 means one per core, 1 means none. Exposed because a threading change
  // that cannot be switched off cannot be measured against its absence.
  g_fft_threads = static_cast<std::size_t>(args.number("--fft-threads", 0.0));
  g_jacobian_threads =
      static_cast<std::size_t>(args.number("--jacobian-threads", 0.0));
  if (args.help) {
    usage();
    return 0;
  }
  if (!args.ok) {
    std::fprintf(stderr, "mxi_index: %s\n", args.error.c_str());
    return 2;
  }
  if (args.positional.size() != 2) {
    std::fprintf(stderr,
                 "mxi_index: expected an .expt and a .refl, got %zu file "
                 "arguments\n",
                 args.positional.size());
    usage();
    return 2;
  }

  IndexOptions options;
  // The search's diagnostics -- candidate vectors, the fit at each tolerance,
  // the cell before and after reduction -- are for whoever is working on the
  // indexer, and were printed by default. The result is what a user reads.
  options.verbose = args.has("--verbose");
  options.d_min = args.number("--d-min", 0.0);
  options.max_cell = args.number("--max-cell", 0.0);
  options.grid = static_cast<std::size_t>(args.number("--grid", 0));
  options.tolerance = args.number("--tolerance", 0.3);
  options.n_candidates =
      static_cast<std::size_t>(args.number("--candidates", 30));
  options.macrocycles = static_cast<int>(args.number("--macrocycles", 3));
  options.split_sweeps = !args.has("--shared-crystal");
  options.refine_on_strong = !args.has("--all-reflections");
  const std::string out_expt = args.value("--output-expt", "indexed.expt");
  const std::string out_refl = args.value("--output-refl", "indexed.refl");

  try {
    const double t_run_start = Timing::now();
    ExperimentList experiments = read_experiments(args.positional[0]);
    Table reflections = read_reflections(args.positional[1]);
    const double t_read_seconds = Timing::now() - t_run_start;
    std::printf("Indexing %zu reflections from %zu experiment%s\n",
                reflections.nrows, experiments.size(),
                experiments.size() == 1 ? "" : "s");
    for (const std::string &d : reflections.dropped()) {
      std::fprintf(stderr, "mxi_index: not read: %s\n", d.c_str());
    }

    // Before indexing, because these are observations expressed through the
    // detector model and dials.find_spots writes them from the model as
    // imported. Computing them from the model indexing has just refined would
    // be a difference from DIALS dressed up as a correction -- and DIALS never
    // recomputes them either, which is exactly why xyzobs.mm.value must not be
    // used as a join key between two runs.
    add_observed_columns(experiments, reflections);

    const IndexResult result = index(experiments, reflections, options);
    // The search's terms whether it succeeds or not: they are what a failure
    // is diagnosed by.
    if (result.max_cell > 0.0)
      std::printf("  to %.2f A, maximum cell %.1f A, FFT grid %zu^3\n",
                  result.d_min, result.max_cell, result.grid);
    if (result.n_indexed == 0) {
      std::fprintf(stderr,
                   "mxi_index: no lattice found. Try --max-cell, or --d-min to "
                   "restrict the resolution range used.\n");
      return 1;
    }

    // Each macrocycle: refine on the strong reflections, index everything
    // again. The RMSDs are the refinement's, in pixels and images, which is
    // what DIALS reports and what to set beside it; the index RMSD, which has
    // no unit, is how far fractional Miller indices sit from integers. It was
    // printed as "rmsd 0.0292" with nothing to say which it was.
    if (!result.cycles.empty()) {
      std::printf("\n  %5s %10s %8s %8s %8s %8s %8s %8s\n", "cycle",
                  "refined on", "rejected", "indexed", "RMSD x", "RMSD y",
                  "RMSD z", "index");
      std::printf("  %5s %10s %8s %8s %8s %8s %8s %8s\n", "", "", "", "",
                  "(px)", "(px)", "(images)", "RMSD");
      for (std::size_t c = 0; c < result.cycles.size(); ++c) {
        const IndexCycle &k = result.cycles[c];
        std::printf("  %5zu %10zu %8zu %8zu %8.3f %8.3f %8.3f %8.4f%s\n", c + 1,
                    k.refined_on, k.rejected, k.indexed, k.rmsd_x, k.rmsd_y,
                    k.rmsd_z, k.rmsd_index,
                    k.per_sweep ? "  each sweep's crystal apart" : "");
      }
      std::printf("\n");
    }
    const UnitCell cell = result.crystal.cell();
    std::printf("Indexed %zu of %zu reflections (%.1f%%)\n", result.n_indexed,
                result.n_total, 100.0 * result.fraction_indexed());
    std::printf(
        "Unit cell: %.3f %.3f %.3f A, %.3f %.3f %.3f deg; volume %.0f A^3\n",
        cell.a, cell.b, cell.c, cell.alpha, cell.beta, cell.gamma,
        cell.volume());
    // Each sweep's, when the last cycle refined them apart.
    if (!result.cycles.empty() && result.cycles.back().per_sweep)
      for (std::size_t i = 0; i < experiments.size(); ++i) {
        const UnitCell c = experiments[i].crystal->cell();
        std::printf("  sweep %zu: %.3f %.3f %.3f A, %.3f %.3f %.3f deg\n", i,
                    c.a, c.b, c.c, c.alpha, c.beta, c.gamma);
      }

    set_indexed_flags(reflections);
    add_reciprocal_columns(experiments, reflections);
    update_predictions(experiments, reflections);
    const double t_write_start = Timing::now();
    write_experiments(out_expt, experiments);
    write_reflections(out_refl, reflections);
    const double t_write_seconds = Timing::now() - t_write_start;
    std::printf("Wrote %s and %s\n", out_expt.c_str(), out_refl.c_str());

    if (args.has("--timing")) {
      const IndexTiming &t = result.timing;
      Timing timing(true, t_run_start);
      timing.add("reading", t_read_seconds);
      timing.add("indexing", t.total);
      timing.add("reciprocal points", t.reciprocal_points, 1);
      timing.add("max cell", t.max_cell, 1);
      timing.add("candidate vectors", t.candidate_vectors, 1);
      timing.add("the transform", t.fft, 2);
      timing.add("the peak search", t.peak_search, 2);
      timing.add("the rest of it", t.candidate_vectors - t.fft - t.peak_search,
                 2);
      timing.add("choose basis", t.choose_basis, 1);
      timing.add("fit and reduce", t.fit_and_reduce, 1);
      timing.add("macrocycles", t.macrocycles, 1);
      timing.add("copy and select", t.subset_copy, 2);
      timing.add("refinement", t.refine, 2);
      timing.add("the jacobian", g_jacobian_seconds, 3);
      timing.add("the normal equations", g_normal_seconds, 3);
      timing.add("reassignment", t.reassign, 2);
      timing.add("writing", t_write_seconds);
      timing.note(std::to_string(t.triples_scored) + " triples scored, " +
                  std::to_string(t.triples_skipped) + " skipped as degenerate");
      timing.report(stdout);
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_index: %s\n", e.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_index.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_index.log");
  return mxi::run_program(argc, argv);
}

// mxi_grid: spots in Kabsch space, next to an average of their neighbours.
//
//   mxi_grid refined.expt refined.refl --out grids.txt
//
// Writes, for each chosen spot, its own density on the grid and the average of
// the spots nearest it on the detector, so the two can be looked at together.
// A single spot is badly undersampled -- a shoebox is a handful of pixels
// across -- and the reference is what says whether its shape is the spot's or
// the sampling's.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
#include "log_mirror.hh"
#include "profile_grid.hh"
#include "rflx.hh"
#include "shoebox.hh"
#include "timing.hh"

namespace mxi {

namespace {

void usage(const char *program) {
  std::printf(
      "usage: %s [options] EXPT REFL\n"
      "\n"
      "  --out FILE        where to write the grids (grids.txt)\n"
      "  --n N             grid is 2N+1 points a side (4)\n"
      "  --half-width W    grid spans plus and minus W sigma (3)\n"
      "  --neighbours K    spots averaged for the reference profile (200)\n"
      "  --spots N         how many example spots to write (4)\n"
      "  --recentre        put each spot on its own centroid before adding "
      "it,\n"
      "                    rather than on its predicted position. The\n"
      "                    prediction is off by 0.44 sigma_D on average, "
      "which\n"
      "                    blurs an aggregate by about root two\n"
      "  --subdivisions S  split each pixel S ways per axis; 1 to see the\n"
      "                    undersampling raw (5)\n"
      "  --sigma-b B --sigma-m M   use these instead of estimating\n"
      "  --timing          where the time goes\n",
      program);
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {
      "--timing",       "--out",        "--n",
      "--half-width",   "--neighbours", "--spots",
      "--subdivisions", "--sigma-b",    "--sigma-m",
      "--map",          "--recentre"};
  // Not every known option takes a value: passing one set as both made
  // --recentre demand an argument.
  std::set<std::string> takes_value = known;
  takes_value.erase("--recentre");
  Arguments args = parse_arguments(argc, argv, known, takes_value);
  // One .rflx stands for the experiment list and the reflections
  // (docs/rflx.md).
  args.positional = rflx::as_pair(args.positional);
  Timing timing(args.has("--timing"));
  double mark = Timing::now();
  if (args.help) {
    usage(argv[0]);
    return 0;
  }
  if (!args.ok) {
    std::fprintf(stderr, "mxi_grid: %s\n", args.error.c_str());
    return 2;
  }
  if (args.positional.size() != 2) {
    std::fprintf(stderr, "mxi_grid: expected an .expt and a .refl\n");
    usage(argv[0]);
    return 2;
  }

  try {
    const ExperimentList experiments = read_experiments(args.positional[0]);
    const Table reflections = read_reflections(args.positional[1]);
    timing.add("reading", Timing::now() - mark);
    mark = Timing::now();
    const std::vector<Shoebox> boxes = decode_shoeboxes(reflections);
    if (boxes.empty()) {
      std::fprintf(stderr, "mxi_grid: %s has no shoeboxes\n",
                   args.positional[1].c_str());
      return 1;
    }
    if (!reflections.has("s1") || !reflections.has("xyzcal.mm")) {
      std::fprintf(stderr, "mxi_grid: needs the s1 and xyzcal.mm columns\n");
      return 1;
    }

    const Experiment &e = experiments[0];
    const Column &s = reflections.at("s1");
    const Column &cal = reflections.at("xyzcal.mm");
    const Column &obs = reflections.at("xyzobs.px.value");
    const bool has_flags = reflections.has("flags");

    // The same selection the profile model uses, so the picture is of the
    // spots the sigmas were measured on and not of some other set.
    std::vector<std::size_t> chosen;
    for (std::size_t i = 0; i < reflections.nrows && i < boxes.size(); ++i) {
      if (has_flags &&
          (reflections.at("flags").integer(i) & flag::kUsedInRefinement) == 0) {
        continue;
      }
      const Vec3 beam{s.real(i, 0), s.real(i, 1), s.real(i, 2)};
      if (std::fabs(compute_zeta(e, beam)) < 0.05)
        continue;
      if (!has_prediction(reflections, i))
        continue;
      chosen.push_back(i);
    }
    if (chosen.size() < 10) {
      std::fprintf(stderr, "mxi_grid: only %zu usable spots\n", chosen.size());
      return 1;
    }

    double sigma_b = args.number("--sigma-b", 0.0);
    double sigma_m = args.number("--sigma-m", 0.0);
    if (!(sigma_b > 0.0) || !(sigma_m > 0.0)) {
      std::vector<Shoebox> selected;
      std::vector<Vec3> beams;
      std::vector<RangeSample> samples;
      for (std::size_t i : chosen) {
        const Vec3 beam{s.real(i, 0), s.real(i, 1), s.real(i, 2)};
        selected.push_back(boxes[i]);
        beams.push_back(beam);
        for (const RangeSample &sample : range_samples(
                 e, boxes[i], cal.real(i, 2), compute_zeta(e, beam))) {
          samples.push_back(sample);
        }
      }
      std::size_t used = 0;
      if (!(sigma_b > 0.0))
        sigma_b = beam_divergence(e, selected, beams, &used);
      if (!(sigma_m > 0.0)) {
        sigma_m =
            reflecting_range(samples, Scan::radians(e.scan.osc_width), 0.0);
      }
    }
    std::printf("sigma_b %.6f  sigma_m %.6f  over %zu spots\n", sigma_b,
                sigma_m, chosen.size());

    const int n = static_cast<int>(args.number("--n", 4));
    const double half = args.number("--half-width", 3.0);
    const int subdivisions = static_cast<int>(args.number("--subdivisions", 5));
    const std::size_t neighbours =
        static_cast<std::size_t>(args.number("--neighbours", 200));
    const bool recentre = args.has("--recentre");
    const std::size_t examples =
        static_cast<std::size_t>(args.number("--spots", 4));

    const std::string path = args.value("--out", "grids.txt");
    std::FILE *out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "mxi_grid: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fprintf(out, "# side %d half_width %g sigma_b %.9f sigma_m %.9f\n",
                 2 * n + 1, half, sigma_b, sigma_m);

    const auto write = [&](const char *label, const ProfileGrid &grid) {
      std::fprintf(out, "%s %zu %.6g %.6g\n", label, grid.n_spots,
                   grid.counts_added, grid.counts_outside);
      for (double v : grid.value)
        std::fprintf(out, "%.9g\n", v);
    };

    // Everything, as the reference of last resort.
    ProfileGrid all = make_grid(n, sigma_b, sigma_m, half);
    for (std::size_t i : chosen) {
      add_to_grid(e, boxes[i], {s.real(i, 0), s.real(i, 1), s.real(i, 2)},
                  cal.real(i, 2), &all, subdivisions, recentre);
    }
    all.normalise();
    write("all", all);
    std::printf("wrote the average of %zu spots\n", all.n_spots);

    // A few examples, each with the spots nearest it on the detector.
    for (std::size_t k = 0; k < examples && k < chosen.size(); ++k) {
      const std::size_t centre =
          chosen[(k + 1) * chosen.size() / (examples + 1)];
      ProfileGrid one = make_grid(n, sigma_b, sigma_m, half);
      add_to_grid(e, boxes[centre],
                  {s.real(centre, 0), s.real(centre, 1), s.real(centre, 2)},
                  cal.real(centre, 2), &one, subdivisions, recentre);
      one.normalise();

      // Nearest on the detector face, which is what "nearby" has to mean: the
      // profile varies across the detector, and two spots at the same place on
      // different images are far more alike than two at opposite corners.
      std::vector<std::pair<double, std::size_t>> distance;
      for (std::size_t i : chosen) {
        if (i == centre)
          continue;
        const double dx = obs.real(i, 0) - obs.real(centre, 0);
        const double dy = obs.real(i, 1) - obs.real(centre, 1);
        distance.emplace_back(dx * dx + dy * dy, i);
      }
      std::partial_sort(distance.begin(),
                        distance.begin() + static_cast<long>(std::min(
                                               neighbours, distance.size())),
                        distance.end());
      ProfileGrid reference = make_grid(n, sigma_b, sigma_m, half);
      for (std::size_t j = 0; j < neighbours && j < distance.size(); ++j) {
        const std::size_t i = distance[j].second;
        add_to_grid(e, boxes[i], {s.real(i, 0), s.real(i, 1), s.real(i, 2)},
                    cal.real(i, 2), &reference, subdivisions, recentre);
      }
      reference.normalise();

      char label[64];
      std::snprintf(label, sizeof(label), "spot_%zu_at_%.0f_%.0f", k,
                    obs.real(centre, 0), obs.real(centre, 1));
      write(label, one);
      std::snprintf(label, sizeof(label), "reference_%zu", k);
      write(label, reference);
    }
    std::fclose(out);
    std::printf("wrote %s\n", path.c_str());

    // The anisotropy against position, and the sensor smear that may explain
    // it. eps2 lies in the scattering plane -- radially on the detector --
    // and eps1 across it, so a smear from the depth at which a photon is
    // absorbed belongs entirely to eps2. If that is what the difference is, it
    // must grow with obliquity in the way the absorption predicts.
    const std::string map_path = args.value("--map", "");
    if (!map_path.empty()) {
      std::FILE *m = std::fopen(map_path.c_str(), "w");
      if (m == nullptr) {
        std::fprintf(stderr, "mxi_grid: cannot write %s\n", map_path.c_str());
        return 1;
      }
      const Panel &p0 = e.detector[0];
      std::fprintf(m,
                   "# x_mm y_mm radius_mm obliquity_deg width1 width2 width3 "
                   "counts predicted_smear width_along_axis width_across_axis "
                   "geometry_per_mm\n");
      const Vec3 normal = p0.fast.cross(p0.slow).normalized();
      for (std::size_t i : chosen) {
        const SpotMoments mom = spot_moments(
            e, boxes[i], {s.real(i, 0), s.real(i, 1), s.real(i, 2)},
            cal.real(i, 2));
        if (!mom.valid)
          continue;
        const double predicted =
            sensor_depth_width(p0.mu, p0.thickness, mom.obliquity, mom.path_mm);
        // The angular smear a source of one millimetre would give, so any
        // extent can be tested afterwards without recomputing the geometry.
        const double per_mm =
            source_extent_width(1.0, mom.obliquity, mom.path_mm);
        std::fprintf(m,
                     "%.3f %.3f %.4f %.5f %.6f %.6f %.6f %.6g %.6f %.6f %.6f "
                     "%.8f\n",
                     obs.real(i, 0) * p0.pixel_size[0],
                     obs.real(i, 1) * p0.pixel_size[1], mom.radius_mm,
                     Scan::degrees(mom.obliquity), mom.width1, mom.width2,
                     mom.width3, mom.counts, predicted, mom.width_along_axis,
                     mom.width_across_axis, per_mm);
      }
      std::fclose(m);
      (void)normal;
      std::printf("wrote %s\n", map_path.c_str());
    }
    timing.add("computing, printing and writing", Timing::now() - mark);
    timing.report(stdout);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_grid: %s\n", e.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_grid.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_grid.log");
  return mxi::run_program(argc, argv);
}

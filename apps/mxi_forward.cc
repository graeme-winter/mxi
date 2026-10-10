// mxi_forward: render the model onto the pixels and compare it with the data.
//
//   mxi_forward refined.expt refined.refl --out forward.txt
//
// For each strong spot, the model is integrated over the same pixels, the same
// images and the same mask as the observation, and both are reduced the same
// way. What is left is the model being wrong rather than the grid.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
#include "forward.hh"
#include "log_mirror.hh"
#include "profile_model.hh"
#include "rflx.hh"
#include "shoebox.hh"
#include "timing.hh"

namespace mxi {

namespace {

void usage(const char *program) {
  std::printf(
      "usage: %s [options] EXPT REFL\n"
      "\n"
      "  --out FILE        table of model against observation (forward.txt)\n"
      "  --sigma-b B --sigma-m M   use these instead of estimating\n"
      "  --no-sensor       leave the absorption depth out of the model\n"
      "  --depth-samples N samples through the sensor; 1 is the mean depth\n"
      "                    alone, which is the parallax correction and no\n"
      "                    smear (8)\n"
      "  --timing          where the time goes\n",
      program);
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {"--timing",    "--out",
                                       "--sigma-b",   "--sigma-m",
                                       "--no-sensor", "--depth-samples"};
  std::set<std::string> takes_value = known;
  takes_value.erase("--no-sensor");
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
    std::fprintf(stderr, "mxi_forward: %s\n", args.error.c_str());
    return 2;
  }
  if (args.positional.size() != 2) {
    std::fprintf(stderr, "mxi_forward: expected an .expt and a .refl\n");
    usage(argv[0]);
    return 2;
  }

  try {
    const ExperimentList experiments = read_experiments(args.positional[0]);
    const Table t = read_reflections(args.positional[1]);
    timing.add("reading", Timing::now() - mark);
    mark = Timing::now();
    const std::vector<Shoebox> boxes = decode_shoeboxes(t);
    if (boxes.empty()) {
      std::fprintf(stderr, "mxi_forward: %s has no shoeboxes\n",
                   args.positional[1].c_str());
      return 1;
    }
    const Experiment &e = experiments[0];
    const Column &s = t.at("s1");
    const Column &cal = t.at("xyzcal.mm");

    std::vector<std::size_t> chosen;
    std::vector<Vec3> beams;
    std::vector<RangeSample> samples;
    for (std::size_t i = 0; i < t.nrows && i < boxes.size(); ++i) {
      if (t.has("flags") &&
          (t.at("flags").integer(i) & flag::kUsedInRefinement) == 0) {
        continue;
      }
      const Vec3 beam{s.real(i, 0), s.real(i, 1), s.real(i, 2)};
      if (std::fabs(compute_zeta(e, beam)) < 0.05)
        continue;
      if (!has_prediction(t, i))
        continue;
      chosen.push_back(i);
      beams.push_back(beam);
      for (const RangeSample &sample :
           range_samples(e, boxes[i], cal.real(i, 2), compute_zeta(e, beam))) {
        samples.push_back(sample);
      }
    }

    ForwardOptions options;
    options.sigma_d = args.number("--sigma-b", 0.0);
    options.sigma_m = args.number("--sigma-m", 0.0);
    options.sensor = !args.has("--no-sensor");
    options.depth_samples = static_cast<int>(args.number("--depth-samples", 8));
    if (!(options.sigma_d > 0.0)) {
      std::vector<Shoebox> selected;
      for (std::size_t i : chosen)
        selected.push_back(boxes[i]);
      std::size_t used = 0;
      options.sigma_d = beam_divergence(e, selected, beams, &used);
    }
    if (!(options.sigma_m > 0.0)) {
      options.sigma_m =
          reflecting_range(samples, Scan::radians(e.scan.osc_width), 0.0);
    }
    std::printf("sigma_b %.6f  sigma_m %.6f  sensor %s (%d depths)\n",
                options.sigma_d, options.sigma_m,
                options.sensor ? "modelled" : "off", options.depth_samples);
    std::printf("%zu spots\n", chosen.size());

    const std::string path = args.value("--out", "forward.txt");
    std::FILE *out = std::fopen(path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "mxi_forward: cannot write %s\n", path.c_str());
      return 1;
    }
    std::fprintf(out, "# obliquity_deg counts shift_fast shift_slow shift_z "
                      "obs_wf obs_ws obs_wz mod_wf mod_ws mod_wz\n");

    const Panel &p0 = e.detector[0];
    const Vec3 normal = p0.fast.cross(p0.slow).normalized();
    std::size_t written = 0;
    for (std::size_t k = 0; k < chosen.size(); ++k) {
      const std::size_t i = chosen[k];
      const Shoebox &box = boxes[i];
      const std::vector<double> model =
          render_shoebox(e, box, beams[k], cal.real(i, 2), options);
      if (model.empty())
        continue;

      std::vector<double> observed(box.size(), 0.0);
      for (std::size_t j = 0; j < box.size(); ++j) {
        observed[j] = static_cast<double>(box.data[j]);
      }
      const Moments a = moments_of(box, observed, true);
      const Moments b = moments_of(box, model, false);
      if (!a.valid || !b.valid || a.total <= 50.0)
        continue;

      const Vec3 direction = beams[k] / beams[k].norm();
      const double obliquity =
          std::acos(std::fmin(1.0, std::fabs(direction.dot(normal))));
      std::fprintf(
          out, "%.5f %.6g %+.6f %+.6f %+.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
          Scan::degrees(obliquity), a.total, a.com_fast - b.com_fast,
          a.com_slow - b.com_slow, a.com_z - b.com_z, a.width_fast,
          a.width_slow, a.width_z, b.width_fast, b.width_slow, b.width_z);
      ++written;
    }
    std::fclose(out);
    std::printf("wrote %s, %zu spots compared\n", path.c_str(), written);
    timing.add("computing, printing and writing", Timing::now() - mark);
    timing.report(stdout);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_forward: %s\n", e.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_forward.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_forward.log");
  return mxi::run_program(argc, argv);
}

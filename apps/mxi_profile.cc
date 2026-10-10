// mxi_profile: estimate the Gaussian profile model from indexed strong spots.
//
//   mxi_profile refined.expt refined.refl
//
// The reflection table must carry its shoeboxes, which is what dials.find_spots
// writes and what this package now preserves. A table whose shoeboxes have been
// stripped cannot be used: the estimate is made from the pixels.

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
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
      "Estimate the Gaussian profile model from the strong spots' shoeboxes.\n"
      "\n"
      "  --all             use every reflection, not only the strong ones\n"
      "  --n-sigma N       width of the integration region, in sigmas (3)\n"
      "  --min-zeta Z      drop reflections whose zeta is below Z (0.05)\n"
      "  --compare --compare-sigma-b B --compare-sigma-m M\n"
      "                    also report the captured fraction for another "
      "pair,\n"
      "                    to see which of them the data prefers\n"
      "  --timing          where the time goes\n",
      program);
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {"--timing",         "--all",
                                       "--n-sigma",        "--min-zeta",
                                       "--compare",        "--compare-sigma-b",
                                       "--compare-sigma-m"};
  const std::set<std::string> takes_value = {
      "--n-sigma", "--min-zeta", "--compare-sigma-b", "--compare-sigma-m"};
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
    std::fprintf(stderr, "mxi_profile: %s\n", args.error.c_str());
    return 2;
  }
  if (args.positional.size() != 2) {
    std::fprintf(stderr, "mxi_profile: expected an .expt and a .refl\n");
    usage(argv[0]);
    return 2;
  }

  try {
    const ExperimentList experiments = read_experiments(args.positional[0]);
    const Table reflections = read_reflections(args.positional[1]);
    timing.add("reading", Timing::now() - mark);
    mark = Timing::now();
    if (experiments.size() == 0) {
      std::fprintf(stderr, "mxi_profile: no experiments\n");
      return 1;
    }

    const std::vector<Shoebox> boxes = decode_shoeboxes(reflections);
    if (boxes.empty()) {
      std::fprintf(stderr,
                   "mxi_profile: %s has no shoeboxes, and the profile model is "
                   "estimated from the pixels. Use a table that still has "
                   "them; dials.find_spots writes them and this package keeps "
                   "them.\n",
                   args.positional[1].c_str());
      return 1;
    }
    if (!reflections.has("s1")) {
      std::fprintf(stderr,
                   "mxi_profile: %s has no s1 column, so there is nothing to "
                   "measure the spread of each spot about\n",
                   args.positional[1].c_str());
      return 1;
    }

    // Strong reflections only by default, which is what the estimate is
    // defined over. Saying how many were dropped matters: a mean over a
    // different set of spots is a different number.
    // DIALS selects the reflections used in refinement, then cuts on zeta.
    // That is not a detail: on 1800 images of insulin it moves sigma_b from
    // -4.2 per cent of the DIALS value to -2.9, and sigma_M from a factor of
    // three to twenty per cent.
    const bool everything = args.has("--all");
    const double min_zeta = args.number("--min-zeta", 0.05);
    const Column &s = reflections.at("s1");
    const bool has_flags = reflections.has("flags");
    std::vector<Shoebox> selected;
    std::vector<Vec3> s1;
    std::vector<std::size_t> chosen;
    for (std::size_t i = 0; i < reflections.nrows && i < boxes.size(); ++i) {
      const Vec3 beam{s.real(i, 0), s.real(i, 1), s.real(i, 2)};
      if (!everything && has_flags &&
          (reflections.at("flags").integer(i) & flag::kUsedInRefinement) == 0) {
        continue;
      }
      if (std::fabs(compute_zeta(experiments[0], beam)) < min_zeta)
        continue;
      selected.push_back(boxes[i]);
      s1.push_back(beam);
      chosen.push_back(i);
    }

    std::printf("%zu reflections, %zu with shoeboxes, %zu %s\n",
                reflections.nrows, boxes.size(), selected.size(),
                everything ? "used" : "used in refinement and above min-zeta");

    std::size_t used = 0;
    ProfileModel model;
    model.n_sigma = args.number("--n-sigma", 3.0);
    model.sigma_d = beam_divergence(experiments[0], selected, s1, &used);
    model.n_used = used;

    std::printf("sigma_D (beam divergence) %.9f degrees, from %zu spots\n",
                model.sigma_d, model.n_used);
    // sigma_M, from one sample per image each spot was seen on.
    std::vector<RangeSample> samples;
    if (reflections.has("xyzcal.mm")) {
      const Column &cal = reflections.at("xyzcal.mm");
      for (std::size_t i : chosen) {
        // A row with no prediction carries uninitialised xyzcal -- denormals,
        // not zeros -- and feeding those to the likelihood put the estimate
        // out by a factor of four.
        if (!has_prediction(reflections, i))
          continue;
        // Kabsch step (vii): reject a spot whose observed centroid is far from
        // where the model puts it. Without this the estimate is 0.506 rather
        // than 0.293 degrees, because a handful of spots whose shoebox sits
        // hundreds of images from their predicted angle dominate a likelihood
        // that falls off quadratically.
        //
        // Kabsch says "deviates too much" without a number. Measured here, the
        // answer is flat at 0.293 for any cut between one and ten images and
        // moves only outside that, so within the plateau this is not a knob.

        const Vec3 beam{s.real(i, 0), s.real(i, 1), s.real(i, 2)};
        const double zeta = compute_zeta(experiments[0], beam);
        for (const RangeSample &sample :
             range_samples(experiments[0], boxes[i], cal.real(i, 2), zeta)) {
          samples.push_back(sample);
        }
      }
      model.sigma_m = reflecting_range(
          samples, Scan::radians(experiments[0].scan.osc_width), 0.0);
      std::printf("sigma_M (reflecting range) %.9f degrees, from %zu images\n",
                  model.sigma_m, samples.size());
    } else {
      std::printf("sigma_M needs xyzcal.mm, which this table does not have\n");
    }
    std::printf("n_sigma %.1f\n", model.n_sigma);

    // The test that does not depend on agreeing with anyone: a Gaussian taken
    // to three sigma holds essentially all of its density. If one sigma
    // already holds it, the sigmas are too large.
    if (!samples.empty() && reflections.has("xyzcal.mm")) {
      const Column &cal = reflections.at("xyzcal.mm");
      std::vector<double> phi;
      for (std::size_t i : chosen)
        phi.push_back(cal.real(i, 2));

      const auto report = [&](const char *label, double sd, double sm) {
        const Capture c =
            capture_fractions(experiments[0], selected, s1, phi, sd, sm);
        std::printf("  %-10s detector  %7.4f %7.4f %7.4f %7.4f\n", label,
                    c.detector[0], c.detector[1], c.detector[2], c.detector[3]);
        std::printf("  %-10s rotation  %7.4f %7.4f %7.4f %7.4f\n", label,
                    c.rotation[0], c.rotation[1], c.rotation[2], c.rotation[3]);
        std::printf("  %-10s both      %7.4f %7.4f %7.4f %7.4f\n", label,
                    c.fraction[0], c.fraction[1], c.fraction[2], c.fraction[3]);
      };
      std::printf("\nfraction of counts within n sigma, over %zu spots\n",
                  selected.size());
      std::printf("  a one-dimensional Gaussian holds 0.6827, 0.9545, 0.9973, "
                  "0.99994\n");
      std::printf("  %-10s %-9s %7s %7s %7s %7s\n", "", "", "1", "2", "3", "4");
      report("ours", model.sigma_d, model.sigma_m);
      if (args.has("--compare")) {
        const double other_b = args.number("--compare-sigma-b", 0.0);
        const double other_m = args.number("--compare-sigma-m", 0.0);
        if (other_b > 0.0 && other_m > 0.0)
          report("given", other_b, other_m);
      }
      std::printf("  A shoebox is small, so at three or four sigma the BOX may "
                  "run out\n"
                  "  before the model does. A fraction near one there can mean "
                  "the box\n"
                  "  ended, not that the model held.\n");
    }
    // Said here rather than only in the documentation, because a number that
    // disagrees with DIALS and does not say so is worse than no number -- but
    // in terms true of any data. It quoted one data set's numbers, insulin's,
    // whatever it was given.
    std::printf("\nNOTE: these are not computed as dials.integrate computes "
                "its own, and\n"
                "will not agree with it exactly; docs/integration.md says how "
                "and why.\n");
    timing.add("computing, printing and writing", Timing::now() - mark);
    timing.report(stdout);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_profile: %s\n", e.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_profile.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_profile.log");
  return mxi::run_program(argc, argv);
}

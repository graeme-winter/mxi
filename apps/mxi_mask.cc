// mxi_mask: predict reflections and mark their integration regions, so the
// profile model can be looked at against the images.
//
//   mxi_mask refined.expt refined.refl -o masked.refl
//   dials.image_viewer refined.expt masked.refl
//
// The shoeboxes carry no counts. Nothing here has read an image, and invented
// values would be worse than none: what this shows is WHERE the model says the
// signal is, drawn over the real image by the viewer.

#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
#include "log_mirror.hh"
#include "mask.hh"
#include "predict.hh"
#include "profile_model.hh"
#include "rflx.hh"
#include "shoebox.hh"
#include "timing.hh"

namespace mxi {

namespace {

void usage(const char *program) {
  std::printf(
      "usage: %s [options] EXPT [REFL]\n"
      "\n"
      "Predict reflections and mark the n-sigma region of each in its mask,\n"
      "for dials.image_viewer. Pixel values are left at zero.\n"
      "\n"
      "  -o FILE           where to write (masked.refl)\n"
      "  --n-sigma N       the foreground spans plus and minus N sigma (3)\n"
      "  --box-scale S     the box is S times wider than the foreground on "
      "the\n"
      "                    detector, to hold background (1.9)\n"
      "  --sigma-b B --sigma-m M   use these instead of estimating from REFL\n"
      "  --d-min D         resolution limit for prediction\n"
      "  --shape box|ellipsoid   the box is Kabsch's mask, each coordinate\n"
      "                    separately within n sigma; the ellipsoid is the\n"
      "                    surface the Gaussian is actually constant on, and "
      "is\n"
      "                    pi/6 of the box (box)\n"
      "  --min-zeta Z      skip reflections whose zeta is below Z (0.05)\n"
      "  --first-image N --last-image N   restrict to part of the scan; a "
      "whole\n"
      "                    sweep of shoeboxes does not fit in memory\n"
      "  --timing          where the time goes\n",
      program);
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {
      "--timing",      "-o",           "--n-sigma",  "--sigma-b",
      "--sigma-m",     "--d-min",      "--min-zeta", "--box-scale",
      "--first-image", "--last-image", "--shape"};
  std::set<std::string> takes_value = known;
  takes_value.erase("--timing");
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
    std::fprintf(stderr, "mxi_mask: %s\n", args.error.c_str());
    return 2;
  }
  if (args.positional.empty() || args.positional.size() > 2) {
    std::fprintf(stderr,
                 "mxi_mask: expected an .expt and optionally a .refl\n");
    usage(argv[0]);
    return 2;
  }

  try {
    ExperimentList experiments = read_experiments(args.positional[0]);
    timing.add("reading", Timing::now() - mark);
    mark = Timing::now();
    if (experiments.size() == 0) {
      std::fprintf(stderr, "mxi_mask: no experiments\n");
      return 1;
    }
    const Experiment &e = experiments[0];

    MaskOptions options;
    options.n_sigma = args.number("--n-sigma", 3.0);
    options.box_scale = args.number("--box-scale", 1.9);
    options.min_zeta = args.number("--min-zeta", 0.05);
    if (args.value("--shape", "box") == "ellipsoid") {
      options.shape = RegionShape::kEllipsoid;
    } else if (args.value("--shape", "box") != "box") {
      std::fprintf(stderr, "mxi_mask: --shape is box or ellipsoid\n");
      return 2;
    }
    options.sigma_d = args.number("--sigma-b", 0.0);
    options.sigma_m = args.number("--sigma-m", 0.0);

    // Estimated from the strong spots unless given, which is the same
    // calculation mxi_profile does and reports.
    if (!(options.sigma_d > 0.0) || !(options.sigma_m > 0.0)) {
      if (args.positional.size() < 2) {
        std::fprintf(stderr,
                     "mxi_mask: give a .refl to estimate the profile model "
                     "from, or --sigma-b and --sigma-m\n");
        return 2;
      }
      const Table strong = read_reflections(args.positional[1]);
      const std::vector<Shoebox> boxes = decode_shoeboxes(strong);
      if (boxes.empty()) {
        std::fprintf(stderr,
                     "mxi_mask: %s has no shoeboxes, so the profile model "
                     "cannot be estimated from it\n",
                     args.positional[1].c_str());
        return 1;
      }
      const Column &s = strong.at("s1");
      const Column &cal = strong.at("xyzcal.mm");
      std::vector<Shoebox> selected;
      std::vector<Vec3> beams;
      std::vector<RangeSample> samples;
      for (std::size_t i = 0; i < strong.nrows && i < boxes.size(); ++i) {
        if (strong.has("flags") &&
            (strong.at("flags").integer(i) & flag::kUsedInRefinement) == 0) {
          continue;
        }
        const Vec3 beam{s.real(i, 0), s.real(i, 1), s.real(i, 2)};
        const double zeta = compute_zeta(e, beam);
        if (std::fabs(zeta) < options.min_zeta)
          continue;
        if (!has_prediction(strong, i))
          continue;
        selected.push_back(boxes[i]);
        beams.push_back(beam);
        for (const RangeSample &sample :
             range_samples(e, boxes[i], cal.real(i, 2), zeta)) {
          samples.push_back(sample);
        }
      }
      std::size_t used = 0;
      if (!(options.sigma_d > 0.0)) {
        options.sigma_d = beam_divergence(e, selected, beams, &used);
      }
      if (!(options.sigma_m > 0.0)) {
        options.sigma_m =
            reflecting_range(samples, Scan::radians(e.scan.osc_width), 0.0);
      }
      std::printf("profile model from %zu spots\n", selected.size());
    }
    std::printf("sigma_b %.6f  sigma_m %.6f  n_sigma %.1f  shape %s\n",
                options.sigma_d, options.sigma_m, options.n_sigma,
                options.shape == RegionShape::kEllipsoid ? "ellipsoid" : "box");
    if (!(options.sigma_d > 0.0) || !(options.sigma_m > 0.0)) {
      std::fprintf(stderr, "mxi_mask: the profile model came out empty\n");
      return 1;
    }

    PredictOptions predict_options;
    predict_options.d_min = args.number("--d-min", 0.0);
    const std::vector<Prediction> predictions = predict(e, predict_options);
    std::printf("%zu reflections predicted\n", predictions.size());

    // An image range, because a whole sweep of shoeboxes does not fit in
    // memory and is not what anyone looks at. A thousand images of insulin at
    // 1.6 Angstrom is several hundred thousand boxes and a gigabyte of empty
    // pixels; the first attempt at this was killed by the machine.
    const double first_image = args.number("--first-image", 0.0);
    const double last_image =
        args.number("--last-image", static_cast<double>(e.scan.num_images()));

    Table out;
    std::string blob;
    std::vector<const Prediction *> kept;
    std::vector<std::array<std::int32_t, 6>> bboxes;
    std::size_t skipped_zeta = 0, skipped_box = 0, skipped_range = 0;
    for (const Prediction &p : predictions) {
      if (p.z < first_image || p.z > last_image) {
        ++skipped_range;
        continue;
      }
      Shoebox box;
      if (!build_shoebox(e, p, options, &box)) {
        const KabschFrame frame = kabsch_frame(e, p.s1);
        if (!frame.valid || std::fabs(frame.zeta) < options.min_zeta) {
          ++skipped_zeta;
        } else {
          ++skipped_box;
        }
        continue;
      }
      // Encoded and discarded one at a time. Keeping the boxes and their
      // encoding at once doubles the peak for no reason.
      blob += encode_shoeboxes({box});
      std::array<std::int32_t, 6> b;
      for (int k = 0; k < 6; ++k)
        b[k] = box.bbox[k];
      bboxes.push_back(b);
      kept.push_back(&p);
    }
    std::printf(
        "%zu shoeboxes; %zu outside the image range, %zu skipped for zeta, "
        "%zu for leaving the scan\n",
        kept.size(), skipped_range, skipped_zeta, skipped_box);
    if (kept.empty()) {
      std::fprintf(stderr, "mxi_mask: nothing to write\n");
      return 1;
    }

    out.nrows = kept.size();
    Column &miller =
        out.int_column("miller_index", "cctbx::miller::index<>", 3);
    Column &panel = out.int_column("panel", "std::size_t", 1);
    Column &id = out.int_column("id", "int", 1);
    Column &imageset = out.int_column("imageset_id", "int", 1);
    Column &flags = out.int_column("flags", "std::size_t", 1);
    Column &entering = out.int_column("entering", "bool", 1);
    Column &bbox = out.int_column("bbox", "int6", 6);
    Column &cal_px = out.real_column("xyzcal.px", "vec3<double>", 3);
    Column &cal_mm = out.real_column("xyzcal.mm", "vec3<double>", 3);
    Column &s1 = out.real_column("s1", "vec3<double>", 3);

    const Vec3 s0 = e.beam.s0();
    const Vec3 axis = e.goniometer.lab_axis();
    for (std::size_t i = 0; i < kept.size(); ++i) {
      const Prediction &p = *kept[i];
      miller.ints[i * 3 + 0] = p.h;
      miller.ints[i * 3 + 1] = p.k;
      miller.ints[i * 3 + 2] = p.l;
      panel.ints[i] = static_cast<std::int64_t>(p.panel);
      id.ints[i] = 0;
      imageset.ints[i] = 0;
      // Predicted, not observed: nothing here has seen an image.
      flags.ints[i] = flag::kPredicted | flag::kIndexed;
      entering.ints[i] = p.s1.dot(axis.cross(s0)) > 0.0 ? 1 : 0;
      for (int k = 0; k < 6; ++k)
        bbox.ints[i * 6 + k] = bboxes[i][k];
      cal_px.reals[i * 3 + 0] = p.px_fast;
      cal_px.reals[i * 3 + 1] = p.px_slow;
      cal_px.reals[i * 3 + 2] = p.z;
      const Panel &q = e.detector[p.panel];
      const auto mm = q.px_to_mm(p.px_fast, p.px_slow);
      cal_mm.reals[i * 3 + 0] = mm.first;
      cal_mm.reals[i * 3 + 1] = mm.second;
      cal_mm.reals[i * 3 + 2] = p.phi;
      for (int k = 0; k < 3; ++k)
        s1.reals[i * 3 + k] = p.s1[k];
    }

    Table::Opaque column;
    column.type = "Shoebox<>";
    column.bytes = std::move(blob);
    column.rows = kept.size();
    out.set_opaque("shoebox", std::move(column));
    // The identifier ties the table to the experiment; without it dials
    // refuses the pair rather than drawing anything.
    if (!e.identifier.empty())
      out.identifiers[0] = e.identifier;

    const std::string path = args.value("-o", "masked.refl");
    write_reflections(path, out);
    // How much of the box the region fills, and how much of the model's
    // density it holds, so the two shapes can be compared on the numbers as
    // well as by eye.
    std::size_t foreground = 0, voxels = 0;
    {
      const std::vector<Shoebox> written = decode_shoeboxes(out);
      for (const Shoebox &b : written) {
        voxels += b.size();
        for (std::uint8_t m : b.mask) {
          if (m & shoebox_mask::kForeground)
            ++foreground;
        }
      }
    }
    std::printf("%zu of %zu voxels marked (%.1f%%)\n", foreground, voxels,
                100.0 * static_cast<double>(foreground) /
                    static_cast<double>(voxels));
    std::printf(
        "a three-dimensional Gaussian holds %.4f inside the box and %.4f\n"
        "inside the ellipsoid at n = 3\n",
        0.99187, 0.97071);
    std::printf("wrote %s\n", path.c_str());
    std::printf("\n  dials.image_viewer %s %s\n", args.positional[0].c_str(),
                path.c_str());
    timing.add("computing, printing and writing", Timing::now() - mark);
    timing.report(stdout);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_mask: %s\n", e.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_mask.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_mask.log");
  return mxi::run_program(argc, argv);
}

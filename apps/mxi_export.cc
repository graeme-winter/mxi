// mxi_export: an unmerged MTZ, as dials.export writes one -- the design and
// its sources in DIALS are docs/export.md.

#include <gemmi/mtz.hpp>
#include <gemmi/symmetry.hpp>
#include <gemmi/unitcell.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "args.hh"
#include "expt.hh"
#include "log_mirror.hh"
#include "refl.hh"
#include "rflx.hh"

namespace mxi {

namespace {

constexpr std::int64_t kIntegratedSum = 1 << 8;
constexpr std::int64_t kIntegratedPrf = 1 << 9;
// dials' bad_for_scaling: outlier in scaling, excluded, excluded by the user.
constexpr std::int64_t kBadForScaling = (1 << 23) | (1 << 24) | (1 << 25);

void usage() {
  std::printf(
      "usage: mxi_export [options] EXPT REFL\n"
      "  An unmerged MTZ, as dials.export writes one: a row an observation, a\n"
      "  batch header an image, one sweep or several (docs/export.md). A "
      "scaled\n"
      "  table gives the scaled intensities; an integrated one the profile-\n"
      "  fitted and summed, corrected for LP, QE and -- summed -- partiality.\n"
      "  -o, --output PATH          the MTZ (scaled.mtz, integrated.mtz)\n"
      "  --partiality-threshold P   leave out partiality below P (0.4)\n"
      "  --min-isigi S              leave out I/sigma below S (-5)\n"
      "  --d-min D                  leave out d below D\n"
      "  --crystal-name NAME        (XTAL)\n"
      "  --project-name NAME        (mxi)\n");
}

double degrees(double radians) { return radians * 180.0 / M_PI; }

// The angle between two vectors, in radians.
double angle_between(const Vec3 &a, const Vec3 &b) {
  const double c = a.dot(b) / (a.norm() * b.norm());
  return std::acos(std::fmax(-1.0, std::fmin(1.0, c)));
}

// rstbx's align_reference_frame: R taking primary onto primary_target, then
// about primary_target bringing secondary as near secondary_target as it goes.
Mat3 align_reference_frame(Vec3 primary, Vec3 primary_target, Vec3 secondary,
                           Vec3 secondary_target) {
  primary = primary.normalized();
  primary_target = primary_target.normalized();
  secondary = secondary.normalized();
  secondary_target = secondary_target.normalized();
  Mat3 r1 = Mat3::identity();
  const double a1 = angle_between(primary_target, primary);
  if (std::fmod(a1, M_PI) != 0.0) {
    r1 = rotation(primary_target.cross(primary), -a1);
  } else if (primary_target.dot(primary) < 0.0) {
    // A vector at right angles to primary, as scitbx's ortho().
    const Vec3 o =
        std::fabs(primary.x) < 0.9 ? Vec3{1.0, 0.0, 0.0} : Vec3{0.0, 1.0, 0.0};
    r1 = rotation(primary.cross(o).normalized(), M_PI);
  }
  const Vec3 turned = r1 * secondary;
  if (std::fabs(angle_between(secondary_target, turned)) < 1.0e-6)
    return r1;
  const Vec3 axis_r = secondary_target.cross(turned);
  const Vec3 axis_s = primary_target;
  const auto orthogonal = [&](const Vec3 &v) {
    return v - axis_s * v.dot(axis_s);
  };
  const double between =
      angle_between(orthogonal(secondary_target), orthogonal(turned));
  const double r_angle =
      axis_r.norm() > 0.0 ? angle_between(axis_r, primary_target) : 0.0;
  const double a2 = r_angle > 0.5 * M_PI ? between : -between;
  return rotation(axis_s, a2) * r1;
}

struct Cell {
  double a, b, c, alpha, beta, gamma; // A, degrees
};

Cell cell_of(const Mat3 &A) {
  const UnitCell u = [&] {
    Crystal c;
    c.A = A;
    return c.cell();
  }();
  return {u.a, u.b, u.c, u.alpha, u.beta, u.gamma};
}

// cctbx's orthogonalization matrix: real-space axes as its columns.
Mat3 orthogonalization(const Cell &c) {
  const double ca = std::cos(c.alpha * M_PI / 180.0),
               cb = std::cos(c.beta * M_PI / 180.0),
               cg = std::cos(c.gamma * M_PI / 180.0),
               sg = std::sin(c.gamma * M_PI / 180.0);
  const double v =
      c.a * c.b * c.c *
      std::sqrt(1.0 - ca * ca - cb * cb - cg * cg + 2 * ca * cb * cg);
  return {c.a, c.b * cg, c.c * cb,
          0.0, c.b * sg, c.c * (ca - cb * cg) / sg,
          0.0, 0.0,      v / (c.a * c.b * sg)};
}

// dxtbx's B, the fractionalization matrix's transpose: A = U B.
Mat3 b_matrix(const Cell &c) {
  return orthogonalization(c).inverse().transpose();
}

Cell cell_from_b(const Mat3 &B) {
  const Mat3 o = B.transpose().inverse(); // columns: a, b, c
  const Vec3 a{o.m[0], o.m[3], o.m[6]}, b{o.m[1], o.m[4], o.m[7]},
      c{o.m[2], o.m[5], o.m[8]};
  return {a.norm(),
          b.norm(),
          c.norm(),
          degrees(angle_between(b, c)),
          degrees(angle_between(a, c)),
          degrees(angle_between(a, b))};
}

// dials' ub_to_mosflm_u: U = UB B^-1, B Busing and Levy's from the reciprocal
// cell.
Mat3 mosflm_u(const Mat3 &UB, const Cell &c) {
  const Mat3 o = orthogonalization(c);
  const Mat3 g = o.transpose() * o; // real-space metric
  const Mat3 gs = g.inverse();      // reciprocal
  const double as = std::sqrt(gs.m[0]), bs = std::sqrt(gs.m[4]),
               cs = std::sqrt(gs.m[8]);
  const double cos_beta_s = gs.m[2] / (as * cs),
               cos_gamma_s = gs.m[1] / (as * bs);
  const double sin_beta_s = std::sqrt(1.0 - cos_beta_s * cos_beta_s),
               sin_gamma_s = std::sqrt(1.0 - cos_gamma_s * cos_gamma_s);
  const double cos_alpha = std::cos(c.alpha * M_PI / 180.0);
  const Mat3 B{as,
               bs * cos_gamma_s,
               cs * cos_beta_s,
               0.0,
               bs * sin_gamma_s,
               -cs * sin_beta_s * cos_alpha,
               0.0,
               0.0,
               1.0 / c.c};
  return UB * B.inverse();
}

// The rotation half way along R, about its own axis.
Mat3 half_rotation(const Mat3 &r) {
  const double angle = rotation_angle(r);
  if (angle < 1e-12)
    return Mat3::identity();
  const Vec3 axis{r.m[7] - r.m[5], r.m[2] - r.m[6], r.m[3] - r.m[1]};
  return rotation(axis.normalized(), 0.5 * angle);
}

// dials' _calculate_batch_offsets.
std::int64_t next_epoch(std::int64_t v) {
  if (v % 100 == 99)
    return v + 2;
  if (v % 100 == 0)
    return v + 101;
  return v - v % 100 + 101;
}

std::vector<std::int64_t> batch_offsets(
    const std::vector<std::pair<std::int64_t, std::int64_t>> &ranges) {
  std::vector<std::int64_t> offsets(ranges.size(), 0);
  std::vector<std::pair<std::int64_t, std::int64_t>> kept;
  std::vector<std::size_t> shift;
  std::int64_t highest = 0;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    std::int64_t lo = ranges[i].first, hi = ranges[i].second;
    if (lo == 0) {
      ++lo;
      ++hi;
    }
    bool overlaps = false;
    for (const auto &[l, h] : kept)
      overlaps = overlaps || (lo < h + 1 && hi >= l - 1);
    if (overlaps) {
      shift.push_back(i);
    } else {
      offsets[i] = lo - ranges[i].first;
      kept.emplace_back(lo, hi);
      highest = std::max(highest, hi);
    }
  }
  for (std::size_t i : shift) {
    const std::int64_t start = next_epoch(highest);
    offsets[i] = start - ranges[i].first;
    highest = start + (ranges[i].second - ranges[i].first);
  }
  return offsets;
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {
      "-o",      "--output",       "--partiality-threshold", "--min-isigi",
      "--d-min", "--crystal-name", "--project-name"};
  Arguments args = parse_arguments(argc, argv, known, known);
  // One .rflx stands for the experiment list and the reflections
  // (docs/rflx.md).
  args.positional = rflx::as_pair(args.positional);
  if (args.help) {
    usage();
    return 0;
  }
  if (!args.ok || args.positional.size() != 2) {
    if (!args.ok)
      std::fprintf(stderr, "mxi_export: %s\n", args.error.c_str());
    usage();
    return 2;
  }
  try {
    ExperimentList experiments = read_experiments(args.positional[0]);
    const Table in = read_reflections(args.positional[1]);
    const std::size_t sweeps = experiments.size();
    if (sweeps == 0)
      throw std::runtime_error("no experiments");
    std::string hall;
    for (std::size_t i = 0; i < sweeps; ++i) {
      if (!experiments[i].crystal)
        throw std::runtime_error("experiment " + std::to_string(i) +
                                 " has no crystal");
      if (i == 0)
        hall = experiments[i].crystal->space_group_hall;
      else if (experiments[i].crystal->space_group_hall != hall)
        throw std::runtime_error(
            "the experiments do not share one space group");
    }
    const gemmi::SpaceGroup *group =
        gemmi::find_spacegroup_by_ops(gemmi::symops_from_hall(hall.c_str()));
    if (!group)
      throw std::runtime_error("space group '" + hall +
                               "' is not in a tabulated setting");

    const bool scaled = in.has("intensity.scale.value") &&
                        in.has("intensity.scale.variance") &&
                        in.has("inverse_scale_factor");
    const std::string output = args.value(
        "-o", args.value("--output", scaled ? "scaled.mtz" : "integrated.mtz"));
    const double partiality_threshold =
        args.number("--partiality-threshold", 0.4);
    const double min_isigi = args.number("--min-isigi", -5.0);
    const double d_min = args.number("--d-min", 0.0);

    // The Cambridge frame for each sweep: the rotation axis along +Z, the
    // beam along +X, everything turned with them.
    std::vector<Mat3> SF(sweeps), A(sweeps);
    std::vector<std::vector<Mat3>> A_points(sweeps);
    std::vector<Vec3> axis(sweeps), source(sweeps);
    for (std::size_t i = 0; i < sweeps; ++i) {
      const Experiment &e = experiments[i];
      const Vec3 unit_s0 = (e.beam.direction * -1.0).normalized();
      const Mat3 R = align_reference_frame(
          e.goniometer.axis, Vec3{0.0, 0.0, 1.0}, unit_s0, Vec3{1.0, 0.0, 0.0});
      const Mat3 S = R * e.goniometer.setting * R.transpose();
      const Mat3 F = R * e.goniometer.fixed * R.transpose();
      SF[i] = S * F;
      A[i] = R * e.crystal->A;
      for (const Mat3 &a : e.crystal->A_points)
        A_points[i].push_back(R * a);
      axis[i] = S * (R * e.goniometer.axis);
      source[i] = (R * e.beam.direction).normalized();
    }

    // The file's cell: the median of each parameter over the sweeps.
    std::vector<double> params[6];
    for (std::size_t i = 0; i < sweeps; ++i) {
      const Cell c = cell_of(experiments[i].crystal->A);
      const double p[6] = {c.a, c.b, c.c, c.alpha, c.beta, c.gamma};
      for (int k = 0; k < 6; ++k)
        params[k].push_back(p[k]);
    }
    const Cell best{median(params[0]), median(params[1]), median(params[2]),
                    median(params[3]), median(params[4]), median(params[5])};
    const Mat3 best_o = orthogonalization(best);
    const Mat3 best_gs = (best_o.transpose() * best_o).inverse();

    // Batch offsets, as dials': each sweep's images its batches unless they
    // overlap another's.
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    for (const Experiment &e : experiments)
      ranges.emplace_back(e.scan.first_image, e.scan.last_image);
    const std::vector<std::int64_t> offsets = batch_offsets(ranges);

    // Datasets: one a wavelength, within 1e-4 A.
    std::vector<double> wavelengths;
    std::vector<int> dataset_of(sweeps);
    for (std::size_t i = 0; i < sweeps; ++i) {
      const double w = experiments[i].beam.wavelength;
      std::size_t k = 0;
      while (k < wavelengths.size() && std::fabs(wavelengths[k] - w) > 1e-4)
        ++k;
      if (k == wavelengths.size())
        wavelengths.push_back(w);
      dataset_of[i] = static_cast<int>(k) + 1;
    }

    // The rows: which, and their corrected intensities.
    const Column &flags = in.at("flags");
    const Column &hkl = in.at("miller_index");
    const bool has_id = in.has("id");
    const auto real = [&](const char *name, std::size_t i, double fallback) {
      return in.has(name) ? in.at(name).real(i) : fallback;
    };
    struct Row {
      float h, k, l, batch;
      std::vector<float> values;
    };
    std::vector<Row> rows;
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < in.nrows; ++i) {
      const std::int64_t f = flags.integer(i);
      const std::int64_t id = has_id ? in.at("id").integer(i) : 0;
      if (id < 0 || static_cast<std::size_t>(id) >= sweeps) {
        ++dropped;
        continue;
      }
      const Experiment &e = experiments[static_cast<std::size_t>(id)];
      const Vec3 h{static_cast<double>(hkl.ints[i * 3]),
                   static_cast<double>(hkl.ints[i * 3 + 1]),
                   static_cast<double>(hkl.ints[i * 3 + 2])};
      const double d = 1.0 / std::sqrt(h.dot(best_gs * h));
      const double partiality = real("partiality", i, 1.0);
      const double lp = real("lp", i, 1.0);
      const double qe = real("qe", i, 1.0);
      std::vector<float> values;
      bool keep = partiality > 0.0 && partiality >= partiality_threshold &&
                  !(d_min > 0.0 && d <= d_min);
      if (scaled) {
        const double isf = in.at("inverse_scale_factor").real(i);
        const double v = in.at("intensity.scale.variance").real(i);
        keep = keep && (f & kBadForScaling) == 0 && isf > 0.0 && v > 0.0;
        if (keep) {
          const double I = in.at("intensity.scale.value").real(i) / isf;
          const double sigma = std::sqrt(v) / isf;
          keep = I / sigma >= min_isigi;
          const double var_isf = real("inverse_scale_factor_variance", i, 0.0);
          values = {static_cast<float>(I), static_cast<float>(sigma),
                    static_cast<float>(isf),
                    static_cast<float>(std::sqrt(std::fmax(var_isf, 0.0)))};
        }
      } else {
        const double pv = in.at("intensity.prf.variance").real(i);
        const double sv = in.at("intensity.sum.variance").real(i);
        keep = keep && (f & kIntegratedPrf) && (f & kIntegratedSum) &&
               pv > 0.0 && sv > 0.0 && qe > 0.0;
        if (keep) {
          const double conversion = lp / qe;
          const double sum_conversion = conversion / partiality;
          const double ip = in.at("intensity.prf.value").real(i) * conversion;
          const double sp = std::sqrt(pv) * conversion;
          const double is =
              in.at("intensity.sum.value").real(i) * sum_conversion;
          const double ss = std::sqrt(sv) * sum_conversion;
          keep = ip / sp >= min_isigi && is / ss >= min_isigi;
          values = {static_cast<float>(ip), static_cast<float>(sp),
                    static_cast<float>(is), static_cast<float>(ss)};
        }
      }
      if (!keep) {
        ++dropped;
        continue;
      }
      if (in.has("background.sum.value") && in.has("background.sum.variance")) {
        values.push_back(
            static_cast<float>(in.at("background.sum.value").real(i)));
        values.push_back(static_cast<float>(std::sqrt(
            std::fmax(in.at("background.sum.variance").real(i), 0.0))));
      }
      const Column &cal = in.at("xyzcal.px");
      const double z_obs = in.has("xyzobs.px.value")
                               ? in.at("xyzobs.px.value").reals[i * 3 + 2]
                               : cal.reals[i * 3 + 2];
      values.push_back(static_cast<float>(partiality));
      values.push_back(static_cast<float>(cal.reals[i * 3]));
      values.push_back(static_cast<float>(cal.reals[i * 3 + 1]));
      values.push_back(
          static_cast<float>(degrees(e.scan.phi_from_z(cal.reals[i * 3 + 2]))));
      values.push_back(static_cast<float>(lp));
      values.push_back(static_cast<float>(qe));
      const std::int64_t batch = static_cast<std::int64_t>(std::floor(z_obs)) +
                                 1 + offsets[static_cast<std::size_t>(id)];
      rows.push_back({static_cast<float>(h.x), static_cast<float>(h.y),
                      static_cast<float>(h.z), static_cast<float>(batch),
                      std::move(values)});
    }
    if (rows.empty())
      throw std::runtime_error("no reflections left to export");

    gemmi::Mtz mtz(true);
    mtz.title = "From mxi_export";
    {
      char when[64];
      const std::time_t now = std::time(nullptr);
      std::strftime(when, sizeof when, "%Y-%m-%d at %H:%M:%S UTC",
                    std::gmtime(&now));
      mtz.history.push_back(std::string("From mxi_export, run on ") + when);
    }
    mtz.spacegroup = group;
    for (double w : wavelengths) {
      gemmi::Mtz::Dataset &ds = mtz.add_dataset("FROMMXI");
      ds.crystal_name = args.value("--crystal-name", "XTAL");
      ds.project_name = args.value("--project-name", "mxi");
      ds.wavelength = w;
    }
    const int last = static_cast<int>(mtz.datasets.back().id);
    const auto column = [&](const char *label, char type) {
      mtz.add_column(label, type, last, -1, false);
    };
    column("M/ISYM", 'Y');
    column("BATCH", 'B');
    if (scaled) {
      column("I", 'J');
      column("SIGI", 'Q');
      column("SCALEUSED", 'R');
      column("SIGSCALEUSED", 'R');
    } else {
      column("IPR", 'J');
      column("SIGIPR", 'Q');
      column("I", 'J');
      column("SIGI", 'Q');
    }
    if (in.has("background.sum.value") && in.has("background.sum.variance")) {
      column("BG", 'R');
      column("SIGBG", 'R');
    }
    column("FRACTIONCALC", 'R');
    column("XDET", 'R');
    column("YDET", 'R');
    column("ROT", 'R');
    column("LP", 'R');
    column("QE", 'R');
    const std::size_t width = mtz.columns.size();
    std::vector<float> data;
    data.reserve(rows.size() * width);
    for (const Row &r : rows) {
      data.insert(data.end(), {r.h, r.k, r.l, 0.0f, r.batch});
      data.insert(data.end(), r.values.begin(), r.values.end());
    }
    // The rows hold their original indices: gemmi must be told so before they
    // are set -- with no data yet this only marks it, as dials.export does --
    // or putting them in the asymmetric unit below does nothing at all.
    mtz.switch_to_original_hkl();
    mtz.set_data(data.data(), data.size());

    // The batch headers, one an image.
    for (std::size_t i = 0; i < sweeps; ++i) {
      const Experiment &e = experiments[i];
      const Panel &panel = e.detector[0];
      const Vec3 normal = panel.fast.cross(panel.slow).normalized();
      gemmi::Mtz::Batch batch;
      batch.set_dataset_id(dataset_of[i]);
      batch.floats[86] = static_cast<float>(
          wavelengths[static_cast<std::size_t>(dataset_of[i] - 1)]);
      batch.ints[12] = 1;      // ncryst
      batch.ints[14] = 2;      // ldtype 3D
      batch.ints[15] = 1;      // jsaxs
      batch.ints[17] = 1;      // ngonax
      batch.ints[19] = 1;      // ndet
      batch.floats[21] = 0.0f; // mosaicity
      const double ax[3] = {axis[i].x, axis[i].y, axis[i].z};
      const double so[3] = {source[i].x, source[i].y, source[i].z};
      for (int j = 0; j < 3; ++j) {
        batch.floats[38 + j] = static_cast<float>(ax[j]); // scanax
        batch.floats[59 + j] = static_cast<float>(ax[j]); // e1
        batch.floats[83 + j] = static_cast<float>(so[j]); // source
      }
      batch.floats[43] = 1.0f; // batch scale
      const int smallest = static_cast<int>(std::min_element(so, so + 3) - so);
      batch.floats[80 + smallest] = -1.0f; // the idealised source
      batch.floats[111] = static_cast<float>(panel.origin.dot(normal));
      batch.floats[114] = static_cast<float>(panel.image_size[0]);
      batch.floats[116] = static_cast<float>(panel.image_size[1]);
      batch.axes = {"AXIS"};
      const std::int64_t n = e.scan.num_images();
      const bool varying =
          A_points[i].size() == static_cast<std::size_t>(n + 1);
      for (std::int64_t k = 0; k < n; ++k) {
        Mat3 ub;
        Cell cell;
        if (varying) {
          const Mat3 &a0 = A_points[i][static_cast<std::size_t>(k)];
          const Mat3 &a1 = A_points[i][static_cast<std::size_t>(k + 1)];
          const Mat3 b0 = b_matrix(cell_of(a0)), b1 = b_matrix(cell_of(a1));
          const Mat3 u0 = a0 * b0.inverse(), u1 = a1 * b1.inverse();
          const Mat3 u_centre = half_rotation(u1 * u0.transpose()) * u0;
          Mat3 b_centre;
          for (std::size_t m = 0; m < 9; ++m)
            b_centre.m[m] = 0.5 * (b0.m[m] + b1.m[m]);
          ub = SF[i] * u_centre * b_centre;
          cell = cell_from_b(b_centre);
        } else {
          ub = SF[i] * A[i];
          cell = cell_of(A[i]);
        }
        const Mat3 U = mosflm_u(ub, cell);
        batch.number = static_cast<int>(e.scan.first_image + k + offsets[i]);
        batch.title = "Batch " + std::to_string(batch.number);
        batch.set_cell(gemmi::UnitCell(cell.a, cell.b, cell.c, cell.alpha,
                                       cell.beta, cell.gamma));
        // Column major, as MTZ keeps it.
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c)
            batch.floats[6 + c * 3 + r] = static_cast<float>(U.m[r * 3 + c]);
        const double start =
            e.scan.osc_start + static_cast<double>(k) * e.scan.osc_width;
        batch.floats[36] = static_cast<float>(start);
        batch.floats[37] = static_cast<float>(start + e.scan.osc_width);
        batch.floats[47] = static_cast<float>(e.scan.osc_width);
        mtz.batches.push_back(batch);
      }
    }

    mtz.set_cell_for_all(gemmi::UnitCell(best.a, best.b, best.c, best.alpha,
                                         best.beta, best.gamma));
    if (!mtz.switch_to_asu_hkl())
      throw std::runtime_error(
          "the indices could not be put in the asymmetric unit");
    mtz.sort(5);
    mtz.write_to_file(output);
    std::printf("Wrote %zu reflections and %zu batches to %s", rows.size(),
                mtz.batches.size(), output.c_str());
    std::printf(" (%s; %zu left out)\n",
                scaled ? "scaled intensities" : "profile-fitted and summed",
                dropped);
    return 0;
  } catch (const std::exception &ex) {
    std::fprintf(stderr, "mxi_export: %s\n", ex.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_export.log");
  return mxi::run_program(argc, argv);
}

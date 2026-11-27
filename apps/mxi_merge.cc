// mxi_merge: a merged MTZ from scaled data, as dials.merge writes one (DIALS'
// dials/command_line/merge.py and dials/algorithms/merging/, read 26
// November): IMEAN and its multiplicity; with Friedel mates apart, I(+) and
// I(-) and theirs; French and Wilson's amplitudes, F, F(+), F(-), and DANO;
// FreeR_flag. Graeme, 26 November: wanted for the solvent analysis of the
// A71EV2AZ crystals, one merged file a crystal, and generally.

#include <cstdint>
#include <gemmi/mtz.hpp>
#include <gemmi/symmetry.hpp>
#include <gemmi/unitcell.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "args.hh"
#include "expt.hh"
#include "log_mirror.hh"
#include "merge.hh"
#include "output.hh"
#include "refl.hh"
#include "rflx.hh"
#include "scale.hh"
#include "symmetry.hh"
#include "views.hh"

namespace mxi {

namespace {

// dials' bad_for_scaling: outlier in scaling, excluded, excluded by the user.
constexpr std::int64_t kBadForScaling = (1 << 23) | (1 << 24) | (1 << 25);

void usage() {
  mxi::print(
      "usage: mxi merge [options] FILE.rflx | EXPT REFL\n"
      "  -j, --threads N   threads for everything this does in parallel; 0, "
      "every\n"
      "                    core this job may use (0)\n"
      "  A merged MTZ from scaled data, as dials.merge writes one: IMEAN,\n"
      "  SIGIMEAN and N; with Friedel mates apart (the default) I(+), I(-),\n"
      "  N(+), N(-); French and Wilson's F, F(+), F(-) and DANO; and\n"
      "  FreeR_flag. Every experiment merged together, or each alone.\n"
      "  -o, --output PATH          the MTZ (merged.mtz); with --each, the\n"
      "                             stem of each crystal's (merged_N.mtz)\n"
      "  --each                     each crystal merged alone, one MTZ apiece\n"
      "  --d-min D, --d-max D       the resolution range\n"
      "  --partiality-threshold P   leave out partiality below P (0.4)\n"
      "  --no-anomalous             Friedel mates merged only: no I(+), I(-),\n"
      "                             F(+), F(-), DANO\n"
      "  --no-truncate              no amplitudes (French and Wilson)\n"
      "  --free-fraction X          the FreeR test set's share, CCP4's flags\n"
      "                             0 to 1/X - 1, 0 the test set (0.05); the\n"
      "                             flag a hash of the index under the\n"
      "                             lattice's symmetry, so the same in every\n"
      "                             data set of one lattice; 0 for none\n"
      "  --shells N                 the merging statistics' resolution shells\n"
      "                             (20), as mxi scale's\n"
      "  --crystal-name NAME        (XTAL)\n"
      "  --project-name NAME        (mxi)\n"
      "  --dataset-name NAME        (NATIVE)\n");
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

struct Selected {
  std::vector<Miller> hkl;
  std::vector<double> intensity, variance;
  std::size_t left_out = 0;
};

// As dials' filter_reflection_table with the scaled intensities: not an
// outlier or excluded, a positive inverse scale factor and variance,
// partiality at least the threshold, within the resolution range; the
// scaled intensity intensity.scale.value over inverse_scale_factor.
Selected select(const Table &in, const gemmi::UnitCell &cell,
                std::int64_t only_id, double partiality_threshold, double d_min,
                double d_max) {
  Selected s;
  const Column &flags = in.at("flags");
  const Column &hkl = in.at("miller_index");
  const Column &value = in.at("intensity.scale.value");
  const Column &variance = in.at("intensity.scale.variance");
  const Column &factor = in.at("inverse_scale_factor");
  const bool has_id = in.has("id"), has_p = in.has("partiality");
  for (std::size_t i = 0; i < in.nrows; ++i) {
    if (only_id >= 0 && (!has_id || in.at("id").integer(i) != only_id))
      continue;
    const Miller h{static_cast<std::int32_t>(hkl.ints[i * 3]),
                   static_cast<std::int32_t>(hkl.ints[i * 3 + 1]),
                   static_cast<std::int32_t>(hkl.ints[i * 3 + 2])};
    const double g = factor.real(i), v = variance.real(i);
    const double p = has_p ? in.at("partiality").real(i) : 1.0;
    bool keep = (flags.integer(i) & kBadForScaling) == 0 && g > 0.0 &&
                v > 0.0 && p > 0.0 && p >= partiality_threshold &&
                !(h[0] == 0 && h[1] == 0 && h[2] == 0);
    if (keep) {
      const double d = cell.calculate_d({h[0], h[1], h[2]});
      keep = !(d_min > 0.0 && d < d_min) && !(d_max > 0.0 && d > d_max);
    }
    if (!keep) {
      ++s.left_out;
      continue;
    }
    s.hkl.push_back(h);
    s.intensity.push_back(value.real(i) / g);
    s.variance.push_back(v / (g * g));
  }
  return s;
}

// The summary of the merging statistics of what was merged, as mxi scale
// prints it (Graeme, 26 November: this table, not the long one), and
// from the same observations: build_scale_data's, with each one's intensity,
// variance and inverse scale factor as scaling wrote them, and those scaling
// left out (an outlier, excluded, no scaled variance) left out. So a merge
// is seen to reproduce scaling's figures (Graeme, 26 November).
void print_statistics(const ExperimentList &experiments, const Table &in,
                      const std::string &hall, std::int64_t only_id,
                      std::int32_t shells, const gemmi::UnitCell &cell,
                      double d_min, double d_max) {
  const SpaceGroup group = SpaceGroup::from_hall(hall);
  // The rows scaling had: not those it excluded -- beyond its resolution
  // limit, say, which never entered its data -- but its outliers, which did,
  // marked so. Its random halves are dealt over exactly those, so CC half
  // comes out the same to the digit (9uoa's outer shell: 0.243 against
  // scaling's 0.286, the rows beyond 1.76 A in the deal).
  constexpr std::int64_t kOutlier = std::int64_t{1} << 23;
  std::vector<std::size_t> keep;
  {
    const Column &flags = in.at("flags");
    const Column &variance = in.at("intensity.scale.variance");
    const Column &factor = in.at("inverse_scale_factor");
    const bool has_id = in.has("id");
    for (std::size_t r = 0; r < in.nrows; ++r) {
      if (only_id >= 0 && (!has_id || in.at("id").integer(r) != only_id))
        continue;
      // Within --d-min and --d-max, by the cell the merge uses: the
      // statistics of what was merged (Graeme, 27 November: with --d-min 1.3
      // the summary went to 1.14 A, every row scaling had).
      if (d_min > 0.0 || d_max > 0.0) {
        const Column &hkl = in.at("miller_index");
        const double d =
            cell.calculate_d({static_cast<int>(hkl.ints[r * 3]),
                              static_cast<int>(hkl.ints[r * 3 + 1]),
                              static_cast<int>(hkl.ints[r * 3 + 2])});
        if ((d_min > 0.0 && d < d_min) || (d_max > 0.0 && d > d_max))
          continue;
      }
      const std::int64_t f = flags.integer(r);
      const bool outlier = (f & kOutlier) != 0;
      const bool excluded = (f & (kBadForScaling & ~kOutlier)) != 0 ||
                            !(factor.real(r) > 0.0) ||
                            !(variance.real(r) > 0.0);
      if (outlier || !excluded)
        keep.push_back(r);
    }
  }
  const Table rows = select_rows(in, keep);
  ScaleData data =
      build_scale_data(experiments, rows, group, ScaleModelShape{1, 0, 0});
  const Column &flags = rows.at("flags");
  const Column &value = rows.at("intensity.scale.value");
  const Column &variance = rows.at("intensity.scale.variance");
  const Column &factor = rows.at("inverse_scale_factor");
  std::vector<double> g(data.size(), 1.0);
  for (std::size_t i = 0; i < data.size(); ++i) {
    const std::size_t r = data.row[i];
    const double gr = factor.real(r), v = variance.real(r);
    if ((flags.integer(r) & kBadForScaling) != 0 || !(gr > 0.0) || !(v > 0.0)) {
      data.outlier[i] = true;
      continue;
    }
    data.intensity[i] = value.real(r);
    data.variance[i] = v;
    g[i] = gr;
  }
  const std::size_t crystal =
      only_id >= 0 ? static_cast<std::size_t>(only_id) : 0;
  MergingShell all;
  const std::vector<MergingShell> table = merging_statistics(
      data, g, group, *experiments[crystal].crystal, shells, &all);
  print_merging_summary(table, all, nullptr, group.centrosymmetric());
}

struct Written {
  std::size_t unique = 0, observations = 0, left_out = 0, pairs = 0,
              refused = 0, free = 0;
  double d_low = 0.0, d_high = 0.0;
};

Written write_merged(const Selected &s, const gemmi::SpaceGroup *group,
                     const gemmi::UnitCell &cell, const UnitCell &mxi_cell,
                     double wavelength, bool anomalous, bool truncate,
                     double free_fraction, const Arguments &args,
                     const std::string &output) {
  Written w;
  w.observations = s.hkl.size();
  w.left_out = s.left_out;
  if (s.hkl.empty())
    throw std::runtime_error("no observations left to merge for " + output);
  const MergedEquivalents mean =
      merge_equivalents(s.hkl, s.intensity, s.variance, *group, false);
  MergedEquivalents anom;
  if (anomalous)
    anom = merge_equivalents(s.hkl, s.intensity, s.variance, *group, true);
  const auto d_star_cubed = [&](const MergedEquivalents &m) {
    std::vector<double> out(m.size());
    for (std::size_t i = 0; i < m.size(); ++i) {
      const double d =
          cell.calculate_d({m.hkl[i][0], m.hkl[i][1], m.hkl[i][2]});
      out[i] = 1.0 / (d * d * d);
    }
    return out;
  };

  // One row an index; the columns as dials.merge orders them.
  std::map<Miller, std::size_t> row;
  for (const Miller &h : mean.hkl)
    row.emplace(h, row.size());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::size_t nrow = row.size();
  std::vector<std::string> labels;
  std::vector<char> types;
  std::vector<std::vector<float>> cols;
  const auto add = [&](const char *label, char type) {
    labels.emplace_back(label);
    types.push_back(type);
    cols.emplace_back(nrow, nan);
    return cols.size() - 1;
  };
  const std::int32_t groups =
      free_fraction > 0.0
          ? static_cast<std::int32_t>(std::lround(1.0 / free_fraction))
          : 0;
  if (groups > 0) {
    const std::size_t c = add("FreeR_flag", 'I');
    const std::vector<Rotation> lattice = lattice_symmetry(mxi_cell, 5.0);
    for (const auto &[h, r] : row) {
      const std::int32_t f = free_flag(h, lattice, groups);
      cols[c][r] = static_cast<float>(f);
      w.free += f == 0;
    }
  }
  {
    const std::size_t c = add("IMEAN", 'J'), cs = add("SIGIMEAN", 'Q'),
                      cn = add("N", 'I');
    for (std::size_t i = 0; i < mean.size(); ++i) {
      const std::size_t r = row.at(mean.hkl[i]);
      cols[c][r] = static_cast<float>(mean.value[i]);
      cols[cs][r] = static_cast<float>(mean.sigma[i]);
      cols[cn][r] = static_cast<float>(mean.n[i]);
    }
  }
  // Amplitudes. With Friedel mates apart, dials.merge's truncate takes French
  // and Wilson to I(+) and I(-), F the mean of F(+) and F(-) by their
  // sigmas, DANO F(+) - F(-); otherwise French and Wilson to IMEAN.
  FrenchWilson fw_anom, fw_mean;
  if (truncate && anomalous)
    fw_anom =
        french_wilson(anom.value, anom.sigma, anom.centric, d_star_cubed(anom));
  else if (truncate)
    fw_mean =
        french_wilson(mean.value, mean.sigma, mean.centric, d_star_cubed(mean));
  if (truncate) {
    const std::size_t c = add("F", 'F'), cs = add("SIGF", 'Q');
    if (anomalous) {
      std::vector<double> sw(nrow, 0.0), swf(nrow, 0.0);
      for (std::size_t i = 0; i < anom.size(); ++i) {
        if (!fw_anom.valid[i]) {
          ++w.refused;
          continue;
        }
        const std::size_t r = row.at(anom.hkl[i]);
        const double wt = 1.0 / (fw_anom.sigF[i] * fw_anom.sigF[i]);
        sw[r] += wt;
        swf[r] += wt * fw_anom.F[i];
      }
      for (std::size_t r = 0; r < nrow; ++r)
        if (sw[r] > 0.0) {
          cols[c][r] = static_cast<float>(swf[r] / sw[r]);
          cols[cs][r] = static_cast<float>(1.0 / std::sqrt(sw[r]));
        }
    } else {
      for (std::size_t i = 0; i < mean.size(); ++i) {
        if (!fw_mean.valid[i]) {
          ++w.refused;
          continue;
        }
        const std::size_t r = row.at(mean.hkl[i]);
        cols[c][r] = static_cast<float>(fw_mean.F[i]);
        cols[cs][r] = static_cast<float>(fw_mean.sigF[i]);
      }
    }
  }
  if (anomalous) {
    const std::size_t ip = add("I(+)", 'K'), sp = add("SIGI(+)", 'M'),
                      im = add("I(-)", 'K'), sm = add("SIGI(-)", 'M'),
                      np = add("N(+)", 'I'), nm = add("N(-)", 'I');
    std::size_t fp = 0, sfp = 0, fm = 0, sfm = 0, dano = 0, sdano = 0;
    if (truncate) {
      fp = add("F(+)", 'G');
      sfp = add("SIGF(+)", 'L');
      fm = add("F(-)", 'G');
      sfm = add("SIGF(-)", 'L');
      dano = add("DANO", 'D');
      sdano = add("SIGDANO", 'Q');
    }
    std::vector<double> fplus(nrow, std::nan("")), fminus(nrow, std::nan("")),
        splus(nrow, 0.0), sminus(nrow, 0.0);
    for (std::size_t i = 0; i < anom.size(); ++i) {
      const std::size_t r = row.at(anom.hkl[i]);
      // A centric reflection has one hemisphere: I(+), as dials.merge's
      // match_bijvoet_mates puts singles there.
      const bool minus = anom.hemisphere[i] < 0;
      cols[minus ? im : ip][r] = static_cast<float>(anom.value[i]);
      cols[minus ? sm : sp][r] = static_cast<float>(anom.sigma[i]);
      cols[minus ? nm : np][r] = static_cast<float>(anom.n[i]);
      if (truncate && fw_anom.valid[i]) {
        cols[minus ? fm : fp][r] = static_cast<float>(fw_anom.F[i]);
        cols[minus ? sfm : sfp][r] = static_cast<float>(fw_anom.sigF[i]);
        (minus ? fminus : fplus)[r] = fw_anom.F[i];
        (minus ? sminus : splus)[r] = fw_anom.sigF[i];
      }
    }
    if (truncate)
      for (std::size_t r = 0; r < nrow; ++r)
        if (std::isfinite(fplus[r]) && std::isfinite(fminus[r])) {
          cols[dano][r] = static_cast<float>(fplus[r] - fminus[r]);
          cols[sdano][r] = static_cast<float>(
              std::sqrt(splus[r] * splus[r] + sminus[r] * sminus[r]));
          ++w.pairs;
        }
  }

  gemmi::Mtz mtz(true);
  mtz.title = "From mxi merge";
  {
    char when[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d at %H:%M:%S UTC",
                  std::gmtime(&now));
    mtz.history.push_back(std::string("From mxi merge, run on ") + when);
  }
  mtz.spacegroup = group;
  gemmi::Mtz::Dataset &ds =
      mtz.add_dataset(args.value("--dataset-name", "NATIVE"));
  ds.crystal_name = args.value("--crystal-name", "XTAL");
  ds.project_name = args.value("--project-name", "mxi");
  ds.wavelength = wavelength;
  const std::int32_t id = static_cast<std::int32_t>(mtz.datasets.back().id);
  for (std::size_t k = 0; k < labels.size(); ++k)
    mtz.add_column(labels[k], types[k], id, -1, false);
  std::vector<float> data;
  data.reserve(nrow * mtz.columns.size());
  std::vector<Miller> order(nrow);
  for (const auto &[h, r] : row)
    order[r] = h;
  for (std::size_t r = 0; r < nrow; ++r) {
    data.insert(data.end(), {static_cast<float>(order[r][0]),
                             static_cast<float>(order[r][1]),
                             static_cast<float>(order[r][2])});
    for (const auto &c : cols)
      data.push_back(c[r]);
  }
  mtz.set_data(data.data(), data.size());
  mtz.set_cell_for_all(cell);
  mtz.sort(3);
  mtz.write_to_file(output);
  mtz.update_reso();
  w.unique = nrow;
  w.d_low = mtz.resolution_low();
  w.d_high = mtz.resolution_high();
  return w;
}

int run_merge(int argc, char **argv) {
  const std::set<std::string> known = {"-o",
                                       "--output",
                                       "--each",
                                       "--d-min",
                                       "--d-max",
                                       "--partiality-threshold",
                                       "--no-anomalous",
                                       "--no-truncate",
                                       "--free-fraction",
                                       "--crystal-name",
                                       "--project-name",
                                       "--dataset-name",
                                       "--shells"};
  std::set<std::string> takes_value = known;
  for (const char *flag : {"--each", "--no-anomalous", "--no-truncate"})
    takes_value.erase(flag);
  Arguments args = parse_arguments(argc, argv, known, takes_value);
  args.positional = rflx::as_pair(args.positional);
  if (args.help) {
    usage();
    return 0;
  }
  if (!args.ok || args.positional.size() != 2) {
    if (!args.ok)
      mxi::eprint("mxi merge: %s\n", args.error.c_str());
    usage();
    return 2;
  }
  try {
    const ExperimentList whole = read_experiments(args.positional[0]);
    const Lattices views = lattices_of(whole);
    const ExperimentList &experiments = views.list;
    const Table in = lattice_rows(read_reflections(args.positional[1]), views);
    if (experiments.size() == 0)
      throw std::runtime_error("no experiments with a crystal");
    if (!(in.has("intensity.scale.value") &&
          in.has("intensity.scale.variance") && in.has("inverse_scale_factor")))
      throw std::runtime_error(
          "not scaled: mxi merge wants mxi scale's output");
    std::string hall;
    for (std::size_t i = 0; i < experiments.size(); ++i) {
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
    std::vector<double> waves;
    for (const Experiment &e : experiments)
      waves.push_back(e.beam.wavelength);
    std::sort(waves.begin(), waves.end());
    if (waves.back() - waves.front() > 1e-4)
      throw std::runtime_error(
          "more than one wavelength: mxi merge writes one dataset");

    const bool each = args.has("--each");
    const bool anomalous = !args.has("--no-anomalous");
    const bool truncate = !args.has("--no-truncate");
    const double free_fraction = args.number("--free-fraction", 0.05);
    const double partiality = args.number("--partiality-threshold", 0.4);
    const double d_min = args.number("--d-min", 0.0);
    const double d_max = args.number("--d-max", 0.0);
    const std::string output = args.value("-o", args.value("--output", ""));
    const std::int32_t shells =
        static_cast<std::int32_t>(args.number("--shells", 20.0));

    // The cell: the median of each parameter over the crystals merged, as
    // dials.merge's determine_best_unit_cell.
    const auto best_cell = [&](const std::vector<std::size_t> &which) {
      std::vector<double> p[6];
      for (std::size_t i : which) {
        const UnitCell c = experiments[i].crystal->cell();
        const double v[6] = {c.a, c.b, c.c, c.alpha, c.beta, c.gamma};
        for (std::int32_t k = 0; k < 6; ++k)
          p[k].push_back(v[k]);
      }
      return UnitCell{median(p[0]), median(p[1]), median(p[2]),
                      median(p[3]), median(p[4]), median(p[5])};
    };
    const auto one = [&](const std::vector<std::size_t> &which,
                         std::int64_t only_id, const std::string &path) {
      const UnitCell c = best_cell(which);
      const gemmi::UnitCell gc(c.a, c.b, c.c, c.alpha, c.beta, c.gamma);
      const Selected s = select(in, gc, only_id, partiality, d_min, d_max);
      const Written w =
          write_merged(s, group, gc, c, experiments[which[0]].beam.wavelength,
                       anomalous, truncate, free_fraction, args, path);
      mxi::print("%s: %zu observations merged to %zu reflections, %.2f to "
                 "%.2f A (%zu left out)",
                 path.c_str(), w.observations, w.unique, w.d_low, w.d_high,
                 w.left_out);
      if (anomalous && truncate)
        mxi::print("; %zu Friedel pairs with DANO", w.pairs);
      if (truncate && w.refused)
        mxi::print("; %zu refused by French and Wilson", w.refused);
      if (free_fraction > 0.0)
        mxi::print("; %zu in the free set", w.free);
      mxi::print(
          "\n  space group %s; cell %.3f %.3f %.3f A, %.3f %.3f %.3f deg\n",
          group->hm, c.a, c.b, c.c, c.alpha, c.beta, c.gamma);
      print_statistics(experiments, in, hall, only_id, shells, gc, d_min,
                       d_max);
    };
    if (each) {
      const std::string stem =
          output.empty()
              ? "merged"
              : (output.size() > 4 && output.substr(output.size() - 4) == ".mtz"
                     ? output.substr(0, output.size() - 4)
                     : output);
      for (std::size_t i = 0; i < experiments.size(); ++i)
        one({i}, static_cast<std::int64_t>(i),
            stem + "_" + std::to_string(i) + ".mtz");
    } else {
      std::vector<std::size_t> all(experiments.size());
      for (std::size_t i = 0; i < all.size(); ++i)
        all[i] = i;
      one(all, -1, output.empty() ? "merged.mtz" : output);
    }
    mxi::print(
        "  columns: H K L%s IMEAN SIGIMEAN N%s%s%s\n",
        free_fraction > 0.0 ? " FreeR_flag" : "", truncate ? " F SIGF" : "",
        anomalous ? " I(+) SIGI(+) I(-) SIGI(-) N(+) N(-)" : "",
        anomalous && truncate ? " F(+) SIGF(+) F(-) SIGF(-) DANO SIGDANO" : "");
    return 0;
  } catch (const std::exception &ex) {
    mxi::eprint("mxi merge: %s\n", ex.what());
    return 1;
  }
}

} // namespace

} // namespace mxi

// The program's entry, run by mxi (merge) -- apps/mxi.cc.
int mxi_merge_main(int argc, char **argv) {
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_merge.log");
  return mxi::run_merge(argc, argv);
}

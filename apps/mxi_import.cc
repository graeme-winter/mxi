// mxi_import: an experiment list from an NXmx HDF5 master file, as
// dials.import writes it, so that the chain needs nothing of DIALS's. What the
// file gets wrong or leaves out, the overrides say.

#include <cmath>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "args.hh"
#include "json.hh"
#include "log_mirror.hh"
#include "nxmx_import.hh"
#include "rflx.hh"

namespace {

const char *kUsage =
    "usage: mxi_import MASTER [MASTER ...] [options]\n"
    "  An experiment list -- beam, detector, goniometer, scan, image set -- "
    "from\n"
    "  NXmx HDF5 master files, as dials.import writes it: one experiment a\n"
    "  master, each sweep with models of its own. The options apply to every\n"
    "  master alike.\n"
    "  -o, --output PATH     where to write it: a .rflx (imported.rflx), or "
    "an\n"
    "                        .expt, JSON, as dials.import writes it\n"
    "  --wavelength A        the wavelength, for a file without or with a "
    "wrong one\n"
    "  --distance MM         the detector distance, along the panel's normal\n"
    "  --beam-centre X,Y     where the beam meets the detector, in pixels, "
    "fast and\n"
    "                        slow: the detector moved in its plane to put it "
    "there\n"
    "  --mu M                the sensor's attenuation coefficient, 1/mm, for "
    "a\n"
    "                        material not tabulated\n"
    "  --trusted-max N       the top of the trusted range, for a file with no\n"
    "                        saturation value\n"
    "  --image-range A,B     only images A to B, counting from 1\n";

std::vector<double> pair_of(const std::string &text, const char *what) {
  const std::size_t comma = text.find(',');
  if (comma == std::string::npos)
    throw std::runtime_error(std::string(what) +
                             " wants two numbers, as X,Y: not '" + text + "'");
  return {std::stod(text.substr(0, comma)), std::stod(text.substr(comma + 1))};
}

} // namespace

//: What was read from one master, for a person to check against what they know.
void describe(const std::string &master, const mxi::json::Value &expt,
              const std::vector<std::string> &notes) {
  using namespace mxi;
  const auto &e = expt.as_object();
  const auto num = [](const json::Value &v) { return v.as_number(); };
  std::printf("mxi_import: %s\n", master.c_str());
  std::printf("  wavelength %.6f A\n",
              num(e.at("beam").as_array()[0].as_object().at("wavelength")));
  for (const json::Value &pv :
       e.at("detector").as_array()[0].as_object().at("panels").as_array()) {
    const auto &p = pv.as_object();
    const auto a = [&](const char *k, int i) {
      return num(p.at(k).as_array()[i]);
    };
    const double fx = a("fast_axis", 0), fy = a("fast_axis", 1),
                 fz = a("fast_axis", 2);
    const double sx = a("slow_axis", 0), sy = a("slow_axis", 1),
                 sz = a("slow_axis", 2);
    const double ox = a("origin", 0), oy = a("origin", 1), oz = a("origin", 2);
    double nx = fy * sz - fz * sy, ny = fz * sx - fx * sz,
           nz = fx * sy - fy * sx;
    const double nn = std::sqrt(nx * nx + ny * ny + nz * nz);
    nx /= nn, ny /= nn, nz /= nn;
    const double d = ox * nx + oy * ny + oz * nz;
    // Where the beam, along -z, meets the plane: t (0,0,-1).n = d.
    std::printf("  panel %s: %lld x %lld pixels of %.4f x %.4f mm, %s %.3f "
                "mm thick, mu %.4f/mm\n",
                p.at("name").as_string().c_str(),
                static_cast<long long>(a("image_size", 0)),
                static_cast<long long>(a("image_size", 1)), a("pixel_size", 0),
                a("pixel_size", 1), p.at("material").as_string().c_str(),
                num(p.at("thickness")), num(p.at("mu")));
    if (std::fabs(nz) > 1e-9) {
      const double t = d / (-nz);
      const double hx = -ox, hy = -oy, hz = -t - oz;
      const double bx = (hx * fx + hy * fy + hz * fz) / a("pixel_size", 0);
      const double by = (hx * sx + hy * sy + hz * sz) / a("pixel_size", 1);
      std::printf("    distance %.3f mm; the beam meets it at pixel %.2f, "
                  "%.2f; trusted to %.0f\n",
                  std::fabs(d), bx, by, a("trusted_range", 1));
    }
  }
  const auto &g = e.at("goniometer").as_array()[0].as_object();
  std::printf("  goniometer:");
  const auto &names = g.at("names").as_array();
  for (std::size_t i = 0; i < names.size(); ++i)
    std::printf(" %s%s", names[i].as_string().c_str(),
                static_cast<int>(i) == static_cast<int>(num(g.at("scan_axis")))
                    ? " (scan)"
                    : "");
  std::printf("\n");
  const auto &s = e.at("scan").as_array()[0].as_object();
  const auto &osc = s.at("properties").as_object().at("oscillation").as_array();
  const auto &range = s.at("image_range").as_array();
  const double width = osc.size() > 1 ? num(osc[1]) - num(osc[0]) : 0.0;
  std::printf("  scan: images %lld to %lld, from %.4f deg, %.4f deg each\n",
              static_cast<long long>(num(range[0])),
              static_cast<long long>(num(range[1])),
              osc.empty() ? 0.0 : num(osc[0]), width);
  for (const std::string &n : notes)
    std::printf("  note: %s\n", n.c_str());
}

int main(int argc, char **argv) {
  using namespace mxi;
  // Mirrored to mxi_import.log in the working directory, as every mxi program
  // and DIALS's own do; not for a run that only asks for help.
  if (!only_asks_for_help(argc, argv))
    mirror_to_log("mxi_import.log");
  const std::set<std::string> known = {
      "-o",   "--output",      "--wavelength",  "--distance", "--beam-centre",
      "--mu", "--trusted-max", "--image-range", "--help"};
  const std::set<std::string> takes_value = {
      "-o",   "--output",      "--wavelength", "--distance", "--beam-centre",
      "--mu", "--trusted-max", "--image-range"};
  const Arguments args = parse_arguments(argc, argv, known, takes_value);
  if (args.has("--help")) {
    std::printf("%s", kUsage);
    return 0;
  }
  if (!args.ok || args.positional.empty()) {
    if (!args.ok)
      std::fprintf(stderr, "mxi_import: %s\n", args.error.c_str());
    std::fprintf(stderr, "%s", kUsage);
    return 2;
  }
  try {
    ImportOverrides o;
    if (args.has("--wavelength"))
      o.wavelength = args.number("--wavelength", 0.0);
    if (args.has("--distance"))
      o.distance = args.number("--distance", 0.0);
    if (args.has("--mu"))
      o.mu = args.number("--mu", 0.0);
    if (args.has("--trusted-max"))
      o.trusted_max = args.number("--trusted-max", 0.0);
    if (args.has("--beam-centre")) {
      const auto v = pair_of(args.value("--beam-centre", ""), "--beam-centre");
      o.beam_centre = std::array<double, 2>{v[0], v[1]};
    }
    if (args.has("--image-range")) {
      const auto v = pair_of(args.value("--image-range", ""), "--image-range");
      o.image_range =
          std::array<int, 2>{static_cast<int>(v[0]), static_cast<int>(v[1])};
    }
    const rflx::Outputs out = rflx::outputs(args, "imported");
    // One experiment a master, each with models of its own, joined as
    // dials.import writes several sweeps; each described as it is read.
    std::vector<json::Value> lists;
    for (const std::string &master : args.positional) {
      std::vector<std::string> notes;
      lists.push_back(import_nxmx(master, o, &notes));
      describe(master, lists.back(), notes);
    }
    const json::Value document = join_experiment_lists(lists);
    rflx::write_outputs(out, &document, nullptr, "mxi_import");
    if (lists.size() > 1)
      std::printf("%zu sweeps, one experiment each\n", lists.size());
    std::printf("Wrote %s\n", rflx::describe(out, true, false).c_str());
    return 0;
  } catch (const std::exception &ex) {
    std::fprintf(stderr, "mxi_import: %s\n", ex.what());
    return 1;
  }
}

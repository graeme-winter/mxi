// mxi_convert: between .rflx (docs/rflx.md) and the DIALS pair, .expt and
// .refl, its direction from what it is given.
//
//   mxi_convert indexed.expt indexed.refl     -> indexed.rflx
//   mxi_convert indexed.rflx                  -> indexed.expt, indexed.refl
//   mxi_convert indexed.rflx -o other.rflx    -> a .rflx rewritten
//
// The experiment list goes through as the JSON tree it is -- never made into
// mxi's models -- so what mxi does not model, profiles and scaling models and
// the rest, goes through untouched.

#include <cstdio>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "args.hh"
#include "json.hh"
#include "log_mirror.hh"
#include "refl.hh"
#include "rflx.hh"

namespace {

const char *kUsage =
    "usage: mxi_convert [options] FILE...\n"
    "  Between .rflx -- mxi's own files, one HDF5 file holding the experiment\n"
    "  list, the reflections or both (docs/rflx.md) -- and the DIALS pair.\n"
    "  Given a .rflx, writes its .expt and .refl, whichever it holds; given "
    "an\n"
    "  .expt, a .refl or both, writes a .rflx. A .rflx is told by what it is,\n"
    "  HDF5, not by its name.\n"
    "  -o, --output PATH      the .rflx to write (from the pair: the first\n"
    "                         input's name as .rflx); from a .rflx, a .rflx\n"
    "                         rewritten\n"
    "  --output-expt PATH     from a .rflx, the .expt (its name as .expt)\n"
    "  --output-refl PATH     from a .rflx, the .refl (its name as .refl)\n";

std::string with_extension(const std::string &path, const std::string &ext) {
  return std::filesystem::path(path).replace_extension(ext).string();
}

bool ends_with(const std::string &s, const std::string &tail) {
  return s.size() >= tail.size() &&
         s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

int main(int argc, char **argv) {
  using namespace mxi;
  if (!only_asks_for_help(argc, argv))
    mirror_to_log("mxi_convert.log");
  const std::set<std::string> known = {"-o", "--output", "--output-expt",
                                       "--output-refl", "--help"};
  const std::set<std::string> takes_value = {"-o", "--output", "--output-expt",
                                             "--output-refl"};
  const Arguments args = parse_arguments(argc, argv, known, takes_value);
  if (args.has("--help")) {
    std::printf("%s", kUsage);
    return 0;
  }
  if (!args.ok || args.positional.empty()) {
    if (!args.ok)
      std::fprintf(stderr, "mxi_convert: %s\n", args.error.c_str());
    std::fprintf(stderr, "%s", kUsage);
    return 2;
  }
  const std::string output = args.value("-o", args.value("--output", ""));
  try {
    std::vector<std::string> hdf5, other;
    for (const std::string &p : args.positional)
      (rflx::is_hdf5(p) ? hdf5 : other).push_back(p);

    if (!hdf5.empty()) {
      // From a .rflx.
      if (hdf5.size() > 1 || !other.empty()) {
        std::fprintf(stderr,
                     "mxi_convert: one .rflx at a time, and nothing with it\n");
        return 2;
      }
      const std::string &in = hdf5.front();
      const std::optional<json::Value> experiments = rflx::read_experiments(in);
      std::vector<std::string> skipped;
      const std::optional<Table> reflections =
          rflx::read_reflections(in, &skipped);
      for (const std::string &name : skipped)
        std::printf("Left out the column %s, of a type mxi's tables have no "
                    "place for\n",
                    name.c_str());
      if (!experiments && !reflections) {
        std::fprintf(stderr,
                     "mxi_convert: %s holds neither experiments nor "
                     "reflections\n",
                     in.c_str());
        return 1;
      }
      if (!output.empty() && ends_with(output, ".rflx")) {
        rflx::write(output, experiments ? &*experiments : nullptr,
                    reflections ? &*reflections : nullptr, "mxi_convert");
        std::printf("Wrote %s\n", output.c_str());
        return 0;
      }
      if (experiments) {
        const std::string out =
            args.value("--output-expt", with_extension(in, ".expt"));
        json::dump_file(out, *experiments);
        std::printf("Wrote the experiment list to %s\n", out.c_str());
      }
      if (reflections) {
        const std::string out =
            args.value("--output-refl", with_extension(in, ".refl"));
        write_reflections(out, *reflections);
        std::printf("Wrote %zu reflections to %s\n", reflections->nrows,
                    out.c_str());
      }
      return 0;
    }

    // From the DIALS pair: an .expt, a .refl or both, told by their contents
    // -- a .refl is msgpack, an .expt JSON -- with their names to break a tie.
    std::optional<json::Value> experiments;
    std::optional<Table> reflections;
    for (const std::string &p : other) {
      const bool looks_expt = ends_with(p, ".expt") || ends_with(p, ".json");
      const bool looks_refl = ends_with(p, ".refl");
      if (looks_expt || (!looks_refl && !experiments)) {
        if (experiments) {
          std::fprintf(stderr, "mxi_convert: two experiment lists given\n");
          return 2;
        }
        experiments = json::parse_file(p);
      } else {
        if (reflections) {
          std::fprintf(stderr, "mxi_convert: two reflection tables given\n");
          return 2;
        }
        reflections = read_reflections(p);
      }
    }
    const std::string out =
        output.empty() ? with_extension(other.front(), ".rflx") : output;
    rflx::write(out, experiments ? &*experiments : nullptr,
                reflections ? &*reflections : nullptr, "mxi_convert");
    std::printf("Wrote %s%s%s\n", out.c_str(),
                experiments ? ": the experiment list" : ":",
                reflections ? (", " + std::to_string(reflections->nrows) +
                               " reflections")
                                  .c_str()
                            : "");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "mxi_convert: %s\n", e.what());
    return 1;
  }
}

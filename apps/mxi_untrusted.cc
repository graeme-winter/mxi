// mxi_untrusted: each image set's untrusted regions, listed, set from a mask
// file, or cleared -- the shapes mxi_view draws (src/untrusted.hh), written
// into the experiment list where every later program finds them. mxi_view
// calls it to save; on its own it applies a mask kept from before to another
// run's sweeps, with no window.

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "args.hh"
#include "commands.hh"
#include "json.hh"
#include "log_mirror.hh"
#include "output.hh"
#include "processing.hh"
#include "refl.hh"
#include "rflx.hh"
#include "untrusted.hh"

namespace {

const char *const kUsage =
    "usage: mxi untrusted FILE.rflx [--set MASK.json | --clear | --hot]\n"
    "                     [-o OUT.rflx]\n"
    "  -j, --threads N   threads for everything this does in parallel; 0, "
    "every\n"
    "                    core this job may use (0)\n"
    "\n"
    "Each image set's untrusted regions -- circles, rectangles and polygons\n"
    "on the detector, and single pixels, that spot finding and integration\n"
    "leave out, as they do the detector's own bad pixels. mxi view draws "
    "them;\n"
    "this lists, sets, clears and adds hot pixels to them. With none of\n"
    "--set, --clear and --hot, lists them.\n"
    "\n"
    "  --set MASK.json  the regions from a mask file: {\"imagesets\": {\"0\":\n"
    "                   [shapes], \"1\": [shapes]}} for each image set named, "
    "or\n"
    "                   a list of shapes alone for every image set. A shape "
    "is\n"
    "                   {\"circle\": [x, y, r]}, {\"rectangle\": [x0, y0, x1,\n"
    "                   y1]} or {\"polygon\": [[x, y], ...]}, in pixels, panel "
    "0\n"
    "  --clear          every image set's regions removed\n"
    "  --hot            hot pixels added: those signal on --hot-fraction of\n"
    "                   the images or more (0.25), from 100 images, found as\n"
    "                   mxi find --mask-hot finds them, every image read\n"
    "  --hot-fraction F that share; --gpu to threshold on the GPU\n"
    "  --hot-list FILE  the hot pixels also as JSON, [[slow, fast], ...] --\n"
    "                   row then column, as a detector's own pixel mask is\n"
    "                   indexed -- or, for several image sets, "
    "{\"imagesets\":\n"
    "                   {\"0\": [...], ...}}\n"
    "  -o PATH          where to write (masked.rflx): the input is left as it\n"
    "                   is, to compare with; name it here to write over it\n"
    "  --help, --version\n";

std::string base_name(const std::string &path) {
  return std::filesystem::path(path).filename().string();
}

} // namespace

// The program's entry, run by mxi (untrusted) -- apps/mxi.cc.
int mxi_untrusted_main(int argc, char **argv) {
  using namespace mxi;
  if (!only_asks_for_help(argc, argv))
    mirror_to_log("mxi_untrusted.log");
  const Arguments args =
      parse_arguments(argc, argv,
                      {"-o", "--set", "--clear", "--hot", "--hot-fraction",
                       "--hot-list", "--gpu", "--help"},
                      {"-o", "--set", "--hot-fraction", "--hot-list"});
  if (args.help) {
    mxi::print("%s", kUsage);
    return 0;
  }
  const int changes = static_cast<int>(args.has("--set")) +
                      static_cast<int>(args.has("--clear")) +
                      static_cast<int>(args.has("--hot"));
  if (!args.ok || args.positional.size() != 1 || changes > 1) {
    mxi::eprint("mxi untrusted: %s\n%s",
                !args.ok ? args.error.c_str()
                : args.positional.size() != 1
                    ? "one experiment list"
                    : "one of --set, --clear and --hot",
                kUsage);
    return 2;
  }
  try {
    const std::string &in = args.positional[0];
    std::optional<json::Value> document = rflx::read_experiments(in);
    if (!document)
      throw std::runtime_error(in + ": a .rflx holding an experiment list");
    json::Value &imagesets = document->as_object()["imageset"];
    if (!imagesets.is_array() || imagesets.as_array().empty())
      throw std::runtime_error(in + " has no image sets");
    json::Array &sets = imagesets.as_array();

    const bool changing =
        args.has("--set") || args.has("--clear") || args.has("--hot");
    if (args.has("--set")) {
      const json::Value mask = json::parse_file(args.value("--set", ""));
      const auto apply = [&](std::size_t k, const json::Value &shapes) {
        if (k >= sets.size())
          throw std::runtime_error("the mask names image set " +
                                   std::to_string(k) + "; the file has " +
                                   std::to_string(sets.size()));
        (void)untrusted_pixels(shapes, 1, 1); // each shape's form, checked
        sets[k].as_object()["untrusted"] = shapes;
      };
      if (mask.is_array()) {
        for (std::size_t k = 0; k < sets.size(); ++k)
          apply(k, mask);
      } else if (mask["imagesets"].is_object()) {
        for (const auto &[key, shapes] : mask["imagesets"].as_object())
          apply(static_cast<std::size_t>(std::stoul(key)), shapes);
      } else {
        throw std::runtime_error(
            "a mask file is a list of shapes, or {\"imagesets\": {...}}");
      }
    } else if (args.has("--clear")) {
      for (json::Value &s : sets)
        if (s.is_object())
          s.as_object().erase("untrusted");
    } else if (args.has("--hot")) {
      // Spot finding run here, as mxi process runs a step, with --mask-hot
      // into a scratch file: the same thresholding and the same judgement as
      // mxi find's own, every image read (Graeme, 16 November). Its image
      // sets' regions, the input's and the hot pixels added, are taken.
      const Command find = command_named("find");
      if (find == nullptr)
        throw std::runtime_error("this mxi has no find");
      const std::string scratch =
          args.value("-o", "masked.rflx") + ".mxi_untrusted_hot.rflx";
      std::vector<std::string> words = {
          "mxi find", in, "-o", scratch, "--mask-hot", "--no-shoeboxes"};
      if (args.has("--hot-fraction"))
        pass_on(words, "--hot-fraction", args.value("--hot-fraction", ""),
                {"--hot-fraction"});
      if (args.has("--gpu"))
        words.push_back("--gpu");
      if (args.has("--threads"))
        pass_on(words, "--threads", args.value("--threads", ""), {});
      std::vector<char *> argv_find;
      for (std::string &w : words)
        argv_find.push_back(w.data());
      argv_find.push_back(nullptr);
      // Spot finding's own lines to its log, not the terminal: what was asked
      // for is the hot pixels (Graeme, 18 November). Its progress bar still
      // shows, written to the terminal directly.
      mxi::print("Reading every image for hot pixels:\n");
      std::int32_t code = 1;
      {
        const QuietTerminal quiet;
        const std::size_t logs = log_depth();
        code = find(static_cast<int>(words.size()), argv_find.data());
        flush();
        close_logs_to(logs);
      }
      if (code != 0)
        throw std::runtime_error(
            "spot finding, for the hot pixels, failed: mxi_find.log says why");
      std::optional<json::Value> found = rflx::read_experiments(scratch);
      const std::optional<json::Value> summary = rflx::read_summary(scratch);
      std::filesystem::remove(scratch);
      if (!found || !(*found)["imageset"].is_array() ||
          (*found)["imageset"].as_array().size() != sets.size())
        throw std::runtime_error("spot finding gave back other image sets");
      const json::Array &with = (*found)["imageset"].as_array();
      for (std::size_t k = 0; k < sets.size(); ++k)
        if (with[k].is_object() && with[k]["untrusted"].is_array())
          sets[k].as_object()["untrusted"] = with[k]["untrusted"];

      // Each sweep's, from spot finding's summary: every one, slow then fast
      // -- a detector's own mask, m[slow, fast] -- with its share.
      json::Array per_sweep;
      if (summary) {
        const std::vector<json::Value> steps = Summary::steps(*summary);
        if (!steps.empty() && steps.back()["hot_pixels"].is_array())
          per_sweep = steps.back()["hot_pixels"].as_array();
      }
      json::Object listed;
      for (std::size_t k = 0; k < per_sweep.size(); ++k) {
        const json::Value &one = per_sweep[k];
        const auto images =
            static_cast<std::int64_t>(one["images"].as_number());
        const double percent = 100.0 * one["fraction"].as_number();
        if (per_sweep.size() > 1)
          mxi::print("Image set %zu: ", k);
        if (!one["judged"].as_bool()) {
          mxi::print("Hot pixels: not judged, from %" PRId64
                     " images (100 at least)\n",
                     images);
          continue;
        }
        const json::Array &pixels = one["pixels"].as_array();
        if (pixels.empty()) {
          mxi::print("Hot pixels: none on >= %.0f%% of the %" PRId64
                     " images\n",
                     percent, images);
          continue;
        }
        mxi::print("Hot pixels: %zu on >= %.0f%% of the %" PRId64
                   " images (--hot-fraction)\n",
                   pixels.size(), percent, images);
        mxi::print("     slow    fast  images\n");
        json::Array pairs;
        for (const json::Value &p : pixels) {
          const json::Array &e = p.as_array();
          mxi::print("  %7.0f %7.0f  %5.1f%%\n", e[0].as_number(),
                     e[1].as_number(),
                     100.0 * e[2].as_number() / static_cast<double>(images));
          pairs.push_back(json::Value(json::Array{e[0], e[1]}));
        }
        listed[std::to_string(k)] = json::Value(std::move(pairs));
      }
      if (args.has("--hot-list")) {
        const std::string path = args.value("--hot-list", "");
        json::Value written;
        if (per_sweep.size() == 1)
          written =
              listed.count("0") ? listed["0"] : json::Value(json::Array{});
        else {
          json::Object wrapped;
          wrapped["imagesets"] = json::Value(std::move(listed));
          written = json::Value(std::move(wrapped));
        }
        std::FILE *out = std::fopen(path.c_str(), "w");
        if (out == nullptr)
          throw std::runtime_error("cannot write " + path);
        const std::string text = json::dump(written, 0);
        std::fputs(text.c_str(), out);
        std::fputs("\n", out);
        std::fclose(out);
        mxi::print("Wrote %s: the hot pixels, [slow, fast]\n", path.c_str());
      }
      mxi::print("\n");
    }

    for (std::size_t k = 0; k < sets.size(); ++k) {
      const json::Value &s = sets[k];
      mxi::print("image set %zu, %s: %s\n", k,
                 s["template"].is_string()
                     ? base_name(s["template"].as_string()).c_str()
                     : "?",
                 describe_untrusted(s["untrusted"]).c_str());
    }
    if (!changing)
      return 0;

    // A new file, the input left as it was, for comparison (Graeme, 22
    // October); the input itself only when named.
    const std::string out = args.value("-o", "masked.rflx");
    std::vector<std::string> skipped;
    const std::optional<Table> table = rflx::read_reflections(in, &skipped);
    Summary step("mxi_untrusted", in);
    step.set("imagesets", json::Value(static_cast<std::int64_t>(sets.size())));
    const json::Value steps = step.document();
    const std::string temporary = out + ".mxi_untrusted.tmp";
    rflx::write(temporary, &*document, table ? &*table : nullptr,
                "mxi_untrusted", false, &steps);
    std::filesystem::rename(temporary, out);
    mxi::print("Wrote %s\n", out.c_str());
    return 0;
  } catch (const std::exception &e) {
    mxi::eprint("mxi untrusted: %s\n", e.what());
    return 1;
  }
}

// mxi_integrate: summation integration on real images.
//
//   mxi_integrate integrated.expt master.nxs -o mine.refl --d-min 2.0
//   mxi_integrate ... --save-shoeboxes            # keep the pixels and the
//   mask
//
// Predicts, builds each reflection's measurement box, fills it from the
// images, fits the background and sums the foreground. The output is a DIALS
// reflection table, so `dials.show`, `dials.image_viewer` and the scaler all
// read it, and `intensity.sum.value` can be compared with DIALS' own directly.
//
// --save-shoeboxes keeps the pixels and the mask in the table, as mxi_mask
// does. They are large -- a megabyte per hundred reflections -- and they are
// the difference between "the intensity is wrong" and "the intensity is wrong
// because the mask is here and the spot is there".

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/expt.hh"
#include "../src/refl.hh"
#include "args.hh"
#include "background.hh"
#include "fill_row.hh"
#include "fit_batch.hh"
#include "integrate.hh"
#include "log_mirror.hh"
#include "mask.hh"
#include "postrefine.hh"
#include "predict.hh"
#include "profile_model.hh"
#include "reference.hh"
#include "shoebox.hh"
#include "summary.hh"
#include "timing.hh"

#include "decompress.hh"
#include "rflx.hh"
#include "series.hh"

#include <memory>
#include <stdexcept>

namespace mxi {

namespace {

//: Wall clock, in seconds. Wall rather than CPU: the question is how long
//: someone waits, and on a seven minute job most of the answer may be the
//: disk.
double now_wall() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

//: Frames decompressed ahead of the loop that consumes them.
//:
//: Decompressing is half of a large integration -- 158 of 310 seconds on ten
//: rotations -- and it is per frame with nothing shared, so it threads. What
//: does NOT thread is the bookkeeping: a shoebox spans several frames and is
//: opened, filled and closed in frame order, so the consumer must see frames
//: in order and one at a time.
//:
//: So the threads only fetch and decompress. They put finished frames into a
//: buffer keyed by frame number and the consumer takes them in sequence,
//: which keeps every shoebox decision on one thread and needs no locking
//: around the boxes at all. HDF5 cannot be entered from two threads at once,
//: so each worker holds its own Reader and the library's own mutex serialises
//: the fetch; the decompression, which is the expensive part, runs outside it.
//:
//: The buffer is bounded because a decompressed frame of a 16M detector is 72
//: MB and running ahead without limit would be a way to exhaust memory
//: instead of time.
class FrameQueue {
public:
  struct Decoded {
    std::int64_t number = -1;
    unsigned bit_depth = 0;
    std::size_t height = 0;
    std::size_t width = 0;
    std::vector<std::uint8_t> pixels;
  };

  FrameQueue(std::size_t depth) : depth_(std::max<std::size_t>(depth, 1)) {}

  //: Called by a worker when a frame is ready. Never blocks.
  //:
  //: Bounding the buffer here deadlocks: a worker holding frame zero waits for
  //: room while the buffer is full of frames one to eight, and the consumer
  //: waits for frame zero. The bound belongs on how far ahead work is handed
  //: out, not on how much has come back -- see `wait_for_room`, which cannot
  //: deadlock because keys are handed out in order and the frame the consumer
  //: is waiting for was therefore claimed before any frame ahead of it.
  void put(Decoded frame) {
    std::lock_guard<std::mutex> held(lock_);
    ready_.emplace(frame.number, std::move(frame));
    arrived_.notify_all();
  }

  //: Called by a worker before it starts on `which`: holds it back until the
  //: consumer is close enough behind.
  void wait_for_room(std::size_t which) {
    std::unique_lock<std::mutex> held(lock_);
    room_.wait(held, [&] { return which < consumed_ + depth_ || stop_; });
  }

  //: Called by the consumer when it has finished with a frame.
  void consumed(std::size_t which) {
    std::lock_guard<std::mutex> held(lock_);
    consumed_ = which + 1;
    room_.notify_all();
  }

  //: Called by a worker that read nothing for a key, so the consumer does not
  //: wait for a frame that will never come.
  void skip(std::int64_t number) {
    std::unique_lock<std::mutex> held(lock_);
    missing_.insert(number);
    arrived_.notify_all();
  }

  //: The frame with this number, once some worker has produced it. False when
  //: no worker ever will.
  bool take(std::int64_t number, Decoded *out) {
    std::unique_lock<std::mutex> held(lock_);
    arrived_.wait(held, [&] {
      return ready_.count(number) > 0 || missing_.count(number) > 0 || stop_;
    });
    if (stop_)
      return false;
    const auto found = ready_.find(number);
    if (found == ready_.end()) {
      missing_.erase(number);
      return false;
    }
    *out = std::move(found->second);
    ready_.erase(found);
    room_.notify_all();
    return true;
  }

  void halt() {
    std::lock_guard<std::mutex> held(lock_);
    stop_ = true;
    arrived_.notify_all();
    room_.notify_all();
  }

private:
  std::mutex lock_;
  std::condition_variable arrived_;
  std::condition_variable room_;
  std::map<std::int64_t, Decoded> ready_;
  std::set<std::int64_t> missing_;
  std::size_t depth_;
  std::size_t consumed_ = 0;
  bool stop_ = false;
};

void usage(const char *program) {
  std::printf(
      "usage: %s [options] EXPT [INDEXED_REFL]\n"
      "\n"
      "  -o FILE           where to write (integrated.refl)\n"
      "  --output-expt PATH   the models integrated with, and the profile\n"
      "                    model used (integrated.expt)\n"
      "  --images PATH     the image file; by default the .expt's own\n"
      "                    imageset template is used\n"
      "  --postrefine      integrate, refine against the centres integration\n"
      "                    measured, and integrate again with the refined\n"
      "                    models, which are what --output-expt then holds.\n"
      "                    Removes the z offset the spot finder's centres\n"
      "                    leave in a refined model\n"
      "  --postrefine-points N   control points for its scan-varying pass;\n"
      "                    default one per 10 degrees, at least 5, as\n"
      "                    mxi_refine --scan-varying\n"
      "  --sigma-b B --sigma-m M   profile model. Estimated from INDEXED_REFL\n"
      "                    if given -- indexed or refined reflections, not a\n"
      "                    spot finder's -- else taken from EXPT's profile "
      "block\n"
      "  --n-sigma N       foreground spans plus and minus N sigma (3)\n"
      "  --box-scale S     box is S times wider than the foreground (1.9)\n"
      "  --min-zeta Z      skip reflections nearer the rotation axis than "
      "this\n"
      "                    (0.05). A reflection's extent in phi goes as "
      "1/|zeta|,\n"
      "                    so the smallest are forty images deep\n"
      "  --d-min D         resolution limit\n"
      "  --d-max D         low resolution limit: nothing integrated with d "
      "above\n"
      "                    D A -- reflections beside a backstop, say\n"
      "  --first-image N --last-image N   restrict to part of the scan\n"
      "  --gain G          detector gain, counts per photon (1)\n"

      "  --save-background-parameters\n"
      "                    also write each reflection's background "
      "dispersion,\n"
      "                    variance over mean, over every background pixel "
      "and\n"
      "                    trimmed, with how many pixels: under investigation\n"
      "  --save-shoeboxes  keep the pixels and the mask in the output\n"
      "  --threads N       threads fetching and decompressing frames; 0 is "
      "one\n"
      "                    per core, 1 is none (0)\n"
      "  --window N        frames read per chunk (64); boxes stay open across\n"
      "                    chunks, so this bounds memory, not re-reading\n"
      "  --max-boxes N     boxes opened per chunk (6000 a thread, at least\n"
      "                    20000); shortens the chunk\n"
      "                    when it bites, about 31 kB a box\n"
      "  --summation-only  skip profile fitting\n"
      "  --two-pass        read the images twice, to learn and then to fit, "
      "as\n"
      "                    before one pass; the same answer, byte for byte\n"
      "  -g, --gpu         profile fitting on the GPU, in single precision; "
      "the\n"
      "                    CPU's is in double. One pass only\n"
      "  --gpu-emulate     the same single-precision fitting on the CPU, for\n"
      "                    testing without a device\n"
      "  --grid-points N   the profile grid is 2N+1 a side (4)\n"
      "  --subdivisions N  pixel subdivisions per axis (5, as Kabsch uses)\n"
      "  --regions N       detector divided N by N for reference profiles (3)\n"
      "  --scan-blocks N   the scan divided N ways as well (one per 10 "
      "degrees)\n"
      "  --reference-signal S   learn from reflections above S sigma (10)\n"
      "  --least-measured F  a fit needs this fraction of the reflection to\n"
      "                    have been recorded (0.6); below it the intensity "
      "is\n"
      "                    written but not flagged as fitted\n"
      "  --save-profiles F the learned reference profiles, as text; several\n"
      "                    sweeps a file each, F_0, F_1 before F's extension\n"
      "  --timing          where the time went, by phase\n",
      program);
}

//: One frame, decompressed into counts.
struct Frame {
  std::vector<std::int32_t> pixels;
  std::size_t fast = 0;
  std::size_t slow = 0;
};

} // namespace

int run_program(int argc, char **argv);

// Integrate; refine against the centres that integration measured; integrate
// again with the refined models. Composed of whole runs of this program rather
// than of a function, because the integration is still one body in
// run_program: the models are written to --output-expt between the two, and
// the second run reads them as any later program would.
int integrate_with_postrefinement(const Arguments &args, const char *program,
                                  const std::set<std::string> &takes_value) {
  const std::string out_refl = args.value("-o", "integrated.refl");
  const std::string out_expt = args.value("--output-expt", "integrated.expt");
  const std::string first_refl = out_refl + ".before-postrefinement.refl";
  const std::string first_expt = out_expt + ".before-postrefinement.expt";
  std::size_t points = 0;
  if (args.has("--postrefine-points")) {
    const double n = args.number("--postrefine-points", 0.0);
    if (!(n >= 2.0)) {
      std::fprintf(stderr,
                   "mxi_integrate: --postrefine-points needs at least 2\n");
      return 2;
    }
    points = static_cast<std::size_t>(n);
  }

  // A command line for one run: this one's options, less those that belong to
  // the post-refinement, with its own models and output. The first run keeps no
  // shoeboxes or profiles, since only the second run's are the result.
  const auto command = [&](const std::string &expt, const std::string &refl,
                           bool first) {
    std::vector<std::string> words = {program, expt};
    if (args.positional.size() == 2)
      words.push_back(args.positional[1]);
    for (const auto &[flag, value] : args.options) {
      if (flag == "--postrefine" || flag == "--postrefine-points" ||
          flag == "--output-expt" || flag == "-o")
        continue;
      if (first && (flag == "--save-shoeboxes" || flag == "--save-profiles"))
        continue;
      words.push_back(flag);
      if (takes_value.count(flag))
        words.push_back(value);
    }
    words.push_back("-o");
    words.push_back(refl);
    // Every run writes its models: the first to a file of its own, removed
    // with its reflections; the second to --output-expt, over the post-refined
    // models it read, now with the profile model it used.
    words.push_back("--output-expt");
    words.push_back(first ? first_expt : out_expt);
    return words;
  };
  const auto run = [](std::vector<std::string> words) {
    std::vector<char *> argv;
    for (std::string &w : words)
      argv.push_back(w.data());
    argv.push_back(nullptr);
    return run_program(static_cast<int>(words.size()), argv.data());
  };

  std::printf("Integrating with the models as given, then refining against the "
              "centres that measures, then integrating again\n\n");
  std::printf("=== Integration with the models as given ===\n");
  int status = run(command(args.positional[0], first_refl, true));
  if (status != 0)
    return status;

  try {
    ExperimentList experiments = read_experiments(args.positional[0]);
    const Table first = read_reflections(first_refl);
    const PostrefineResult result = postrefine(experiments, first, points);
    std::printf("\n=== Post-refinement ===\n");
    std::printf(
        "Refining against %zu of the %zu integrated reflections: summed, "
        "and with a centre of mass\n",
        result.selected, result.candidates);
    const RefineResult &st = result.refinement.static_pass;
    const RefineResult &sv = result.refinement.varying_pass;
    if (st.n_used == 0) {
      std::fprintf(stderr,
                   "mxi_integrate: post-refinement fitted nothing, so there "
                   "is nothing to integrate again with\n");
      std::remove(first_refl.c_str());
      std::remove(first_expt.c_str());
      return 1;
    }
    std::printf(
        "  scan-static:  %3zu parameters, %zu reflections, RMSD %.4f px, "
        "%.4f px, %.4f images\n",
        st.n_parameters, st.n_used, st.rmsd_x, st.rmsd_y, st.rmsd_z);
    if (result.refinement.varied) {
      std::printf(
          "  scan-varying: %3zu parameters, %zu reflections, RMSD %.4f px, "
          "%.4f px, %.4f images; %zu control points, the detector held\n",
          sv.n_parameters, sv.n_used, sv.rmsd_x, sv.rmsd_y, sv.rmsd_z,
          experiments[0].crystal ? experiments[0].crystal->A_points.size() : 0);
    }
    write_experiments(out_expt, experiments);
    std::printf("Wrote the post-refined models to %s\n\n", out_expt.c_str());
  } catch (const std::exception &error) {
    std::fprintf(stderr, "mxi_integrate: post-refinement: %s\n", error.what());
    std::remove(first_refl.c_str());
    std::remove(first_expt.c_str());
    return 1;
  }
  std::remove(first_refl.c_str());
  std::remove(first_expt.c_str());

  std::printf("=== Integration with the post-refined models ===\n");
  return run(command(out_expt, out_refl, false));
}

// Several sweeps, as DIALS integrates them: each experiment alone -- a list of
// one, and the reflections indexed to it -- in a whole run of this program,
// --postrefine and all, then the tables joined, each row with its
// experiment's index as its id, and the experiments, each with its own profile
// model. A list of one never comes here, so one sweep is integrated as it
// always was.
int integrate_several(const Arguments &args, const char *program,
                      const std::set<std::string> &takes_value,
                      const json::Value &document) {
  // --save-profiles: each sweep learns its own reference profiles, so each
  // writes its own file, its index before the extension -- profiles.txt as
  // profiles_0.txt, profiles_1.txt.
  const auto profiles_of = [](const std::string &path, std::size_t sweep) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    const bool has_ext =
        dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return has_ext ? path.substr(0, dot) + "_" + std::to_string(sweep) +
                         path.substr(dot)
                   : path + "_" + std::to_string(sweep);
  };
  const std::string out_refl = args.value("-o", "integrated.refl");
  const std::string out_expt = args.value("--output-expt", "integrated.expt");
  const json::Array &experiments =
      document.as_object().at("experiment").as_array();
  const std::size_t n = experiments.size();
  const bool has_refl = args.positional.size() == 2;

  std::vector<std::string> made; // the temporary files, removed at the end
  const auto tidy = [&] {
    for (const std::string &f : made)
      std::remove(f.c_str());
  };
  const auto run = [](std::vector<std::string> words) {
    std::vector<char *> argv;
    for (std::string &w : words)
      argv.push_back(w.data());
    argv.push_back(nullptr);
    return run_program(static_cast<int>(words.size()), argv.data());
  };

  try {
    Table indexed;
    if (has_refl) {
      indexed = read_reflections(args.positional[1]);
      if (!indexed.has("id")) {
        std::fprintf(stderr,
                     "mxi_integrate: %s has no id column, so which of the %zu "
                     "sweeps each reflection belongs to is not known\n",
                     args.positional[1].c_str(), n);
        return 1;
      }
    }
    std::printf("Integrating %zu sweeps, each alone, into one table\n", n);
    std::vector<Table> tables;
    std::vector<json::Value> lists;
    for (std::size_t i = 0; i < n; ++i) {
      const std::string stem = out_refl + ".sweep" + std::to_string(i);
      const std::string in_expt = stem + ".in.expt";
      const std::string in_refl = stem + ".in.refl";
      const std::string got_refl = stem + ".refl";
      const std::string got_expt = stem + ".expt";
      made.insert(made.end(), {in_expt, in_refl, got_refl, got_expt});
      json::dump_file(in_expt, slice_experiment_list(document, i));
      const json::Value &ident = experiments[i]["identifier"];
      const std::string identifier =
          ident.is_string() ? ident.as_string() : std::string();
      if (has_refl) {
        const Column &id = indexed.at("id");
        std::vector<std::size_t> rows;
        for (std::size_t r = 0; r < indexed.nrows; ++r)
          if (id.integer(r) == static_cast<std::int64_t>(i))
            rows.push_back(r);
        Table part = select_rows_with_shoeboxes(indexed, rows);
        Column &own = part.modify_int_column("id", "int", 1);
        std::fill(own.ints.begin(), own.ints.end(), 0);
        part.identifiers.clear();
        if (!identifier.empty())
          part.identifiers[0] = identifier;
        write_reflections(in_refl, part);
      }
      std::vector<std::string> words = {program, in_expt};
      if (has_refl)
        words.push_back(in_refl);
      for (const auto &[flag, value] : args.options) {
        if (flag == "-o" || flag == "--output-expt")
          continue;
        words.push_back(flag);
        if (flag == "--save-profiles")
          words.push_back(profiles_of(value, i));
        else if (takes_value.count(flag))
          words.push_back(value);
      }
      words.insert(words.end(), {"-o", got_refl, "--output-expt", got_expt});
      std::printf("\n=== Sweep %zu of %zu ===\n", i + 1, n);
      std::fflush(stdout);
      const int status = run(words);
      if (status != 0) {
        tidy();
        return status;
      }
      Table got = read_reflections(got_refl);
      Column &id = got.modify_int_column("id", "int", 1);
      std::fill(id.ints.begin(), id.ints.end(), static_cast<std::int64_t>(i));
      got.identifiers.clear();
      if (!identifier.empty())
        got.identifiers[i] = identifier;
      // The image set's index in the joined list, as join_experiment_lists
      // renumbers it: the image sets of the sweeps before, plus its own in
      // its list of one. The sweep's run wrote 0, its list's; left so, every
      // sweep's spots fall on the first image set in dials.image_viewer.
      json::Value list = read_experiment_document(got_expt);
      std::int64_t imageset = 0;
      for (const json::Value &earlier : lists)
        imageset +=
            static_cast<std::int64_t>(earlier["imageset"].as_array().size());
      const json::Value &own = list["experiment"].as_array().at(0)["imageset"];
      if (own.is_number())
        imageset += static_cast<std::int64_t>(own.as_number());
      if (got.has("imageset_id")) {
        Column &imagesets = got.modify_int_column("imageset_id", "int", 1);
        std::fill(imagesets.ints.begin(), imagesets.ints.end(), imageset);
      }
      tables.push_back(std::move(got));
      lists.push_back(std::move(list));
    }
    const Table joined = concat_rows_with_shoeboxes(tables);
    write_reflections(out_refl, joined);
    // One crystal, shared, if the sweeps came in sharing one and none changed
    // it -- as without --postrefine none does: each sweep's list carried a
    // copy, and DIALS writes it once. Postrefined, they may differ, and stay
    // apart.
    json::Value joined_expt = join_experiment_lists(lists);
    bool shared = true;
    for (const json::Value &x : experiments)
      shared =
          shared && x["crystal"].is_number() &&
          x["crystal"].as_number() == experiments[0]["crystal"].as_number();
    if (shared)
      share_identical(&joined_expt, "crystal");
    json::dump_file(out_expt, joined_expt);
    tidy();
    std::printf("\n");
    for (std::size_t i = 0; i < n; ++i)
      std::printf("  sweep %zu: %zu reflections\n", i, tables[i].nrows);
    std::printf("Wrote %zu reflections of %zu sweeps to %s, and the models to "
                "%s\n",
                joined.nrows, n, out_refl.c_str(), out_expt.c_str());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "mxi_integrate: %s\n", error.what());
    tidy();
    return 1;
  }
}

int run_program(int argc, char **argv) {
  const std::set<std::string> known = {"-o",
                                       "--sigma-b",
                                       "--sigma-m",
                                       "--n-sigma",
                                       "--box-scale",
                                       "--d-min",
                                       "--d-max",
                                       "--first-image",
                                       "--last-image",
                                       "--gain",
                                       "--save-shoeboxes",
                                       "--images",
                                       "--timing",
                                       "--threads",
                                       "--window",
                                       "--max-boxes",
                                       "--grid-points",
                                       "--subdivisions",
                                       "--regions",
                                       "--scan-blocks",
                                       "--reference-signal",
                                       "--summation-only",
                                       "--save-background-parameters",
                                       "--two-pass",
                                       "-g",
                                       "--gpu",
                                       "--gpu-emulate",
                                       "--save-profiles",
                                       "--min-zeta",
                                       "--least-measured",
                                       "--postrefine",
                                       "--postrefine-points",
                                       "--output-expt"};
  std::set<std::string> takes_value = known;
  takes_value.erase("--save-shoeboxes");
  takes_value.erase("--timing");
  takes_value.erase("--summation-only");
  takes_value.erase("--save-background-parameters");
  takes_value.erase("--two-pass");
  takes_value.erase("-g");
  takes_value.erase("--gpu");
  takes_value.erase("--gpu-emulate");
  takes_value.erase("--postrefine");
  Arguments args = parse_arguments(argc, argv, known, takes_value);
  // One .rflx stands for the experiment list and the reflections
  // (docs/rflx.md).
  args.positional = rflx::as_pair(args.positional);
  if (args.help) {
    usage(argv[0]);
    return 0;
  }
  if (!args.ok) {
    std::fprintf(stderr, "mxi_integrate: %s\n", args.error.c_str());
    return 2;
  }
  if (args.positional.empty() || args.positional.size() > 2) {
    std::fprintf(stderr,
                 "mxi_integrate: expected an .expt and optionally a .refl of "
                 "indexed reflections\n");
    usage(argv[0]);
    return 2;
  }
  const std::string strong_path =
      args.positional.size() == 2 ? args.positional[1] : std::string();

  // Several sweeps: each integrated alone, then joined.
  {
    json::Value document;
    try {
      document = read_experiment_document(args.positional[0]);
    } catch (const std::exception &) {
      // Read again below, where a bad file is reported as it always was.
    }
    if (document.is_object() && document["experiment"].is_array() &&
        document["experiment"].as_array().size() > 1)
      return integrate_several(args, argv[0], takes_value, document);
  }
  if (args.has("--postrefine"))
    return integrate_with_postrefinement(args, argv[0], takes_value);
  // Refused rather than ignored: a run that silently drops an option has used
  // settings nobody chose.
  for (const char *needs : {"--postrefine-points"}) {
    if (args.has(needs)) {
      std::fprintf(stderr, "mxi_integrate: %s is for --postrefine\n", needs);
      return 2;
    }
  }

  try {
    const double t_start = now_wall();
    double t_profile = 0.0, t_predict = 0.0, t_boxes = 0.0;
    double t_fetch = 0.0, t_decompress = 0.0, t_fill = 0.0, t_open = 0.0;
    double t_region = 0.0;
    double t_integrate = 0.0, t_write = 0.0;

    const ExperimentList experiments = read_experiments(args.positional[0]);
    if (experiments.size() == 0) {
      std::fprintf(stderr, "mxi_integrate: no experiments\n");
      return 1;
    }
    const Experiment &e = experiments[0];
    const Panel &panel = e.detector[0];

    const double t_profile_start = now_wall();
    MaskOptions mask_options;
    mask_options.box_scale = args.number("--box-scale", 1.9);
    // How near the rotation axis a reflection may be and still be integrated.
    //
    // This is the lever on the near-axis reflections, and they are worth a
    // word. A reflection's extent in phi is n_sigma sigma_m / |zeta|, so at the
    // default cut of 0.05 a box is forty images deep and its foreground is two
    // thousand voxels of which a handful hold the reflection. Those are the
    // whole of the summation disagreement with DIALS: setting them aside takes
    // the correlation from 0.939 to 1.000.
    //
    // They also cost time twice over, since a window has to read every frame
    // its longest reflection reaches, which is what drives the re-read rate.
    mask_options.min_zeta = args.number("--min-zeta", 0.05);

    // The profile model, in order of preference: what the command line says,
    // then what this package estimates from the strong spots, then what the
    // .expt was carrying. Estimating is the default because the numbers should
    // not have to be carried by hand between two programs, and because these
    // estimates are the ones the rest of this package was measured with.
    double sigma_b = args.number("--sigma-b", 0.0);
    double sigma_m = args.number("--sigma-m", 0.0);
    double n_sigma = args.number("--n-sigma", 0.0);
    const char *source = "the command line";
    if ((!(sigma_b > 0.0) || !(sigma_m > 0.0)) && !strong_path.empty()) {
      const Table strong = read_reflections(strong_path);
      const std::vector<Shoebox> strong_boxes = decode_shoeboxes(strong);
      if (strong_boxes.empty()) {
        std::fprintf(stderr,
                     "mxi_integrate: %s has no shoeboxes, so the profile model "
                     "cannot be estimated from it\n",
                     strong_path.c_str());
        return 1;
      }
      // The profile model needs INDEXED spots -- each spot's predicted
      // diffracted beam -- and a spot finder's strong.refl has none. Said in
      // those terms, because "no column 's1'" is true and names nothing a
      // user can do.
      if (!strong.has("s1") || !strong.has("xyzcal.mm")) {
        std::fprintf(
            stderr,
            "mxi_integrate: %s has no predictions for its spots, so the "
            "profile model cannot be estimated from it. Give the "
            "refined or indexed reflections (refined.refl), not the "
            "spot finder's, or set --sigma-b and --sigma-m\n",
            strong_path.c_str());
        return 1;
      }
      const Column &s1_in = strong.at("s1");
      const Column &cal_in = strong.at("xyzcal.mm");
      std::vector<Shoebox> selected;
      std::vector<Vec3> beams;
      std::vector<RangeSample> samples;
      for (std::size_t i = 0; i < strong.nrows && i < strong_boxes.size();
           ++i) {
        if (strong.has("flags") &&
            (strong.at("flags").integer(i) & flag::kUsedInRefinement) == 0) {
          continue;
        }
        const Vec3 beam{s1_in.real(i, 0), s1_in.real(i, 1), s1_in.real(i, 2)};
        if (std::fabs(compute_zeta(e, beam)) < 0.05)
          continue;
        if (!has_prediction(strong, i))
          continue;
        selected.push_back(strong_boxes[i]);
        beams.push_back(beam);
        for (const RangeSample &sample :
             range_samples(e, strong_boxes[i], cal_in.real(i, 2),
                           compute_zeta(e, beam))) {
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
      source = "estimated from the indexed spots";
    }
    if ((!(sigma_b > 0.0) || !(sigma_m > 0.0)) && experiments.profile.present) {
      if (!(sigma_b > 0.0))
        sigma_b = experiments.profile.sigma_b;
      if (!(sigma_m > 0.0))
        sigma_m = experiments.profile.sigma_m;
      if (!(n_sigma > 0.0))
        n_sigma = experiments.profile.n_sigma;
      source = "the profile block of the .expt";
    }
    if (!(sigma_b > 0.0) || !(sigma_m > 0.0)) {
      std::fprintf(stderr,
                   "mxi_integrate: no profile model. Give a .refl of strong "
                   "spots with shoeboxes to estimate it from, or --sigma-b and "
                   "--sigma-m, or an .expt that carries a profile block.\n");
      return 1;
    }
    mask_options.sigma_d = sigma_b;
    mask_options.sigma_m = sigma_m;
    mask_options.n_sigma = n_sigma > 0.0 ? n_sigma : 3.0;
    t_profile = now_wall() - t_profile_start;
    std::printf("Profile model: sigma_b %.4f deg, sigma_m %.4f deg (%s); "
                "foreground to %.1f sigma\n",
                sigma_b, sigma_m, source, mask_options.n_sigma);
    IntegrateOptions integrate_options;
    integrate_options.gain = args.number("--gain", 1.0);

    // The images. series::nxmx unpacks the NXmx virtual dataset so each
    // frame's compressed bytes can be read without HDF5's filter pipeline;
    // decompress::image then turns them into pixels.
    // Where the images are: the .expt says, and --images overrides it. The
    // experiment list already carries the path and being told it twice is how
    // the two come to disagree -- but a file moved since it was written needs
    // the override, so both exist and the program says which it used.
    std::string image_path = args.value("--images", "");
    const char *image_source = "--images";
    if (image_path.empty()) {
      image_path = experiments.image_template;
      image_source = "the .expt's imageset template";
      if (image_path.empty()) {
        std::fprintf(stderr,
                     "mxi_integrate: %s has no imageset template, so there is "
                     "nothing to read images from; give --images\n",
                     args.positional[0].c_str());
        return 1;
      }
      if (image_path.find('#') != std::string::npos) {
        std::fprintf(stderr,
                     "mxi_integrate: the imageset template is %s, which names "
                     "a numbered series rather than one file; give --images\n",
                     image_path.c_str());
        return 1;
      }
    }
    std::printf("Images: %s (from %s)\n", image_path.c_str(), image_source);

    std::unique_ptr<series::Series> images = series::nxmx(image_path);
    series::Info info;
    if (!images || !images->try_open(&info)) {
      std::fprintf(stderr,
                   "mxi_integrate: cannot open %s. If the data have moved "
                   "since the .expt was written, give --images\n",
                   image_path.c_str());
      return 1;
    }

    std::size_t workers =
        static_cast<std::size_t>(args.number("--threads", 0.0));
    if (workers == 0) {
      workers = std::thread::hardware_concurrency();
      if (workers == 0)
        workers = 1;
    }

    PredictOptions predict_options;
    predict_options.d_min = args.number("--d-min", 0.0);
    const double d_max = args.number("--d-max", 0.0);
    // Prediction was 39.7 per cent of a large integration and all of it on one
    // thread. One unit of work per h, joined in h order, so the list is the
    // same whatever the thread count.
    predict_options.threads = workers;
    const double t_predict_start = now_wall();
    const std::vector<Prediction> predictions = predict(e, predict_options);
    t_predict = now_wall() - t_predict_start;
    // As frames' array indices, the scan's own by default: a scan from image
    // 151 begins at frame 150.
    const double first_image = args.number("--first-image", e.scan.z_first());
    const double last_image = args.number("--last-image", e.scan.z_last());
    std::printf("Predicted %zu reflections\n", predictions.size());

    // TWO PASSES, AND WHY
    //
    // Every shoebox cannot exist at once. Ten rotations of insulin is 7.4
    // million reflections and, at about 3500 voxels each, 233 GB of pixels. So
    // the first pass works out every bounding box and keeps nothing but the
    // box, and the second walks the scan in blocks of images, holding only the
    // shoeboxes of the block it is on.
    //
    // A reflection belongs to the block its FIRST image falls in, and a block
    // reads whatever frames its reflections span -- which is a few images past
    // its own end. Splitting a reflection across two blocks would integrate
    // half of it twice, so the blocks overlap in what they read and never in
    // what they own.
    struct Planned {
      const Prediction *prediction;
      std::int32_t bbox[6];
    };
    const double t_boxes_start = now_wall();
    std::vector<Planned> planned;
    std::map<std::string, std::size_t> refused;
    std::size_t outside_range = 0;
    for (const Prediction &p : predictions) {
      if (p.z < first_image || p.z > last_image) {
        ++outside_range;
        continue;
      }
      if (d_max > 0.0 && e.crystal &&
          resolution(*e.crystal, p.h, p.k, p.l) > d_max) {
        ++refused["lower resolution than --d-max"];
        continue;
      }
      Planned item;
      item.prediction = &p;
      BoxRejection why = BoxRejection::kNone;
      if (!integration_bbox(e, p, mask_options, item.bbox, &why)) {
        ++refused[describe(why)];
        continue;
      }
      planned.push_back(item);
    }
    t_boxes = now_wall() - t_boxes_start;
    {
      std::size_t left_out = outside_range;
      for (const auto &entry : refused)
        left_out += entry.second;
      std::printf("Integrating %zu of them", planned.size());
      if (left_out > 0) {
        std::printf("; not integrated:\n");
        if (outside_range > 0)
          std::printf("  %8zu  outside the image range\n", outside_range);
        for (const auto &entry : refused)
          std::printf("  %8zu  %s\n", entry.second, entry.first.c_str());
      } else {
        std::printf("\n");
      }
    }
    if (planned.empty())
      return 1;

    // In order of first image, so a block is a contiguous run of this vector.
    std::sort(planned.begin(), planned.end(),
              [](const Planned &a, const Planned &b) {
                return a.bbox[4] < b.bbox[4];
              });

    // ONE PASS OVER THE FRAMES, WITH SHOEBOXES OPEN ACROSS THEM
    //
    // Blocks of images do not work here. A reflection near the rotation axis
    // spans n_sigma sigma_m / |zeta| in phi, which at the zeta cut is about a
    // hundred and fifty frames, and one of those in a block forces the whole
    // block to read that far -- and the next block to read it again. On three
    // hundred frames with blocks of ten that was 2951 reads of 300 frames.
    //
    // So the frames are walked once in order instead. A shoebox opens when the
    // frame it starts on comes round, takes a slice from every frame it spans,
    // and is integrated and released on the frame it ends. Every frame is read
    // exactly once and memory is bounded by what is open at the time, which is
    // what the block size was trying to bound anyway.
    Table out;
    out.nrows = planned.size();
    Column &miller =
        out.int_column("miller_index", "cctbx::miller::index<>", 3);
    Column &panel_column = out.int_column("panel", "std::size_t", 1);
    Column &id = out.int_column("id", "int", 1);
    Column &imageset = out.int_column("imageset_id", "int", 1);
    Column &flags = out.int_column("flags", "std::size_t", 1);
    Column &entering = out.int_column("entering", "bool", 1);
    Column &bbox = out.int_column("bbox", "int6", 6);
    Column &n_fg = out.int_column("num_pixels.foreground", "int", 1);
    Column &n_bg = out.int_column("num_pixels.background", "int", 1);
    Column &n_val = out.int_column("num_pixels.valid", "int", 1);
    Column &cal_px = out.real_column("xyzcal.px", "vec3<double>", 3);
    Column &cal_mm = out.real_column("xyzcal.mm", "vec3<double>", 3);
    Column &s1_column = out.real_column("s1", "vec3<double>", 3);
    Column &isum = out.real_column("intensity.sum.value", "double", 1);
    Column &ivar = out.real_column("intensity.sum.variance", "double", 1);
    Column &bmean = out.real_column("background.mean", "double", 1);
    Column &bsum = out.real_column("background.sum.value", "double", 1);
    Column &bsumvar = out.real_column("background.sum.variance", "double", 1);
    Column &qe_column = out.real_column("qe", "double", 1);
    Column &lp_column = out.real_column("lp", "double", 1);
    Column &obs_px = out.real_column("xyzobs.px.value", "vec3<double>", 3);
    Column &obs_px_var =
        out.real_column("xyzobs.px.variance", "vec3<double>", 3);
    Column &obs_mm = out.real_column("xyzobs.mm.value", "vec3<double>", 3);
    Column &d_column = out.real_column("d", "double", 1);
    // Where the reflection WAS against where it was predicted, in the frame
    // the images are in: pixels on the detector and images along the scan.
    // Both ends are in the same frame -- the prediction is the pixel that
    // fires, parallax included, and the centroid is of the counts that pixel
    // recorded -- so this is a straight difference and needs no correction.
    //
    // Not a DIALS column. For post-analysis of how well positions were
    // predicted, which is otherwise a join and a subtraction every time.
    Column &res_px = out.real_column("xyzres.px.value", "vec3<double>", 3);
    Column &res_px_var =
        out.real_column("xyzres.px.variance", "vec3<double>", 3);
    Column &iprf = out.real_column("intensity.prf.value", "double", 1);
    Column &iprf_var = out.real_column("intensity.prf.variance", "double", 1);
    Column &prf_cc = out.real_column("profile.correlation", "double", 1);
    // How much of each reflection the detector actually recorded. Not a DIALS
    // column; written because 11 per cent of reflections here touch a module
    // gap and 5 per cent lose half their box to one, and nothing else in the
    // table says so.
    Column &measured = out.real_column("profile.measured", "double", 1);
    Column &zeta_column = out.real_column("zeta", "double", 1);
    Column &part_column = out.real_column("partiality", "double", 1);
    Column &partial_id = out.int_column("partial_id", "std::size_t", 1);
    Column &n_bg_used = out.int_column("num_pixels.background_used", "int", 1);
    // The background's dispersion, under --save-background-parameters only:
    // measured while it is under investigation (docs/backstop.md), so that by
    // default the table is what it was.
    const bool save_background = args.has("--save-background-parameters");
    Column *bg_dispersion =
        save_background ? &out.real_column("background.dispersion", "double", 1)
                        : nullptr;
    Column *bg_dispersion_trimmed =
        save_background
            ? &out.real_column("background.dispersion_trimmed", "double", 1)
            : nullptr;
    Column *n_bg_trimmed =
        save_background
            ? &out.int_column("num_pixels.background_trimmed", "int", 1)
            : nullptr;
    Column &obs_mm_var =
        out.real_column("xyzobs.mm.variance", "vec3<double>", 3);

    const bool save = args.has("--save-shoeboxes");
    std::string shoebox_bytes;
    // Saved shoeboxes come out in the order they close, which is not the order
    // of the table, so they are held by row and encoded at the end.
    std::vector<Shoebox> saved;
    if (save)
      saved.resize(planned.size());

    std::unique_ptr<series::Reader> reader = images->reader();
    // The keys, in the order the series gives them. Reading each one to build
    // a frame-number-to-key map first would read every chunk twice, which on
    // the thirty degree sweep was most of the run: 18 of 21 seconds spent
    // fetching compressed bytes that were then fetched again.
    //
    // The loop below is driven by the frames as they arrive instead, and
    // checks they arrive in order rather than assuming it -- the Series
    // interface promises keys that can be read, not keys in sequence.
    const std::vector<std::string> keys = images->ready();

    series::Frame frame;
    std::vector<std::uint8_t> pixels;
    std::size_t frames_read = 0;
    std::size_t frames_missing = 0;
    // Which frames any shoebox wanted, so "frames read" can be compared with
    // something rather than left to be compared with the image count.
    std::set<std::int32_t> wanted;
    std::size_t bad_pixels = 0;
    std::size_t most_open = 0;
    const Vec3 s0 = e.beam.s0();
    const Vec3 axis = e.goniometer.lab_axis();

    // The shoeboxes currently open, by their row in the table.

    const auto close = [&](std::size_t row, Shoebox *box) -> bool {
      const Prediction &p = *planned[row].prediction;
      const IntegratedReflection r = integrate_shoebox(box, integrate_options);
      miller.ints[row * 3 + 0] = p.h;
      miller.ints[row * 3 + 1] = p.k;
      miller.ints[row * 3 + 2] = p.l;
      panel_column.ints[row] = static_cast<std::int64_t>(p.panel);
      id.ints[row] = 0;
      imageset.ints[row] = 0;
      // DIALS' convention for a reflection that reaches a masked pixel, read
      // from its source and confirmed on a real integration. A foreground
      // with pixels missing makes the summed intensity not this reflection's
      // intensity -- summation cannot put back what the gap took -- so it is
      // not flagged as integrated by summation, and it says why. The profile
      // fitted intensity, which does put it back, keeps its own flag.
      //
      // Setting kIntegratedSum on these regardless let 1415 gap-crossing
      // reflections into dials.scale where DIALS lets 62, and the combined
      // intensity leans on the sum for strong reflections -- so a truncated
      // sum flagged as good is what scaling rejected, clustered along every
      // module edge.
      flags.ints[row] = flag::kPredicted | summation_flags(r);
      entering.ints[row] = p.s1.dot(axis.cross(s0)) > 0.0 ? 1 : 0;
      for (int k = 0; k < 6; ++k)
        bbox.ints[row * 6 + k] = box->bbox[k];
      n_fg.ints[row] = static_cast<std::int64_t>(r.n_foreground);
      n_bg.ints[row] = static_cast<std::int64_t>(r.n_background);
      n_val.ints[row] = static_cast<std::int64_t>(r.n_valid);
      cal_px.reals[row * 3 + 0] = p.px_fast;
      cal_px.reals[row * 3 + 1] = p.px_slow;
      cal_px.reals[row * 3 + 2] = p.z;
      const auto mm = panel.px_to_mm(p.px_fast, p.px_slow);
      cal_mm.reals[row * 3 + 0] = mm.first;
      cal_mm.reals[row * 3 + 1] = mm.second;
      cal_mm.reals[row * 3 + 2] = p.phi;
      for (int k = 0; k < 3; ++k)
        s1_column.reals[row * 3 + k] = p.s1[k];
      isum.reals[row] = r.intensity;
      ivar.reals[row] = r.variance;
      bmean.reals[row] = r.background_mean;
      bsum.reals[row] = r.background_sum;
      bsumvar.reals[row] = r.background_sum_variance;
      qe_column.reals[row] = quantum_efficiency(panel, p.s1);
      lp_column.reals[row] = lorentz_polarization(e.beam, e.goniometer, p.s1);
      d_column.reals[row] =
          e.crystal ? resolution(*e.crystal, p.h, p.k, p.l) : 0.0;
      const double z_of = compute_zeta(e, p.s1);
      zeta_column.reals[row] = z_of;
      part_column.reals[row] =
          partiality(e.scan, p.phi, z_of, mask_options.sigma_m, box->bbox[4],
                     box->bbox[5]);
      // Every reflection here is its own, since nothing splits one across two
      // rows; DIALS uses this to tie the pieces of a split reflection together.
      partial_id.ints[row] = static_cast<std::int64_t>(row);
      // Everything not foreground was used: this integrator has no second
      // round of rejection on top of the GLM's own weighting.
      n_bg_used.ints[row] = static_cast<std::int64_t>(r.n_background);
      if (save_background) {
        bg_dispersion->reals[row] = r.background_dispersion;
        bg_dispersion_trimmed->reals[row] = r.background_dispersion_trimmed;
        n_bg_trimmed->ints[row] =
            static_cast<std::int64_t>(r.n_background_trimmed);
      }
      // The centroid variance in millimetres and radians. The px values it
      // comes from do not reproduce DIALS' and neither will these.
      obs_mm_var.reals[row * 3 + 0] =
          r.centroid_variance_fast * panel.pixel_size[0] * panel.pixel_size[0];
      obs_mm_var.reals[row * 3 + 1] =
          r.centroid_variance_slow * panel.pixel_size[1] * panel.pixel_size[1];
      obs_mm_var.reals[row * 3 + 2] = r.centroid_variance_z *
                                      Scan::radians(e.scan.osc_width) *
                                      Scan::radians(e.scan.osc_width);
      const double of = r.centroid_valid ? r.centroid_fast : p.px_fast;
      const double os = r.centroid_valid ? r.centroid_slow : p.px_slow;
      const double oz = r.centroid_valid ? r.centroid_z : p.z;
      obs_px.reals[row * 3 + 0] = of;
      obs_px.reals[row * 3 + 1] = os;
      obs_px.reals[row * 3 + 2] = oz;
      obs_px_var.reals[row * 3 + 0] = r.centroid_variance_fast;
      obs_px_var.reals[row * 3 + 1] = r.centroid_variance_slow;
      obs_px_var.reals[row * 3 + 2] = r.centroid_variance_z;
      // The residual, NaN where there was no signal to find a centre in. The
      // observed column falls back to the prediction there because dials.scale
      // stops on a NaN in it; this column is ours, so it can say "nothing
      // measured" honestly instead of claiming a residual of exactly zero.
      //
      // The variance is the centroid's only. The prediction has an
      // uncertainty too, from the refined model's covariance, and it is not
      // propagated here -- so a pull of residual over sigma larger than one
      // is prediction error PLUS whatever that leaves out.
      {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        // From the UNCLIPPED centre of mass, whose uncertainty was checked;
        // not from xyzobs.px.value, whose is overconfident. So this is not
        // xyzobs - xyzcal, and is not meant to be.
        const bool has = r.unbiased_valid;
        res_px.reals[row * 3 + 0] = has ? r.unbiased_fast - p.px_fast : nan;
        res_px.reals[row * 3 + 1] = has ? r.unbiased_slow - p.px_slow : nan;
        res_px.reals[row * 3 + 2] = has ? r.unbiased_z - p.z : nan;
        res_px_var.reals[row * 3 + 0] = has ? r.unbiased_variance_fast : nan;
        res_px_var.reals[row * 3 + 1] = has ? r.unbiased_variance_slow : nan;
        res_px_var.reals[row * 3 + 2] = has ? r.unbiased_variance_z : nan;
      }
      const auto obs_mm_pair = panel.px_to_mm(of, os);
      obs_mm.reals[row * 3 + 0] = obs_mm_pair.first;
      obs_mm.reals[row * 3 + 1] = obs_mm_pair.second;
      obs_mm.reals[row * 3 + 2] = e.scan.phi_from_z(oz);
      return r.valid;
    };

    // WINDOWS, AND WHY NOT A PIPELINE
    //
    // Threading only the reads settles at about one core in use. The thread
    // that owns the shoeboxes has 111 of the 310 seconds of work on it --
    // building masks, filling, background and summation -- so the readers
    // finish their lookahead and wait. No amount of reader threads fixes that.
    //
    // The in-order constraint that forced the pipeline is not actually
    // needed. A frame writes only its own z plane of a shoebox, so two frames
    // of the same box can be filled by two threads at once without touching
    // the same double. What needs ordering is nothing; what needs a box to
    // exist is everything.
    //
    // The scan is read in chunks of frames, and a box stays open from the
    // chunk its first frame is in to the chunk its last frame is in -- so a
    // frame is read once a pass however deep the boxes crossing it are.
    // Within a chunk the boxes are built, the frames fetched, decompressed
    // and filled, and the finished boxes integrated, each in parallel.
    //
    // The chunk is short because the boxes now outlive it: what is open at
    // once is the chunk's worth of boxes plus those still running on from
    // before, and a thousand frames of those is tens of gigabytes. Sixty-four
    // frames keeps sixteen threads decompressing four frames each.
    const std::size_t window =
        static_cast<std::size_t>(std::max(1.0, args.number("--window", 64.0)));
    // A cap on the boxes opened in one chunk, which shortens the chunk rather
    // than dropping boxes when it bites.
    // Boxes opened a chunk, by default 6000 a thread and never fewer than
    // 20000. A chunk ends where opening stops, so on dense data a fixed 20000
    // cut ferritin's 64 frame chunks to some 11 frames, and 11 of 32 threads
    // had a frame to read: a third of them busy. The cap bounds memory, so it
    // grows with the threads that need the frames, not without limit.
    const std::size_t default_boxes =
        std::max<std::size_t>(20000, 6000 * workers);
    const std::size_t max_boxes = static_cast<std::size_t>(std::max(
        1.0, args.number("--max-boxes", static_cast<double>(default_boxes))));

    //: Run `count` units of work over the pool, by index.
    // Run body(i, worker) for i in [0, count), where worker is this call's
    // number for the thread running it: 0 for the calling thread, 1 to n - 1
    // for the threads started here. It is stable within one call and means
    // nothing across calls, so anything kept per thread must be indexed by it
    // and not by thread_local state.
    //
    // thread_local lane numbers were a data race. The threads are new on every
    // call but the calling thread takes part in all of them, so its lane,
    // chosen once, outlived the call that chose it -- while each later call's
    // new threads were numbered from zero again. A probe caught two threads
    // sharing lane 0 in 32 calls of a thirty image run: the calling thread did
    // the whole of three small early calls on lane 0 and kept it. In profile
    // learning that meant two threads adding into the same partial profiles at
    // once. ThreadSanitizer saw nothing on that run only because the two rarely
    // both learned a reflection in the same call.
    const auto in_parallel_by_worker = [&](std::size_t count, auto &&body) {
      if (count == 0)
        return;
      const std::size_t n = std::min(workers, count);
      if (n <= 1) {
        for (std::size_t i = 0; i < count; ++i)
          body(i, std::size_t{0});
        return;
      }
      std::atomic<std::size_t> next_unit{0};
      std::vector<std::thread> pool;
      pool.reserve(n - 1);
      const auto run = [&](std::size_t worker) {
        for (;;) {
          const std::size_t i = next_unit.fetch_add(1);
          if (i >= count)
            break;
          body(i, worker);
        }
      };
      for (std::size_t t = 1; t < n; ++t)
        pool.emplace_back(run, t);
      run(0);
      for (std::thread &t : pool)
        t.join();
    };
    const auto in_parallel = [&](std::size_t count, auto &&body) {
      in_parallel_by_worker(count,
                            [&](std::size_t i, std::size_t) { body(i); });
    };

    // ONE PASS OVER THE IMAGES, AND WHY IT GIVES THE TWO PASSES' ANSWER
    //
    // The reference profiles are learned from the reflections themselves, so
    // none exists until something has been integrated, and the images used to
    // be read twice: once to sum and to learn, once to fit. But a reflection's
    // profile comes only from the cells near it, and a cell is complete once
    // the scan has passed every box that learns into its block. So one pass
    // holds each shoebox until the blocks its profile is interpolated from are
    // final, fits it and lets it go. That is NOT a profile learned from part of
    // a scan and applied to the rest: every fit uses the finished profile it
    // would have used anyway, the profiles are learned in the same order, and
    // a held box is the box the second pass would rebuild -- so the table is
    // the same bytes, which python/tests/test_single_pass.py holds. --two-pass
    // keeps the old way, to compare against.
    GridSpec grid_spec;
    grid_spec.n = static_cast<int>(args.number("--grid-points", 4.0));
    grid_spec.sigma_d = sigma_b;
    grid_spec.sigma_m = sigma_m;
    grid_spec.half_width = mask_options.n_sigma;
    grid_spec.subdivisions =
        static_cast<int>(args.number("--subdivisions", 5.0));
    // One scan block per 10 degrees of the range integrated, unless told: the
    // profile drifts along the scan, so a block should be a fixed ANGLE rather
    // than a fixed fraction of the scan -- and a reflection is fitted only once
    // the block after its own is complete, so the block's length is how long
    // its shoebox must wait in a single pass. Five blocks whatever the scan,
    // the old default, made 72 degree blocks of a 360 degree scan.
    const double range_degrees =
        std::abs(e.scan.osc_width) * std::max(last_image - first_image, 1.0);
    const int scan_blocks =
        args.has("--scan-blocks")
            ? std::max(1, static_cast<int>(args.number("--scan-blocks", 1.0)))
            : std::max(1, static_cast<int>(std::lround(range_degrees / 10.0)));
    ReferenceProfiles reference = make_reference(
        grid_spec, static_cast<int>(args.number("--regions", 3.0)), scan_blocks,
        e.detector.size(), first_image, last_image);
    const bool fitting = !args.has("--summation-only");
    // ONE PASS over the images unless --two-pass: a reflection is fitted as
    // soon as the scan blocks its profile is interpolated from are complete,
    // its shoebox held until then. See "One pass over the images" in
    // docs/integration.md; the answer is the two passes' own, byte for byte.
    const bool single_pass = fitting && !args.has("--two-pass");
    // Which reflections are worth learning from: strong, nearly whole, and
    // mostly inside the grid. DIALS marks these `reference_spot`.
    const double least_signal = args.number("--reference-signal", 10.0);
    // How much of a reflection must have been measured for its fit to count.
    const double least_measured = args.number("--least-measured", 0.6);

    // For one pass: the scan block each reflection learns into, the last block
    // its fit waits for -- the neighbours the interpolation uses, exactly --
    // and the frame after which every box learning into a block has closed.
    // All known before a frame is read: they depend on positions alone.
    const int n_blocks = std::max(reference.blocks, 1);
    std::vector<std::int32_t> block_done(
        static_cast<std::size_t>(n_blocks),
        std::numeric_limits<std::int32_t>::min());
    std::vector<int> learns_into, waits_for;
    if (single_pass) {
      learns_into.assign(planned.size(), 0);
      waits_for.assign(planned.size(), 0);
      in_parallel(planned.size(), [&](std::size_t r) {
        const Prediction &q = *planned[r].prediction;
        learns_into[r] = block_of_cell(
            reference,
            reference.region_of(panel, static_cast<std::size_t>(q.panel),
                                q.px_fast, q.px_slow, q.z));
        int last = 0;
        for (const Neighbour &nb :
             neighbours_of(reference, panel, static_cast<std::size_t>(q.panel),
                           q.px_fast, q.px_slow, q.z))
          if (nb.weight > 0.0)
            last = std::max(last, block_of_cell(reference, nb.region));
        waits_for[r] = last;
      });
      for (std::size_t r = 0; r < planned.size(); ++r) {
        std::int32_t &done =
            block_done[static_cast<std::size_t>(learns_into[r])];
        done = std::max(done, planned[r].bbox[5]);
      }
    }
    // Blocks final in order, as their boxes all close: each against its own
    // average if it has spots, else the latest earlier one's, else -- no
    // earlier block having any -- it waits for the first later one that
    // does. finalise_reference's rule, a block at a time.
    int finalised_through = -1;
    std::vector<int> waiting_empty;
    std::vector<double> latest_average;
    bool have_average = false;
    const auto finalise_closed = [&](std::int32_t closed_to) {
      while (finalised_through + 1 < n_blocks &&
             block_done[static_cast<std::size_t>(finalised_through + 1)] <=
                 closed_to) {
        const int b = ++finalised_through;
        std::vector<double> average;
        if (block_average(reference, b, &average)) {
          for (int w : waiting_empty)
            finalise_block(&reference, w, 10, average);
          waiting_empty.clear();
          finalise_block(&reference, b, 10, average);
          latest_average = average;
          have_average = true;
        } else if (have_average) {
          finalise_block(&reference, b, 10, latest_average);
        } else {
          waiting_empty.push_back(b);
        }
      }
    };
    // Every block up to this one is final.
    const auto final_through = [&]() {
      return waiting_empty.empty() ? finalised_through
                                   : waiting_empty.front() - 1;
    };
    std::vector<Shoebox> held;
    std::vector<std::size_t> held_rows;
    // A held box's cells, if it was learned from: fitting uses them rather
    // than computing them again.
    std::vector<PixelCells> held_cells;
    std::size_t most_held = 0;
    double most_held_bytes = 0.0;

    // One reflection's profile fit, which both ways of reading the images
    // call: against its own pixels, with the reference profile interpolated
    // between the neighbouring cells and carried onto them.
    // The fit's time, summed across threads, by stage: interpolating the
    // reference, carrying it onto the pixels, and the least squares.
    ThreadSeconds t_fit_interpolate, t_fit_onto, t_fit_solve;
    // A fit recorded in the table, the CPU's or the device's alike.
    // Why a reflection was not profile fitted, counted, the CPU's way and the
    // device's: scaling takes only profile-fitted observations, so every one
    // lost here is lost to the merged data, and a third of a data set's
    // failing said nothing of which reason it was.
    std::atomic<std::size_t> prf_no_box{0}, prf_no_reference{0},
        prf_no_pixels{0}, prf_off_foreground{0}, prf_masked{0},
        prf_degenerate{0}, prf_device{0}, prf_too_little{0};
    // Each reflection's reason, written as profile.failure: 0 fitted, else why
    // not (FitFailure), so that where a data set's fits fail can be looked at.
    std::vector<std::uint8_t> failure_of(planned.size(), 0);
    const auto failed = [&](std::size_t row, FitFailure why) {
      failure_of[row] = static_cast<std::uint8_t>(why);
      std::atomic<std::size_t> *counter = &prf_degenerate;
      switch (why) {
      case FitFailure::no_box:
        counter = &prf_no_box;
        break;
      case FitFailure::no_reference:
        counter = &prf_no_reference;
        break;
      case FitFailure::no_pixels:
        counter = &prf_no_pixels;
        break;
      case FitFailure::profile_off_foreground:
        counter = &prf_off_foreground;
        break;
      case FitFailure::foreground_masked:
        counter = &prf_masked;
        break;
      case FitFailure::too_little:
        counter = &prf_too_little;
        break;
      case FitFailure::device:
        counter = &prf_device;
        break;
      default:
        break;
      }
      counter->fetch_add(1, std::memory_order_relaxed);
    };
    const auto record_fit = [&](std::size_t row, const ProfileFit &fit) {
      if (!fit.valid) {
        failed(row,
               fit.why == FitFailure::none ? FitFailure::degenerate : fit.why);
        return;
      }
      // A fit is an extrapolation when part of the reflection is missing,
      // and past some point it is guesswork dressed as a measurement. The
      // intensity is still written, so it can be looked at; the flag that
      // says it was profile fitted is not, so nothing downstream merges it
      // by accident.
      measured.reals[row] = fit.measured;
      if (fit.measured < least_measured) {
        failed(row, FitFailure::too_little);
        return;
      }
      iprf.reals[row] = fit.intensity;
      iprf_var.reals[row] = fit.variance;
      prf_cc.reals[row] = fit.correlation;
      flags.ints[row] |= flag::kIntegratedPrf;
    };

    const auto fit_one = [&](std::size_t row, Shoebox &box,
                             const PixelCells *cells) {
      if (box.data.empty()) {
        failed(row, FitFailure::no_box);
        return;
      }
      const Prediction &q = *planned[row].prediction;
      // The background the GLM found in the first pass, put back so the
      // fit subtracts the same thing the sum did.
      box.background.assign(box.size(), static_cast<float>(bmean.reals[row]));
      // No transform here. The second pass fits against the pixels, so
      // carrying the counts onto the grid is work whose answer is thrown
      // away -- and it is the same cost as carrying the profile back, so
      // doing both doubled this phase.
      //
      // A weighted average of the nearby profiles, not the nearest one:
      // taking the nearest makes the model jump at a cell boundary, so two
      // reflections either side of one are fitted with different profiles.
      const double f0 = Timing::now();
      const std::vector<double> local =
          profile_at(reference, panel, static_cast<std::size_t>(q.panel),
                     q.px_fast, q.px_slow, q.z);
      const double f1 = Timing::now();
      t_fit_interpolate.add(f1 - f0);
      if (local.size() != grid_spec.size()) {
        failed(row, FitFailure::no_reference);
        return;
      }
      // Fitted against the PIXELS, with the profile carried onto them,
      // rather than against the grid with the pixels carried onto it. The
      // two give the same intensity and very different variances: the grid
      // has more points than the shoebox has pixels and one pixel's counts
      // reach several of them, so treating its points as independent
      // overcounts the information. Measured against DIALS, the grid fit
      // claimed a variance 0.35 of the summed one at high resolution where
      // DIALS has 0.84 -- errors too small by 1.6, and everything weighted
      // by them wrong.
      const std::vector<double> on_pixels =
          profile_on_pixels(e, box, q.s1, q.phi, grid_spec, local, cells);
      const double f2 = Timing::now();
      t_fit_onto.add(f2 - f1);
      if (on_pixels.empty()) {
        failed(row, FitFailure::no_pixels);
        return;
      }
      const ProfileFit fit =
          fit_on_pixels(box, on_pixels, integrate_options.gain);
      t_fit_solve.add(Timing::now() - f2);
      record_fit(row, fit);
    };

    std::vector<Shoebox> boxes;
    std::size_t at = 0;
    int pass = 0;
    std::size_t references_used = 0;
    double t_transform = 0.0, t_fit = 0.0;
    // Boxes OUTLIVE a chunk of frames. Each chunk opens the boxes that start
    // in it, reads its frames once into every open box, and closes the boxes
    // whose last frame it held.
    //
    // It used to be windows of boxes that read every frame they touched and
    // then discarded the boxes. A box starting in one window and ending past
    // it made that window read on into the next one's frames, and the next
    // window read them again for its own -- and near-axis reflections run forty
    // to eighty frames deep, so every boundary repeated them. On a 3600 frame
    // Eiger 16M run that was 20224 frames read for 3600 wanted, 5.62 reads a
    // frame over two passes where 2.0 is the floor. Decompression was the
    // same 10 to 12 ms a frame as the spot finder; there were five times as
    // many of them.
    // --gpu: profile fitting in single precision, on the device, a batch of
    // boxes at a time; --gpu-emulate the same on the CPU. One pass only.
    const bool emulate_gpu = args.has("--gpu-emulate");
    bool use_gpu = emulate_gpu || args.has("-g") || args.has("--gpu");
    if (use_gpu && !single_pass) {
      std::printf("--gpu fits in one pass only; with --two-pass the fitting is "
                  "on the CPU\n");
      use_gpu = false;
    }
    const bool on_device =
        use_gpu && !emulate_gpu && fit_device_name() != nullptr;
    if (use_gpu && !emulate_gpu && !on_device) {
      // The emulation runs the kernel's algorithm, which recomputes each
      // pixel's cells for every slice -- cheap on a device, several times the
      // work on a CPU -- so without a device the fit is the CPU's own.
      std::printf("--gpu: no usable device (%s); fitting on the CPU, in double "
                  "precision\n",
                  fit_device_unavailable_reason().c_str());
      use_gpu = false;
    } else if (on_device)
      std::printf("Profile fitting on %s, in single precision\n",
                  fit_device_name());
    FitBatch fit_batch = make_fit_batch(e, grid_spec, integrate_options.gain);
    ThreadSeconds t_fit_batched, t_gpu_prepare, t_gpu_pack, t_gpu_device;
    std::size_t gpu_boxes = 0;
    double gpu_bytes = 0.0;
    // Batches started on the device and not yet collected, oldest first.
    struct PendingFit {
      int ticket;
      std::vector<std::size_t> rows;
    };
    std::deque<PendingFit> pending_fits;
    ThreadSeconds t_gpu_wait;
    const auto collect_oldest_fit = [&]() {
      PendingFit p = std::move(pending_fits.front());
      pending_fits.pop_front();
      std::vector<fitdev::FitF> fits;
      const double t0 = Timing::now();
      if (!fit_batch_collect(p.ticket, &fits))
        throw std::runtime_error("profile fitting on the device failed");
      t_gpu_wait.add(Timing::now() - t0);
      for (std::size_t n = 0; n < p.rows.size(); ++n) {
        // A device's fit that failed says only that: the reason is the
        // CPU's to give (a run without --gpu).
        ProfileFit fit = to_profile_fit(fits[n]);
        if (!fit.valid)
          fit.why = FitFailure::device;
        record_fit(p.rows[n], fit);
      }
    };
    const auto fit_on_gpu = [&](const std::vector<std::size_t> &which) {
      const std::size_t chunk = 4096;
      for (std::size_t from = 0; from < which.size(); from += chunk) {
        const std::size_t to = std::min(which.size(), from + chunk);
        const double t_prepare = Timing::now();
        std::vector<std::vector<double>> locals(to - from);
        in_parallel(to - from, [&](std::size_t n) {
          Shoebox &box = held[which[from + n]];
          if (box.data.empty())
            return;
          const std::size_t row = held_rows[which[from + n]];
          const Prediction &q = *planned[row].prediction;
          // A saved box has its background, as fit_one gives it; the device
          // takes the background as one number, so otherwise it is not written
          // -- four bytes a voxel for nothing.
          if (save)
            box.background.assign(box.size(),
                                  static_cast<float>(bmean.reals[row]));
          locals[n] =
              profile_at(reference, panel, static_cast<std::size_t>(q.panel),
                         q.px_fast, q.px_slow, q.z);
        });
        const double t_packing = Timing::now();
        t_gpu_prepare.add(t_packing - t_prepare);
        clear_batch(&fit_batch);
        std::vector<std::size_t> in_batch;
        std::vector<BatchEntry> entries;
        for (std::size_t n = 0; n < to - from; ++n) {
          const Shoebox &box = held[which[from + n]];
          const std::size_t row = held_rows[which[from + n]];
          if (box.data.empty()) {
            failed(row, FitFailure::no_box);
            continue;
          }
          if (locals[n].size() != grid_spec.size()) {
            failed(row, FitFailure::no_reference);
            continue;
          }
          const Prediction &q = *planned[row].prediction;
          entries.push_back({&box, q.s1, q.phi, bmean.reals[row], &locals[n]});
          in_batch.push_back(row);
        }
        // A slot must be free before the batch can be packed into it.
        if (on_device)
          while (static_cast<int>(pending_fits.size()) >= kFitSlots)
            collect_oldest_fit();
        add_to_batch(&fit_batch, entries, on_device);
        gpu_boxes += fit_batch.boxes.size();
        gpu_bytes +=
            static_cast<double>(fit_batch.voxels) * (sizeof(float) + 1) +
            static_cast<double>(fit_batch.reference_floats) * sizeof(float);
        const double t_device = Timing::now();
        t_gpu_pack.add(t_device - t_packing);
        // On a device the batch is started and not waited for: the threads go
        // back to reading frames while it fits, and its fits are collected
        // when a slot is wanted again or the last boxes are done. The GPU and
        // the CPU took turns before, each idle while the other worked.
        int ticket = -1;
        if (on_device) {
          while (static_cast<int>(pending_fits.size()) >= kFitSlots)
            collect_oldest_fit();
          ticket = fit_batch_submit(fit_batch);
        }
        if (ticket >= 0) {
          pending_fits.push_back({ticket, std::move(in_batch)});
        } else {
          const std::vector<fitdev::FitF> fits = fit_batch_emulated(fit_batch);
          for (std::size_t n = 0; n < in_batch.size(); ++n) {
            ProfileFit fit = to_profile_fit(fits[n]);
            if (!fit.valid)
              fit.why = FitFailure::device;
            record_fit(in_batch[n], fit);
          }
        }
        t_gpu_device.add(Timing::now() - t_device);
        t_fit_batched.add(Timing::now() - t_prepare);
      }
    };

    // Fit every held box whose scan blocks are all final -- or every one, at
    // the end -- in parallel, and let it go.
    const auto fit_ready = [&](bool everything) {
      most_held = std::max(most_held, held.size());
      double bytes = 0.0;
      for (const Shoebox &box : held)
        bytes += static_cast<double>(box.data.size() * sizeof(float) +
                                     box.mask.size());
      for (const PixelCells &c : held_cells)
        bytes += static_cast<double>((c.start.size() + c.cell.size()) *
                                         sizeof(std::uint32_t) +
                                     c.hits.size() * sizeof(std::uint16_t));
      most_held_bytes = std::max(most_held_bytes, bytes);
      const int ready = final_through();
      std::vector<std::size_t> now;
      for (std::size_t k = 0; k < held.size(); ++k)
        if (everything || waits_for[held_rows[k]] <= ready)
          now.push_back(k);
      // The last call collects every fit still on the device before the table
      // is written -- whether or not it has boxes of its own to fit, since
      // earlier calls have usually fitted them all.
      const auto collect_everything = [&]() {
        if (everything)
          while (!pending_fits.empty())
            collect_oldest_fit();
      };
      if (now.empty()) {
        collect_everything();
        return;
      }
      const double t0 = now_wall();
      if (use_gpu) {
        fit_on_gpu(now);
        collect_everything();
      } else
        in_parallel(now.size(), [&](std::size_t n) {
          fit_one(held_rows[now[n]], held[now[n]], &held_cells[now[n]]);
        });
      t_fit += now_wall() - t0;
      std::vector<bool> gone(held.size(), false);
      for (std::size_t k : now) {
        gone[k] = true;
        if (save) {
          to_dials_convention(&held[k]);
          saved[held_rows[k]] = std::move(held[k]);
        }
      }
      std::vector<Shoebox> keep;
      std::vector<std::size_t> keep_rows;
      std::vector<PixelCells> keep_cells;
      for (std::size_t k = 0; k < held.size(); ++k)
        if (!gone[k]) {
          keep.push_back(std::move(held[k]));
          keep_rows.push_back(held_rows[k]);
          keep_cells.push_back(std::move(held_cells[k]));
        }
      held.swap(keep);
      held_rows.swap(keep_rows);
      held_cells.swap(keep_cells);
    };
    std::atomic<bool> learned_into_final{false};

    std::vector<Shoebox> active;
    std::vector<std::size_t> active_rows;
    std::int32_t chunk_start = planned.empty() ? 0 : planned[0].bbox[4];
    while (at < planned.size() || !active.empty()) {
      if (active.empty() && at < planned.size()) {
        chunk_start = std::max(chunk_start, planned[at].bbox[4]);
      }
      const std::int32_t chunk_limit =
          chunk_start + static_cast<std::int32_t>(window);
      // Open everything starting before the chunk's end -- capped by
      // --max-boxes, but never part way through the boxes of one frame, and
      // when the cap bites the chunk ENDS where opening stopped. Otherwise a
      // box not yet opened would miss the frames the chunk reads.
      std::size_t stop = at;
      while (stop < planned.size() && planned[stop].bbox[4] < chunk_limit) {
        if (stop - at >= max_boxes && planned[stop].bbox[4] > chunk_start)
          break;
        ++stop;
      }
      const std::int32_t chunk_end =
          (stop < planned.size() && planned[stop].bbox[4] < chunk_limit)
              ? planned[stop].bbox[4]
              : chunk_limit;
      const std::size_t opening = stop - at;

      // The new boxes, built in parallel: this is the mask, and it was 46
      // seconds before it was.
      const double t_open_start = now_wall();
      const std::size_t first_new = active.size();
      active.resize(first_new + opening);
      active_rows.resize(first_new + opening);
      in_parallel(opening, [&](std::size_t i) {
        active_rows[first_new + i] = at + i;
        BoxRejection ignored = BoxRejection::kNone;
        Shoebox &box = active[first_new + i];
        if (build_shoebox(e, *planned[at + i].prediction, mask_options, &box,
                          &ignored)) {
          box.data.assign(box.size(), 0.0f);
        }
      });
      at = stop;
      t_open += now_wall() - t_open_start;
      most_open = std::max(most_open, active.size());

      // Which open boxes each of THIS chunk's frames touches.
      std::map<std::int32_t, std::vector<std::size_t>> touching;
      for (std::size_t i = 0; i < active.size(); ++i) {
        const std::int32_t lo = std::max(active[i].bbox[4], chunk_start);
        const std::int32_t hi = std::min(active[i].bbox[5], chunk_end);
        for (std::int32_t z = lo; z < hi; ++z)
          touching[z].push_back(i);
      }
      std::vector<std::int32_t> frame_numbers;
      frame_numbers.reserve(touching.size());
      for (const auto &entry : touching)
        frame_numbers.push_back(entry.first);

      // Fetch, decompress and fill, in parallel over frames. Each frame writes
      // only its own z plane of each box it touches, so two frames of one box
      // never touch the same voxel.
      std::atomic<std::size_t> frames_done{0};
      std::atomic<std::size_t> bad_here{0};
      std::atomic<std::size_t> unread{0};
      std::vector<double> fetch_by_thread(std::max<std::size_t>(workers, 1),
                                          0.0);
      std::vector<double> decompress_by_thread(fetch_by_thread.size(), 0.0);
      std::vector<double> fill_by_thread(fetch_by_thread.size(), 0.0);
      const double t_region_start = now_wall();
      in_parallel_by_worker(
          frame_numbers.size(), [&](std::size_t which, std::size_t worker) {
            // A reader per thread, kept for the thread's life: it belongs to
            // one thread only, so outliving a call is harmless. Its TIMING lane
            // is the worker number, which is unique within the call; a
            // thread_local lane was shared between the calling thread and a new
            // one.
            thread_local std::unique_ptr<series::Reader> mine;
            if (!mine)
              mine = images->reader();
            const std::size_t slot = worker % fetch_by_thread.size();
            const std::int32_t z = frame_numbers[which];
            if (z < 0 || static_cast<std::size_t>(z) >= keys.size())
              return;
            series::Frame raw;
            const double t0 = now_wall();
            if (!mine->read(keys[static_cast<std::size_t>(z)], &raw)) {
              // A frame the writer never received: an unallocated chunk.
              // Counted, because a shoebox that spans it is missing a slice and
              // will integrate low, and silently dropping it leaves "frames
              // read" less than the number of images with no explanation.
              unread.fetch_add(1);
              return;
            }
            const double t1 = now_wall();
            const std::size_t height = static_cast<std::size_t>(raw.height);
            const std::size_t width = static_cast<std::size_t>(raw.width);
            const std::size_t bytes =
                decompress::frame_bytes(height, width, raw.bit_depth);
            std::vector<std::uint8_t> pixels(bytes);
            decompress::image(raw.data, raw.algorithm, raw.bit_depth, height,
                              width, {pixels.data(), bytes});
            series::apply_pixel_mask(raw, {pixels.data(), bytes});
            const double t2 = now_wall();
            frames_done.fetch_add(1);

            const auto fill = [&](auto typed) {
              using Pixel = decltype(typed);
              const Pixel *const raw_pixels =
                  reinterpret_cast<const Pixel *>(pixels.data());
              std::size_t bad_count = 0;
              for (std::size_t i : touching[z]) {
                Shoebox &box = active[i];
                if (box.data.empty())
                  continue;
                const std::int32_t zi = z - box.bbox[4];
                const std::size_t nx = static_cast<std::size_t>(box.nx());
                for (std::int32_t y = 0; y < box.ny(); ++y) {
                  // A row of the box is a run of the frame's row: converted
                  // in one loop with no branch, which the compiler makes SIMD,
                  // then checked for bad pixels by an OR across it, also SIMD,
                  // and only a row that has one -- rare -- gone over again. A
                  // pixel at a time, with a branch each, filling was two
                  // thirds of reading dense data.
                  const Pixel *const from =
                      raw_pixels +
                      static_cast<std::size_t>(box.bbox[2] + y) * width +
                      static_cast<std::size_t>(box.bbox[0]);
                  float *const to = box.data.data() + box.at(0, y, zi);
                  std::uint8_t *const mask = box.mask.data() + box.at(0, y, zi);
                  // Both markers, the bad pixel's and the tile join's:
                  // fill_row.hh.
                  bad_count += fill_row(from, nx, to, mask);
                }
              }
              bad_here.fetch_add(bad_count);
            };
            if (raw.bit_depth == 16) {
              fill(std::uint16_t{});
            } else if (raw.bit_depth == 32) {
              fill(std::uint32_t{});
            } else {
              throw std::runtime_error("unsupported bit depth " +
                                       std::to_string(raw.bit_depth));
            }
            const double t3 = now_wall();
            fetch_by_thread[slot] += t1 - t0;
            decompress_by_thread[slot] += t2 - t1;
            fill_by_thread[slot] += t3 - t2;
          });
      t_region += now_wall() - t_region_start;
      frames_read += frames_done.load();
      bad_pixels += bad_here.load();
      frames_missing += unread.load();
      // Only frames the series has. A box can reach past the last image, and
      // counting frames that cannot be read made a slice of 300 frames read
      // twice report "356 wanted, each read 1.69 times" instead of 2.00.
      for (const auto &entry : touching) {
        if (entry.first >= 0 &&
            static_cast<std::size_t>(entry.first) < keys.size()) {
          wanted.insert(entry.first);
        }
      }
      // Thread-seconds, summed over threads, NOT the busiest thread's share.
      //
      // Reporting the busiest thread per phase gave fetching 77.1 per cent and
      // decompressing 47.4 per cent of the same run: both are wall clock
      // inside one parallel region, they overlap, and the percentages summed
      // to 124. Thread-seconds are additive and comparable with each other,
      // and the region's own wall clock is reported beside them so the
      // difference between work and waiting is visible.
      const auto summed = [](const std::vector<double> &v) {
        double all = 0.0;
        for (double x : v)
          all += x;
        return all;
      };
      t_fetch += summed(fetch_by_thread);
      t_decompress += summed(decompress_by_thread);
      t_fill += summed(fill_by_thread);

      // Close what is finished: every frame a box spans is now filled.
      std::vector<Shoebox> boxes;
      std::vector<std::size_t> rows;
      {
        std::vector<Shoebox> staying;
        std::vector<std::size_t> staying_rows;
        for (std::size_t i = 0; i < active.size(); ++i) {
          if (active[i].bbox[5] <= chunk_end) {
            boxes.push_back(std::move(active[i]));
            rows.push_back(active_rows[i]);
          } else {
            staying.push_back(std::move(active[i]));
            staying_rows.push_back(active_rows[i]);
          }
        }
        active.swap(staying);
        active_rows.swap(staying_rows);
      }
      const std::size_t count = boxes.size();
      chunk_start = chunk_end;
      // Which of this chunk's boxes one pass holds for fitting: their release
      // saves them, not the save below.
      std::vector<bool> moved_to_held(count, false);
      std::vector<PixelCells> cells_of;

      if (pass == 0) {
        // Integrate, in parallel over boxes.
        const double t_close_start = now_wall();
        in_parallel(count, [&](std::size_t i) {
          if (boxes[i].data.empty())
            return;
          close(rows[i], &boxes[i]);
        });
        t_integrate += now_wall() - t_close_start;

        // Learn from the ones worth learning from, in parallel.
        //
        // The transform is the cost and it is per reflection with nothing
        // shared; only the accumulation into a cell's profile is shared, and
        // that is a few hundred adds against a few hundred thousand
        // multiplies. So each thread keeps its own set of profiles and they
        // are added together at the end -- which also makes the result
        // independent of the thread count, since addition of the same numbers
        // in a different order is the only thing that changes.
        if (fitting) {
          const double t0 = now_wall();
          // FIXED BLOCKS, NOT THREADS. The closing reflections are cut into a
          // fixed number of blocks by index, each summed in index order by one
          // task into its own partial, and the partials added in block order.
          // Which thread runs a block is left to the scheduler and changes
          // nothing: every addition happens in the same order on every run,
          // whatever the thread count. Refinement's reduction already worked
          // this way.
          //
          // It replaced partials per thread, which were a data race (see
          // in_parallel_by_worker) and, even without the race, added in an
          // order the scheduler chose: two four-thread runs differed by 3.6e-11
          // in intensity.prf.value.
          //
          // Sixteen blocks bounds the parallelism of this phase at sixteen,
          // which is a few per cent of a run, and costs sixteen partial sets of
          // profiles: about two megabytes each at 324 cells.
          constexpr std::size_t kBlocks = 16;
          const std::size_t blocks =
              std::min(kBlocks, std::max<std::size_t>(count, 1));
          // The cells each box's transform computes, kept for its fit.
          cells_of.assign(single_pass ? count : 0, PixelCells{});
          std::vector<ReferenceProfiles> partial(blocks, reference);
          for (ReferenceProfiles &r : partial) {
            for (std::vector<double> &p : r.profile)
              std::fill(p.begin(), p.end(), 0.0);
            std::fill(r.spots.begin(), r.spots.end(), 0);
          }
          std::atomic<std::size_t> learned{0};
          in_parallel(blocks, [&](std::size_t b) {
            const std::size_t first = b * count / blocks;
            const std::size_t last = (b + 1) * count / blocks;
            for (std::size_t i = first; i < last; ++i) {
              if (boxes[i].data.empty())
                continue;
              const std::size_t row = rows[i];
              const double signal = isum.reals[row];
              const double sigma = std::sqrt(std::max(ivar.reals[row], 1e-12));
              if (!(signal > least_signal * sigma))
                continue;
              if (part_column.reals[row] < 0.99)
                continue;
              const Prediction &q = *planned[row].prediction;
              const Transformed t =
                  transform_shoebox(e, boxes[i], q.s1, q.phi, grid_spec,
                                    single_pass ? &cells_of[i] : nullptr);
              if (!t.valid || t.outside > 0.05)
                continue;
              const std::size_t region =
                  reference.region_of(panel, static_cast<std::size_t>(q.panel),
                                      q.px_fast, q.px_slow, q.z);
              // Never into a block already final: that would mean the rule
              // for when a block is complete was wrong, and the one pass's
              // answer no longer the two passes'.
              if (single_pass &&
                  block_of_cell(reference, region) <= finalised_through)
                learned_into_final.store(true);
              if (add_reference(&partial[b], region, t))
                learned.fetch_add(1);
            }
          });
          for (const ReferenceProfiles &r : partial) {
            for (std::size_t g = 0; g < reference.profile.size(); ++g) {
              for (std::size_t k = 0; k < reference.profile[g].size(); ++k) {
                reference.profile[g][k] += r.profile[g][k];
              }
              reference.spots[g] += r.spots[g];
            }
          }
          references_used += learned.load();
          t_transform += now_wall() - t0;
          if (learned_into_final.load())
            throw std::logic_error(
                "a reflection learned into a scan block already final: the "
                "one pass's rule for when a block is complete is wrong");
        }
        if (single_pass) {
          // Held until the blocks its profile comes from are final, without
          // its background: the fit puts the GLM's mean back, as the second
          // pass did. Then every block whose boxes have all closed is
          // finalised, and whatever that makes ready is fitted and let go.
          for (std::size_t i = 0; i < count; ++i) {
            if (boxes[i].data.empty())
              continue;
            std::vector<float>().swap(boxes[i].background);
            held.push_back(std::move(boxes[i]));
            held_rows.push_back(rows[i]);
            held_cells.push_back(i < cells_of.size() ? std::move(cells_of[i])
                                                     : PixelCells{});
            moved_to_held[i] = true;
          }
          finalise_closed(chunk_end);
          fit_ready(false);
        }
      } else {
        // Fit, in parallel over boxes: each writes only its own row.
        const double t0 = now_wall();
        in_parallel(
            count, [&](std::size_t i) { fit_one(rows[i], boxes[i], nullptr); });
        t_fit += now_wall() - t0;
      }

      if (save) {
        for (std::size_t i = 0; i < count; ++i) {
          if (moved_to_held[i])
            continue;
          // Into DIALS' convention before it goes to the file.
          //
          // Here a bad pixel keeps its region flag and loses only Valid, so
          // the profile fit can tell a foreground voxel with nothing in it
          // from one that was never foreground -- which is what lets a
          // reflection crossing a module gap keep its intensity. DIALS' own
          // mask calculator sets Foreground and Background only on voxels that
          // are already Valid, so a region bit without Valid never occurs
          // there, and a table carrying them was rejected as an invalid
          // structure. The file is DIALS' format and follows DIALS'
          // convention: a voxel with no measurement is zero, as the spot
          // finder has always written it.
          to_dials_convention(&boxes[i]);
          saved[rows[i]] = std::move(boxes[i]);
        }
      }
      if (at >= planned.size() && active.empty() && pass == 0 && fitting) {
        // Between the passes -- or, in one pass, at the end of the scan: the
        // profiles are what they are going to be.
        if (single_pass) {
          finalise_closed(std::numeric_limits<std::int32_t>::max());
          for (int w : waiting_empty)
            finalise_block(&reference, w, 10,
                           std::vector<double>(reference.spec.size(), 0.0));
          waiting_empty.clear();
          reference.finalised = true;
        } else {
          finalise_reference(&reference);
        }
        const std::string profile_path = args.value("--save-profiles", "");
        if (!profile_path.empty()) {
          std::FILE *f = std::fopen(profile_path.c_str(), "w");
          if (f == nullptr) {
            std::fprintf(stderr, "mxi_integrate: cannot write %s\n",
                         profile_path.c_str());
          } else {
            // Plain text, one profile a block, because the thing that reads
            // this is a person with a plotting script and not a program that
            // needs a format.
            std::fprintf(f, "# reference profiles\n");
            std::fprintf(f, "side %d\n", grid_spec.side());
            std::fprintf(f, "sigma_b %.9g\n", grid_spec.sigma_d);
            std::fprintf(f, "sigma_m %.9g\n", grid_spec.sigma_m);
            std::fprintf(f, "half_width %.9g\n", grid_spec.half_width);
            std::fprintf(f, "divisions %d\n", reference.divisions);
            std::fprintf(f, "blocks %d\n", reference.blocks);
            std::fprintf(f, "panels %zu\n", reference.panels);
            for (std::size_t r = 0; r < reference.profile.size(); ++r) {
              std::fprintf(f, "profile %zu spots %zu\n", r, reference.spots[r]);
              for (double v : reference.profile[r])
                std::fprintf(f, "%.9g\n", v);
            }
            std::fclose(f);
            std::printf("wrote %s\n", profile_path.c_str());
          }
        }
        std::size_t empty = 0;
        for (std::size_t r = 0; r < reference.spots.size(); ++r) {
          if (reference.spots[r] < 10)
            ++empty;
        }
        std::printf(
            "Reference profiles: %zu learned from %zu reflections (%zu of %zu "
            "regions borrowed their scan block's detector average)\n",
            reference.region_count() - empty, references_used, empty,
            reference.region_count());
        if (references_used == 0) {
          std::fprintf(stderr,
                       "mxi_integrate: nothing to learn profiles from, so no "
                       "profile fitting: lower --reference-signal or check the "
                       "summation\n");
          // Nothing learned, so nothing fitted: what one pass held goes out
          // as it was, as the two passes' first pass left it.
          if (single_pass && save)
            for (std::size_t k = 0; k < held.size(); ++k) {
              held[k].background.assign(
                  held[k].size(),
                  static_cast<float>(bmean.reals[held_rows[k]]));
              to_dials_convention(&held[k]);
              saved[held_rows[k]] = std::move(held[k]);
            }
        } else if (single_pass) {
          fit_ready(true);
        } else {
          pass = 1;
          at = 0;
          chunk_start = planned.empty() ? 0 : planned[0].bbox[4];
          continue;
        }
      }
    }
    if (save)
      shoebox_bytes = encode_shoeboxes(saved);
    if (frames_missing > 0) {
      std::fprintf(
          stderr,
          "mxi_integrate: %zu reads found no frame: an unallocated chunk is a "
          "frame the writer never received, and a shoebox spanning one is "
          "missing a slice\n",
          frames_missing);
    }

    // What a user reads to judge the run: the summaries dials.integrate
    // prints, against resolution and overall, from the columns just written.
    {
      SummaryInput summary;
      const std::size_t n = planned.size();
      summary.d.assign(d_column.reals.begin(), d_column.reals.begin() + n);
      summary.flags.assign(flags.ints.begin(), flags.ints.begin() + n);
      summary.intensity_sum.assign(isum.reals.begin(), isum.reals.begin() + n);
      summary.variance_sum.assign(ivar.reals.begin(), ivar.reals.begin() + n);
      summary.intensity_prf.assign(iprf.reals.begin(), iprf.reals.begin() + n);
      summary.variance_prf.assign(iprf_var.reals.begin(),
                                  iprf_var.reals.begin() + n);
      summary.profile_correlation.assign(prf_cc.reals.begin(),
                                         prf_cc.reals.begin() + n);
      summary.background.assign(bmean.reals.begin(), bmean.reals.begin() + n);
      summary.partiality.assign(part_column.reals.begin(),
                                part_column.reals.begin() + n);
      summary.res_fast.resize(n);
      summary.res_slow.resize(n);
      // The OBSERVED centre against the prediction, over reflections that were
      // detected -- which is what dials.integrate's RMSD XY is, so the two can
      // be read side by side. Not xyzres.px: that is the unclipped centre,
      // honest about its noise and so twice as scattered for weak spots
      // (0.544 px against 0.267 on a 300 image insulin sweep, where DIALS gave
      // about 0.25). It is the right thing for judging predicted positions,
      // and the wrong one for a column people will set beside DIALS'.
      // xyzres.px being NaN is what says a reflection was not detected, since
      // xyzobs.px falls back to the prediction and would read as perfect.
      const double nan = std::numeric_limits<double>::quiet_NaN();
      for (std::size_t i = 0; i < n; ++i) {
        // Over reflections integrated by summation, as DIALS reports it: one
        // crossing a module gap has a centre pulled off by the gap, which
        // took the overall figure from 0.267 px to 0.343.
        const bool detected = std::isfinite(res_px.reals[i * 3 + 0]) &&
                              (flags.ints[i] & flag::kIntegratedSum) != 0;
        summary.res_fast[i] =
            detected ? obs_px.reals[i * 3 + 0] - cal_px.reals[i * 3 + 0] : nan;
        summary.res_slow[i] =
            detected ? obs_px.reals[i * 3 + 1] - cal_px.reals[i * 3 + 1] : nan;
      }
      print_summary(stdout, summarise(summary, 10));
    }
    std::printf("\n");

    if (save) {
      Table::Opaque column;
      column.type = "Shoebox<>";
      column.rows = planned.size();
      std::printf("Shoeboxes kept: %.1f MB\n", shoebox_bytes.size() / 1e6);
      column.bytes = std::move(shoebox_bytes);
      out.set_opaque("shoebox", std::move(column));
    }
    {
      // Why each reflection was not profile fitted, 0 if it was: FitFailure's
      // codes, for mxeq failures. DIALS ignores a column it does not know.
      Column &why = out.int_column("profile.failure", "int", 1);
      for (std::size_t r = 0; r < planned.size(); ++r)
        why.ints[r] = failure_of[r];
    }
    if (!e.identifier.empty())
      out.identifiers[0] = e.identifier;

    {
      const std::size_t lost = prf_no_box + prf_no_reference + prf_no_pixels +
                               prf_off_foreground + prf_masked +
                               prf_degenerate + prf_device + prf_too_little;
      if (lost > 0 && fitting) {
        std::printf("Not profile fitted, %zu, and why:\n", lost);
        const auto line = [](std::size_t n, const char *why) {
          if (n > 0)
            std::printf("  %10zu  %s\n", n, why);
        };
        line(prf_no_box, "the box was rejected before fitting");
        line(prf_no_reference,
             "no reference profile for its place and scan block");
        line(prf_no_pixels, "the profile could not be carried onto its pixels");
        line(prf_off_foreground,
             "the profile zero over the whole foreground: off its box");
        line(prf_masked,
             "none of the foreground measured: wholly in a gap, say");
        line(prf_degenerate, "the least squares had no solution");
        line(prf_device, "the device's fit failed (without --gpu says why)");
        char cut[96];
        std::snprintf(cut, sizeof cut,
                      "under %.2f of the profile measured (--least-measured)",
                      least_measured);
        line(prf_too_little, cut);
        std::printf("\n");
      }
    }

    const std::string path = args.value("-o", "integrated.refl");
    const double t_write_start = now_wall();
    write_reflections(path, out);
    t_write = now_wall() - t_write_start;
    std::printf("Wrote %zu reflections to %s\n", planned.size(), path.c_str());
    // And the models integrated with, carrying the profile model used -- what
    // the next program reads: mxi_symmetry integrated.expt integrated.refl.
    {
      ExperimentList written = experiments;
      written.profile.present = true;
      written.profile.sigma_b = sigma_b;
      written.profile.sigma_m = sigma_m;
      written.profile.n_sigma = mask_options.n_sigma;
      const std::string expt_path =
          args.value("--output-expt", "integrated.expt");
      write_experiments(expt_path, written);
      std::printf("Wrote the models, with the profile model, to %s\n",
                  expt_path.c_str());
    }

    if (args.has("--timing")) {
      Timing timing(true, t_start);
      timing.add("the profile model", t_profile);
      timing.add("prediction", t_predict);
      timing.add("bounding boxes", t_boxes);
      timing.add("opening shoeboxes", t_open);
      timing.add("reading frames, wall", t_region);
      timing.add("background and summation", t_integrate);
      timing.add("learning profiles", t_transform);
      timing.add("profile fitting", t_fit);
      timing.add("writing", t_write);
      timing.report(stdout);
      const auto per = [&](double seconds) {
        return t_region > 0.0 ? seconds / t_region : 0.0;
      };
      std::printf("  %zu threads, chunks of %zu frames, at most %zu boxes "
                  "opened a chunk, %zu "
                  "open at once\n",
                  workers, window, max_boxes, most_open);
      std::printf("  %zu frames read, %zu wanted by a shoebox, each read %.2f "
                  "times; %zu voxels "
                  "on masked pixels\n",
                  frames_read, wanted.size(),
                  wanted.empty() ? 0.0
                                 : static_cast<double>(frames_read) /
                                       static_cast<double>(wanted.size()),
                  bad_pixels);
      std::printf(
          "  reading frames, in thread-seconds: fetching %.3f s (%.2f x wall), "
          "decompressing %.3f s (%.2f x), filling shoeboxes %.3f s (%.2f x)\n",
          t_fetch, per(t_fetch), t_decompress, per(t_decompress), t_fill,
          per(t_fill));
      if (use_gpu) {
        double device[3] = {0.0, 0.0, 0.0};
        fit_device_times(device);
        std::printf("  profile fitting in single precision, %s: %.3f s on the "
                    "CPU's side -- "
                    "preparing %.3f s (the local references), packing %.3f s, "
                    "handing over "
                    "%.3f s, waiting for the device %.3f s",
                    on_device ? fit_device_name() : "emulated on the CPU",
                    t_fit_batched.seconds() + t_gpu_wait.seconds(),
                    t_gpu_prepare.seconds(), t_gpu_pack.seconds(),
                    t_gpu_device.seconds(), t_gpu_wait.seconds());
        if (on_device)
          std::printf(" (on the device: uploading %.3f s, %.2f GB for %zu "
                      "boxes, %.1f GB/s; the "
                      "kernel %.3f s; downloading %.3f s)",
                      device[0], gpu_bytes / 1e9, gpu_boxes,
                      device[0] > 0.0 ? gpu_bytes / 1e9 / device[0] : 0.0,
                      device[1], device[2]);
        std::printf("\n");
      }
      std::printf(
          "  profile fitting, in thread-seconds: interpolating the reference "
          "%.3f s, "
          "carrying it onto the pixels %.3f s, the least squares %.3f s\n",
          t_fit_interpolate.seconds(), t_fit_onto.seconds(),
          t_fit_solve.seconds());
      if (single_pass)
        std::printf("  one pass: at most %zu shoeboxes held for fitting, %.2f "
                    "GB of pixels and "
                    "masks\n",
                    most_held, most_held_bytes / 1e9);
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "mxi_integrate: %s\n", error.what());
    return 1;
  }
}

} // namespace mxi

int main(int argc, char **argv) {
  // Mirrored to mxi_integrate.log in the working directory, as DIALS writes
  // dials.<program>.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_integrate.log");
  return mxi::run_program(argc, argv);
}

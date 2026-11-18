// mxi_find: spot finding, the first step after import -- each image
// thresholded, on the CPU or a GPU, the signal pixels grouped in three
// dimensions, and the spots written as a reflection table (docs/spots.md).
//
//   mxi_import /data/ins10_1_master.h5
//   mxi_find imported.rflx --gpu
//
// The threshold (src/dext.{hh,cc}, and the device kernels after it) is the
// extended dispersion threshold, DIALS's default; the grouping, the centroids
// and the table are src/spots.{hh,cc} and src/spot_table.{hh,cc}. The
// experiment list is read, never written: where each sweep's images are, the
// scan's image range, the panel size and the experiment identifier
// (src/experiment_images.{hh,cc}).
//
// Frames are read and thresholded in parallel and grouped in one thread, since
// the grouping is inherently sequential -- a reflection spans frames and a
// component is only finished when a frame arrives without it. Frames therefore
// have to reach the grouping in order, and they are collected a chunk at a
// time: dispatch a chunk, wait for it, sort it, group it. A chunk is a few
// frames per thread, so the barrier costs a fraction of one frame's latency
// per chunk and the alternative -- resequencing a stream whose frame numbers
// may have gaps in it, since a chunk the writer never received is a frame that
// does not exist -- is a great deal more machinery for that fraction.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "args.hh"
#include "decompress.hh"
#include "dext.hh"
#include "experiment_images.hh"
#include "expt.hh"
#include "histogram.hh"
#include "log_mirror.hh"
#include "output.hh"
#include "parallel.hh"
#include "processing.hh"
#include "progress.hh"
#include "queue.hh"
#include "rflx.hh"
#include "spots.hh"
#include "timing.hh"
#include "untrusted.hh"

namespace {
// This run's step of the processing's summary (docs/output.md): begun once,
// on the experiments given, filled as the run reports, written with the table.
mxi::Summary &find_summary(const std::string &input = "") {
  static mxi::Summary step("mxi_find", input);
  return step;
}

// Spots by resolution, from each sweep's own geometry: bins of equal width in
// 1/d^2, as dials.report draws them -- shells of equal reciprocal volume put
// nearly every strong spot in the first two (Graeme's 4 x 3600 images). This
// sweep's in ten, printed; every sweep's so far in fifty, the summary's.
// And the ice rings, where the spots crowd one -- three times the share a
// uniform spread puts in its band, and twenty spots or more.
struct Shells {
  std::vector<double> d_max, d_min;
  std::vector<std::size_t> count;
};
Shells shells_of(const std::vector<double> &dstar, std::size_t bins) {
  Shells out;
  double top = 0.0;
  for (double x : dstar)
    top = std::max(top, x * x);
  if (!(top > 0.0))
    return out;
  out.count.assign(bins, 0);
  for (double x : dstar)
    ++out.count[std::min<std::size_t>(
        bins - 1,
        static_cast<std::size_t>(x * x / top * static_cast<double>(bins)))];
  for (std::size_t k = 0; k < bins; ++k) {
    const double lo = top * static_cast<double>(k) / static_cast<double>(bins),
                 hi = top * static_cast<double>(k + 1) /
                      static_cast<double>(bins);
    out.d_max.push_back(lo > 0.0 ? 1.0 / std::sqrt(lo) : 0.0);
    out.d_min.push_back(1.0 / std::sqrt(hi));
  }
  return out;
}

void report_resolution(const std::vector<double> &dstar) {
  if (dstar.size() < 10)
    return;
  static std::vector<double> every; // every sweep's, so far
  every.insert(every.end(), dstar.begin(), dstar.end());
  const Shells mine = shells_of(dstar, 10);
  mxi::print("Spots by resolution\n  %8s %8s %8s\n", "d max", "d min", "spots");
  for (std::size_t k = 0; k < mine.count.size(); ++k) {
    if (mine.d_max[k] > 0.0)
      mxi::print("  %8.2f %8.2f %8zu\n", mine.d_max[k], mine.d_min[k],
                 mine.count[k]);
    else
      mxi::print("  %8s %8.2f %8zu\n", "inf", mine.d_min[k], mine.count[k]);
  }
  mxi::print("\n");
  const Shells all = shells_of(every, 50);
  std::vector<mxi::json::Value> c_max, c_min, c_n;
  for (std::size_t k = 0; k < all.count.size(); ++k) {
    c_max.push_back(mxi::json::Value(all.d_max[k]));
    c_min.push_back(mxi::json::Value(all.d_min[k]));
    c_n.push_back(mxi::json::Value(static_cast<std::int64_t>(all.count[k])));
  }
  find_summary().table("spots_by_resolution", {"d_max", "d_min", "spots"},
                       {c_max, c_min, c_n});
  const auto [lo_it, hi_it] = std::minmax_element(dstar.begin(), dstar.end());
  const double lo = *lo_it, hi = *hi_it;
  const double v_lo = lo * lo * lo, v_hi = hi * hi * hi;
  if (!(v_hi > v_lo))
    return;
  // Hexagonal ice, as dials.find_spots' powder-ring filter has them; a band
  // of 0.004 in d*^2 about each.
  static const double ice[] = {3.897, 3.669, 3.441, 2.671, 2.249,
                               2.072, 1.948, 1.918, 1.883, 1.721};
  mxi::json::Array crowded;
  std::string said;
  for (double d : ice) {
    const double centre = 1.0 / (d * d);
    const double a = std::sqrt(std::fmax(centre - 0.002, 0.0)),
                 b = std::sqrt(centre + 0.002);
    if (b < lo || a > hi)
      continue;
    std::size_t in = 0;
    for (double x : dstar)
      in += (x >= a && x <= b) ? 1 : 0;
    const double expected = static_cast<double>(dstar.size()) *
                            (b * b * b - a * a * a) / (v_hi - v_lo);
    if (in >= 20 && static_cast<double>(in) > 3.0 * expected) {
      crowded.push_back(mxi::json::Value(d));
      char text[32];
      std::snprintf(text, sizeof text, "%s%.2f", said.empty() ? "" : ", ", d);
      said += text;
    }
  }
  if (!said.empty())
    mxi::print("Ice rings: the spots crowd %s A\n", said.c_str());
  find_summary().set("ice_rings", mxi::json::Value(std::move(crowded)));
}
} // namespace
#include "image_series.hh"
#include "signal_pixel.hh"
#include "spot_table.hh"

#ifdef MXI_GPU
#include "dext_gpu.hh"
#endif

namespace {

volatile std::sig_atomic_t interrupted = 0;

void on_signal(std::int32_t /*signal*/) { interrupted = 1; }

using Clock = std::chrono::steady_clock;

struct Options {
  std::int32_t threads = 0; // every core, as the other programs; -j for fewer
  std::int32_t timeout_seconds = 60;
  // A progress bar from this many images, as integration's (progress.hh):
  // the frames are the slow part, and on a slow machine -- a Raspberry Pi --
  // a long series had run in silence (Graeme, 29 October).
  std::size_t progress_from = 100;
  std::int32_t poll_milliseconds = 200;
  std::string master;                 // the NXmx HDF5 master file
  bool master_given = false;          // named on the command line
  std::size_t sweeps = 1;             // experiments in the list
  std::string experiments;            // the experiment list, if given
  std::string output = "strong.refl"; // -o
  // The .rflx to write (docs/rflx.md), with the experiment list given if one
  // was; empty when -o named a .refl, written then as before.
  std::string rflx;
  bool output_named = false;
  bool gpu = false;
  bool gpu_force =
      false; // 32-bit frames narrowed to 16 bits, for a GPU of 16 only
  bool timing = false;
  bool shoeboxes = true;
  // Hot pixels: signal on hot_fraction of the images or more, said always,
  // written into the output's untrusted regions with --mask-hot (Graeme, 16
  // November, after his DIALS tool: a pixel signal on every image, or every
  // other one).
  double hot_fraction = 0.25;
  bool mask_hot = false;
  bool two_d = false;
  bool z_offset_given = false;
  std::int64_t z_offset = 0;
  mxi::spots::Options grouping;
  std::size_t chunk = 0; // frames collected before grouping; 0 picks one
};

// Which backend, and whether its arithmetic is the CPU's. A reflection table
// from a fast-math build is not one to compare against DIALS without knowing
// that, so it is in the one line that identifies the binary.
void report_version(const char *program) {
  mxi::print("%s %s (%s%s)\n", program, MXI_VERSION,
#ifdef MXI_GPU
             mxi::gpu::backend(),
#else
             "no GPU",
#endif
#ifdef MXI_FAST_MATH
             ", fast math"
#else
             ""
#endif
  );
}

// To standard output when it was asked for, and to standard error when it is
// the answer to a mistake: `mxi_find --help | less` showed nothing while it
// all went to standard error.
void usage(const char *program, mxi::Stream to = mxi::Stream::err) {
  mxi::print_to(
      to,
      "usage: %s IMPORTED.rflx [options]\n"
      "  -j, --threads N   threads for everything this does in parallel; 0, "
      "every\n"
      "                    core this job may use (0)\n"
      "       %s MASTER.nxs [options]\n"
      "\n"
      "Strong spots on every image of every sweep: the input an experiment\n"
      "list, as mxi import writes it (or a DIALS .expt), naming where each\n"
      "sweep's images are; or an NXmx master file directly.\n"
      "\n"
      "  -x MASTER.nxs      the images, where the experiment list's are moved\n"
      "  -o file            where to write: a .rflx, with the experiment list\n"
      "                     if one was given (strong.rflx), or a .refl, the\n"
      "                     reflection table alone\n"
      "  --timing           where the time goes: each stage's time summed "
      "across\n"
      "                     the threads, against the time they had\n"
      "  --verbose          the run's particulars as well: the images, the\n"
      "                     series, threads, mask and grouping\n"
      "  -g, --gpu          run the threshold on the GPU; 16-bit only under\n"
      "                     Metal, which has no double precision\n"
      "  --gpu-force        32-bit frames narrowed to 16 bits, so that a GPU "
      "of\n"
      "                     16 bits only takes them: the bad-pixel and\n"
      "                     the 16-bit one, every other count as it is, "
      "stopping "
      "at\n"
      "                     one of 0xFFFD or more. mxi max says whether a "
      "series\n"
      "                     fits. The same spots as the 32-bit threshold\n"
      "  --no-shoeboxes     leave out the pixel data, which is most of the\n"
      "                     file and is not needed for indexing\n"
      "  --hot-fraction F   a pixel signal on this share of the images or "
      "more\n"
      "                     is hot, and said (0.25)\n"
      "  --mask-hot         and written to the output's untrusted regions, so\n"
      "                     that integration leaves it out\n"
      "  --min-spot-size N  contiguous pixels a spot needs (3)\n"
      "  --max-spot-size N  and the most it may have (1000)\n"
      "  --max-separation D peak to centroid, in pixels; 0 turns it off (2)\n"
      "  --subtract-background\n"
      "                     centroids weighted by count less the threshold's "
      "local\n"
      "                     background, the intensity less it, and the "
      "background\n"
      "                     kept in the shoeboxes, where the integrator's "
      "profile\n"
      "                     model uses it; off, as DIALS has it\n"
      "  --2d               group each frame on its own, as for stills\n"
      "  --z-offset N       array index of image number 0; taken from the\n"
      "                     experiment list when one is given, and 0 "
      "otherwise\n"
      "  --chunk N          frames held between groupings (a few per thread)\n"
      "  --progress-from N  a progress bar from N images (100); a line each\n"
      "                    tenth where the output is not a terminal\n"
      "  -t timeout         seconds to wait for the file to appear (default "
      "60)\n"
      "  -p poll-ms         interval between checks while waiting (default "
      "200)\n"
      "  --version          what this binary is, and what it was built with\n",
      program, program);
}

//: Whether a file is JSON, as an experiment list is: its first character that
//: is not white space an opening brace. A file that cannot be read is not.
//: A 32-bit frame as 16 bits, for --gpu-force. The detector's markers -- the
//: bad pixel 0xfffffffe and the tile join 0xffffffff -- keep their low half,
//: which is the 16-bit markers 0xfffe and 0xffff, and every count is copied as
//: it is: so the low 16 bits are the answer for every pixel. One pass with no
//: branch, which the compiler makes SIMD; false, with the count, if a valid
//: pixel holds 0xFFFD or more, which 16 bits cannot carry apart from the
//: markers.
bool narrow_to_16(const std::uint8_t *in, std::size_t n,
                  std::uint8_t *out_bytes, std::uint32_t *offending) {
  const std::uint32_t *pixels = reinterpret_cast<const std::uint32_t *>(in);
  std::uint16_t *out = reinterpret_cast<std::uint16_t *>(out_bytes);
  std::uint32_t worst = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t v = pixels[i];
    const bool marker = v >= 0xFFFFFFFEu;
    worst = std::max(worst, marker ? 0u : v);
    out[i] = static_cast<std::uint16_t>(v);
  }
  *offending = worst;
  return worst < 0xFFFDu;
}

bool looks_like_json(const std::string &path) {
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return false;
  std::int32_t c = 0;
  while ((c = std::fgetc(file)) != EOF && std::isspace(c))
    ;
  std::fclose(file);
  return c == '{';
}

bool parse_options(int argc, char **argv, Options *options) {
  // --version first: mxi_find's says which threshold backend it was built with,
  // more than the shared parser's.
  for (std::int32_t i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--version") {
      report_version("mxi find");
      std::exit(0);
    }
  // As every program: the shared parser (args.hh), -j and --threads alike.
  const std::set<std::string> known = {"-j",
                                       "--threads",
                                       "-t",
                                       "-p",
                                       "-x",
                                       "-o",
                                       "-g",
                                       "--gpu",
                                       "--gpu-force",
                                       "--subtract-background",
                                       "--timing",
                                       "--verbose",
                                       "--no-shoeboxes",
                                       "--hot-fraction",
                                       "--mask-hot",
                                       "--2d",
                                       "--min-spot-size",
                                       "--max-spot-size",
                                       "--max-separation",
                                       "--z-offset",
                                       "--chunk",
                                       "--progress-from",
                                       "-h",
                                       "--help"};
  const std::set<std::string> takes_value = {"-j",
                                             "--threads",
                                             "-t",
                                             "-p",
                                             "-x",
                                             "-o",
                                             "--min-spot-size",
                                             "--max-spot-size",
                                             "--max-separation",
                                             "--z-offset",
                                             "--chunk",
                                             "--hot-fraction",
                                             "--progress-from"};
  mxi::Arguments args = mxi::parse_arguments(argc, argv, known, takes_value);
  if (args.help) {
    usage(argv[0], mxi::Stream::out);
    std::exit(0);
  }
  if (!args.ok) {
    mxi::eprint("mxi find: %s\n", args.error.c_str());
    return false;
  }
  // -j/--threads, as every command takes it (args.cc), or every core this
  // job may use (parallel.hh): read once the parser has set it.
  options->threads = static_cast<std::int32_t>(mxi::parallel_threads());
  options->timeout_seconds =
      static_cast<std::int32_t>(args.number("-t", options->timeout_seconds));
  options->progress_from = static_cast<std::size_t>(args.number(
      "--progress-from", static_cast<double>(options->progress_from)));
  options->poll_milliseconds =
      static_cast<std::int32_t>(args.number("-p", options->poll_milliseconds));
  if (args.has("-x"))
    options->master = args.value("-x", "");
  if (args.has("-o")) {
    options->output = args.value("-o", "");
    options->output_named = true;
  }
  options->gpu = args.has("--gpu") || args.has("-g");
  options->gpu_force = args.has("--gpu-force");
  options->grouping.subtract_background = args.has("--subtract-background");
  options->timing = args.has("--timing");
  if (args.has("--verbose"))
    mxi::set_verbose(true);
  options->shoeboxes = !args.has("--no-shoeboxes");
  options->mask_hot = args.has("--mask-hot");
  if (args.has("--hot-fraction")) {
    options->hot_fraction = std::atof(args.value("--hot-fraction", "").c_str());
    if (!(options->hot_fraction > 0.0 && options->hot_fraction <= 1.0)) {
      mxi::eprint("--hot-fraction is a share of the images, above 0 and at "
                  "most 1\n");
      std::exit(2);
    }
  }
  options->two_d = args.has("--2d");
  if (args.has("--min-spot-size"))
    options->grouping.min_spot_size = static_cast<std::size_t>(
        std::atoll(args.value("--min-spot-size", "").c_str()));
  if (args.has("--max-spot-size"))
    options->grouping.max_spot_size = static_cast<std::size_t>(
        std::atoll(args.value("--max-spot-size", "").c_str()));
  if (args.has("--max-separation"))
    options->grouping.max_separation =
        std::atof(args.value("--max-separation", "").c_str());
  if (args.has("--z-offset")) {
    options->z_offset = std::atoll(args.value("--z-offset", "").c_str());
    options->z_offset_given = true;
  }
  if (args.has("--chunk"))
    options->chunk =
        static_cast<std::size_t>(std::atoll(args.value("--chunk", "").c_str()));
  // The inputs: an experiment list (JSON, or a .rflx, as mxi_import writes
  // it), as every program takes it; or an NXmx master given directly; or both,
  // the master then overriding where the list says the images are.
  for (const std::string &input : args.positional) {
    if (looks_like_json(input) || mxi::rflx::has_experiments(input)) {
      if (!options->experiments.empty()) {
        mxi::eprint("%s: two experiment lists, %s and %s\n", argv[0],
                    options->experiments.c_str(), input.c_str());
        return false;
      }
      options->experiments = input;
    } else if (options->master.empty()) {
      options->master = input;
    } else {
      usage(argv[0]);
      return false;
    }
  }
  options->master_given = !options->master.empty();
  if (!options->experiments.empty()) {
    mxi::ExperimentImages info;
    try {
      info = mxi::experiment_images(options->experiments);
    } catch (const std::exception &error) {
      mxi::eprint("%s\n", error.what());
      return false;
    }
    options->sweeps = std::max<std::size_t>(info.experiments, 1);
    if (!options->master.empty() || info.experiments > 1) {
    } else if (!info.has_imageset) {
      mxi::eprint("%s has no imageset, so it does not say where the images "
                  "are; name the master file, or pass -x\n",
                  options->experiments.c_str());
      return false;
    } else if (info.templated) {
      mxi::eprint("%s names a file template, '%s', which is a numbered "
                  "sequence rather than one NXmx file; this reads NXmx only\n",
                  options->experiments.c_str(), info.image_file.c_str());
      return false;
    } else {
      options->master = info.image_file;
      mxi::detail("Images: %s, from %s\n", options->master.c_str(),
                  options->experiments.c_str());
    }
  }
  if (options->master.empty() && options->sweeps <= 1) {
    usage(argv[0]);
    return false;
  }
  options->grouping.two_d = options->two_d;
  if (options->chunk == 0)
    options->chunk = static_cast<std::size_t>(options->threads) * 4 + 4;
  return true;
}

void sleep_for(std::int32_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// ---------------------------------------------------------------------------
// The frame buffer, and the threshold.
//
// The frame buffer is allocated through mxi::gpu::host_alloc when a device is
// in use, which on Metal is a shared MTLBuffer: the frame is decompressed
// straight into memory the kernels read and there is no copy in either
// direction.
// ---------------------------------------------------------------------------

#ifdef MXI_GPU
// Only meaningful in a build that has a device to be on; without one the
// threshold has a single path and this would be an unused variable.
bool on_device = false;
#endif

class FrameBuffer {
public:
  ~FrameBuffer() { release(); }
  FrameBuffer() = default;
  FrameBuffer(const FrameBuffer &) = delete;
  FrameBuffer &operator=(const FrameBuffer &) = delete;

  std::uint8_t *get(std::size_t bytes) {
    if (bytes > capacity_) {
      release();
#ifdef MXI_GPU
      // Shared with the device on Metal, so the frame is decompressed straight
      // into memory the kernels read and there is no copy at all.
      if (on_device)
        data_ = static_cast<std::uint8_t *>(mxi::gpu::host_alloc(bytes));
      if (data_ != nullptr)
        pinned_ = true;
#endif
      if (data_ == nullptr)
        data_ = new std::uint8_t[bytes];
      capacity_ = bytes;
    }
    return data_;
  }

private:
  void release() {
    if (data_ == nullptr)
      return;
    // Spelled out for both builds. With the GPU compiled out, the dangling
    // 'else' used here left a bare delete indented as though it belonged to
    // the early return above it -- correct, and exactly the shape that gets
    // edited wrongly by the next person to read it.
#ifdef MXI_GPU
    if (pinned_) {
      mxi::gpu::host_free(data_);
    } else {
      delete[] data_;
    }
#else
    delete[] data_;
#endif
    data_ = nullptr;
    pinned_ = false;
    capacity_ = 0;
  }

  std::uint8_t *data_ = nullptr;
  std::size_t capacity_ = 0;
  bool pinned_ = false;
};

template <typename T>
void threshold(const std::uint8_t *frame, std::vector<SignalPixel> *signal,
               std::size_t height, std::size_t width) {
  const T *const pixels = reinterpret_cast<const T *>(frame);
  std::int32_t status = 0;
#ifdef MXI_GPU
  if (on_device) {
    status = mxi::gpu::find<T>(pixels, *signal, height, width);
  } else
#endif
  {
    static thread_local dext_scratch<T> scratch;
    status = dext<T>(pixels, *signal, height, width, scratch);
  }
  if (status == -2) {
    throw std::runtime_error(
        "more signal pixels than the device buffer holds; a frame that dense "
        "is not a frame of spots");
  }
  if (status != 0)
    throw std::runtime_error("the threshold rejected the frame");
}

// ---------------------------------------------------------------------------
// One chunk of frames in flight: the workers fill it and the main thread
// groups it once every frame in it is accounted for.
// ---------------------------------------------------------------------------

struct Found {
  std::int64_t number = 0;
  std::vector<SignalPixel> pixels;
};

class Chunk {
public:
  void expect(std::size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    outstanding_ += frames;
  }

  // Called once per dispatched key, whatever became of it.
  void done(Found *found) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (found != nullptr)
        frames_.push_back(std::move(*found));
      outstanding_--;
    }
    empty_.notify_all();
  }

  // Set when the last worker has gone. Without it, a chunk dispatched to
  // threads that all failed to open the series would be waited on forever.
  void abandon() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      abandoned_ = true;
    }
    empty_.notify_all();
  }

  bool abandoned() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return abandoned_ && outstanding_ > 0;
  }

  // Frames of this chunk, in ascending order, once they are all in.
  std::vector<Found> collect() {
    std::unique_lock<std::mutex> lock(mutex_);
    empty_.wait(lock, [this] { return outstanding_ == 0 || abandoned_; });
    std::vector<Found> frames = std::move(frames_);
    frames_.clear();
    std::sort(frames.begin(), frames.end(), [](const Found &a, const Found &b) {
      return a.number < b.number;
    });
    return frames;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable empty_;
  std::size_t outstanding_ = 0;
  bool abandoned_ = false;
  std::vector<Found> frames_;
};

mxi::series::Info open_series(mxi::series::Series *source,
                              const Options &options) {
  const Clock::time_point deadline =
      Clock::now() + std::chrono::seconds(options.timeout_seconds);
  bool announced = false;
  while (interrupted == 0) {
    mxi::series::Info info;
    if (source->try_open(&info))
      return info;
    if (Clock::now() > deadline)
      throw std::runtime_error("timed out waiting for " + source->describe());
    if (!announced) {
      mxi::print("Waiting for %s\n", source->describe().c_str());
      announced = true;
    }
    sleep_for(options.poll_milliseconds);
  }
  throw std::runtime_error("interrupted while waiting for a series");
}

// What the experiment list says, against what the series says. A mismatch here
// is worth stopping for: spot finding would succeed and index nothing, and the
// reason would not be visible in either file.
void reconcile(const mxi::ExperimentImages &experiments,
               const mxi::series::Info &series, Options *options) {
  mxi::detail("Experiments: %s\n", describe(experiments).c_str());

  // One experiment's, always: several sweeps are taken one at a time.
  if (experiments.panels > 1) {
    throw std::runtime_error(
        "the detector in " + options->experiments + " has " +
        std::to_string(experiments.panels) +
        " panels, and a frame is treated here as one panel; a segmented "
        "detector needs a panel number per spot, which this does not do");
  }
  if (experiments.image_slow > 0 && (experiments.image_slow != series.height ||
                                     experiments.image_fast != series.width)) {
    // The panel the frames' size turned round is the file's metadata with its
    // x and y pixel counts swapped, the data the right shape: mxi_import now
    // takes the data's, so importing again mends it.
    const bool swapped = experiments.image_fast == series.height &&
                         experiments.image_slow == series.width;
    throw std::runtime_error(
        "the panel in " + options->experiments + " is " +
        std::to_string(experiments.image_fast) + " x " +
        std::to_string(experiments.image_slow) + " and the frames are " +
        std::to_string(series.width) + " x " + std::to_string(series.height) +
        " (fast x slow); " +
        (swapped ? "the one is the other turned round -- the file's own "
                   "detector size has its x and y swapped, the data the "
                   "right shape. mxi import takes the data's now: import "
                   "again"
                 : std::string("these are not the same images")));
  }
  // A scan covering fewer images than the file is not a mismatch to warn
  // about: the frames outside it are simply not read. It used to warn that z
  // would be wrong outside the scan and then read them all anyway, which was
  // the warning describing the bug rather than preventing it. A scan claiming
  // MORE images than the file has is still an error, since there is nothing
  // to read.
  if (experiments.has_scan && series.images > 0 &&
      static_cast<std::uint64_t>(experiments.images()) > series.images) {
    throw std::runtime_error(
        options->experiments + " covers " +
        std::to_string(experiments.images()) + " images and the file has " +
        std::to_string(series.images) + "; the scan reaches past the data");
  }

  // The imageset and the scan can disagree about how many images there are --
  // one sliced and the other not, or an .expt assembled by hand. The scan sets
  // z, so a mismatch means z is measured against a range the images do not
  // cover.
  if (!experiments.imageset_matches_scan()) {
    mxi::eprint("warning: %s lists %" PRIu64
                " frames in its imageset and %" PRId64 " in "
                "its scan; z follows the scan\n",
                options->experiments.c_str(),
                static_cast<std::uint64_t>(experiments.frames),
                static_cast<std::int64_t>(experiments.images()));
  }
  // Gaps in single_file_indices mean the imageset is not the whole file in
  // order, which nothing here allows for: frames are read as a contiguous run,
  // so a gap would silently shift every spot after it onto the wrong image.
  if (experiments.has_imageset && !experiments.contiguous_indices) {
    throw std::runtime_error(
        "the imageset in " + options->experiments +
        " skips frames, and this reads a contiguous run; z would be wrong "
        "from the first gap onwards");
  }

  // z is measured in scan array indices, where image n is n - 1. A frame
  // arrives numbered by its index IN THE FILE, and the .expt says which file
  // index each scan image is: single_file_indices. So the offset is the
  // difference between the two, which is nothing when they line up.
  //
  // It was first_image - 1 unconditionally, which assumed a sliced import's
  // frames are numbered from zero -- true of a file holding only the slice,
  // false of a master file for the whole run, where image 6 already arrives
  // as frame 5. Reading only the scan's frames made that visible: a scan of
  // images 6 to 10 put its spots at z of 10.5 to 14.5 instead of 5.5 to 9.5.
  //
  // Without single_file_indices the file index of image n is taken to be
  // n - 1, the same assumption the frame restriction makes, so that the frames
  // read and the z they are given cannot disagree.
  if (!options->z_offset_given && experiments.has_scan) {
    const std::int64_t first_index = experiments.frames > 0
                                         ? experiments.first_index
                                         : experiments.first_image - 1;
    options->z_offset = (experiments.first_image - 1) - first_index;
    if (options->z_offset != 0) {
      mxi::detail("The scan starts at image %" PRId64
                  ", so z starts at %" PRId64 "\n",
                  static_cast<std::int64_t>(experiments.first_image),
                  static_cast<std::int64_t>(options->z_offset));
    }
  }
}

} // namespace

// The table written, through the library's writers: to its .refl, or into a
// .rflx (docs/rflx.md) with the experiment list given, if one was. Throws as
// the writers do.
//: Each sweep's hot pixels, by its index, row-major: found by find_one, for
//: write_table to add to the sweep's image set's untrusted regions.
std::map<std::size_t, std::vector<std::uint32_t>> &hot_pixels() {
  static std::map<std::size_t, std::vector<std::uint32_t>> hot;
  return hot;
}

//: Each sweep's hot pixels in the step's summary, a sweep an entry: images,
//: the share, and [slow, fast, images] each, most first -- or judged false.
void record_hot(std::uint64_t frames, double fraction, mxi::json::Array pixels,
                bool judged = true) {
  static mxi::json::Array per_sweep;
  mxi::json::Object one;
  one["images"] = mxi::json::Value(static_cast<std::int64_t>(frames));
  one["fraction"] = mxi::json::Value(fraction);
  one["judged"] = mxi::json::Value(judged);
  one["pixels"] = mxi::json::Value(std::move(pixels));
  per_sweep.push_back(mxi::json::Value(std::move(one)));
  find_summary().set("hot_pixels", mxi::json::Value(per_sweep));
}

//: The hot pixels, a line each: slow, fast, the share of the images.
void print_hot_table(const std::vector<std::uint32_t> &order,
                     const std::vector<std::uint32_t> &signal_on,
                     std::uint64_t frames, std::uint64_t width) {
  mxi::print("     slow    fast  images\n");
  for (std::uint32_t i : order)
    mxi::print("  %7" PRIu64 " %7" PRIu64 "  %5.1f%%\n",
               static_cast<std::uint64_t>(i) / width,
               static_cast<std::uint64_t>(i) % width,
               100.0 * signal_on[i] / static_cast<double>(frames));
}

//: Pixels signal on `fraction` of the `frames` or more, said, and kept for
//: the sweep. A pixel on every image is a column through them all, one long
//: spot that the largest size removes; one every other image, single pixels
//: that the smallest does -- spot finding mostly survives them, but in a
//: reflection's box integration takes them at their word.
void judge_hot(const std::vector<std::uint32_t> &signal_on,
               std::uint64_t frames, std::uint64_t width, double fraction,
               bool mask, std::size_t index) {
  hot_pixels()[index].clear();
  // A reflection is signal on its rocking curve's images, so a pixel needs a
  // share of a long series to be told from one: of 30 images, a spot 3 wide
  // is 10% of them. From 100, 25% is 25 images -- slicing so fine that one
  // reflection spans that many would mark it hot too, and is not judged here.
  if (frames < 100) {
    mxi::print("Hot pixels: not judged, from %" PRIu64
               " images (100 at least)\n",
               frames);
    record_hot(frames, fraction, {}, false);
    return;
  }
  const auto cut = static_cast<std::uint32_t>(
      std::ceil(fraction * static_cast<double>(frames) - 1e-9));
  std::vector<std::uint32_t> hot;
  for (std::size_t i = 0; i < signal_on.size(); ++i)
    if (signal_on[i] >= std::max<std::uint32_t>(cut, 2))
      hot.push_back(static_cast<std::uint32_t>(i));
  const double percent = 100.0 * fraction;
  // One line, as Graeme put it (16 November): how many, the share, and what
  // to do about them; the breakdown and the worst, with --verbose.
  if (hot.empty()) {
    mxi::print("Hot pixels: none on >= %.0f%% of the %" PRIu64 " images\n",
               percent, frames);
    record_hot(frames, fraction, {});
    return;
  }
  mxi::print("Hot pixels: %zu on >= %.0f%% of the %" PRIu64
             " images (--hot-fraction)%s\n",
             hot.size(), percent, frames,
             mask ? ": masked" : ": --mask-hot to exclude and mask them");
  // Each, slow then fast as the detector's own mask counts them (m[slow,
  // fast]), with its images, most first: in the summary always, for mxi
  // untrusted --hot and mxi report; on the terminal with --verbose.
  std::vector<std::uint32_t> order = hot;
  std::stable_sort(order.begin(), order.end(),
                   [&](std::uint32_t a, std::uint32_t b) {
                     return signal_on[a] > signal_on[b];
                   });
  mxi::json::Array listed;
  for (std::uint32_t i : order)
    listed.push_back(mxi::json::Value(mxi::json::Array{
        mxi::json::Value(static_cast<std::int64_t>(i / width)),
        mxi::json::Value(static_cast<std::int64_t>(i % width)),
        mxi::json::Value(static_cast<std::int64_t>(signal_on[i]))}));
  record_hot(frames, fraction, std::move(listed));
  if (mxi::verbose())
    print_hot_table(order, signal_on, frames, width);
  hot_pixels()[index] = std::move(hot);
}

void write_table(const Options &options,
                 const std::vector<mxi::SpotPart> &parts, bool shoeboxes) {
  const mxi::Table table = mxi::spot_table(parts, 0, shoeboxes);
  if (options.rflx.empty()) {
    mxi::write_reflections(options.output, table);
    return;
  }
  std::optional<mxi::json::Value> document;
  if (!options.experiments.empty())
    document = mxi::read_experiment_document(options.experiments);
  if (options.mask_hot && document) {
    // Each sweep's hot pixels into its own image set's untrusted regions,
    // beside any already there, as a list of single pixels (untrusted.hh).
    mxi::json::Value &sets = document->as_object()["imageset"];
    const mxi::json::Value &experiments = (*document)["experiment"];
    for (const mxi::SpotPart &part : parts) {
      const auto found = hot_pixels().find(static_cast<std::size_t>(part.id));
      if (found == hot_pixels().end() || found->second.empty() ||
          !sets.is_array())
        continue;
      std::size_t at = 0;
      if (experiments.is_array() &&
          static_cast<std::size_t>(part.id) < experiments.as_array().size() &&
          experiments.as_array()[part.id]["imageset"].is_number())
        at = static_cast<std::size_t>(
            experiments.as_array()[part.id]["imageset"].as_number());
      if (at >= sets.as_array().size() || !sets.as_array()[at].is_object())
        continue;
      mxi::json::Array pixels;
      for (std::uint32_t i : found->second)
        pixels.push_back(mxi::json::Value(mxi::json::Array{
            mxi::json::Value(static_cast<double>(i % part.width)),
            mxi::json::Value(static_cast<double>(i / part.width))}));
      mxi::json::Object region;
      region["panel"] = mxi::json::Value(0.0);
      region["pixels"] = mxi::json::Value(std::move(pixels));
      mxi::json::Object &set = sets.as_array()[at].as_object();
      if (!set["untrusted"].is_array())
        set["untrusted"] = mxi::json::Value(mxi::json::Array{});
      set["untrusted"].as_array().push_back(
          mxi::json::Value(std::move(region)));
      mxi::print("Hot pixels masked: %zu, in image set %zu's untrusted "
                 "regions\n",
                 found->second.size(), at);
    }
  }
  const mxi::json::Value summary = find_summary().document();
  mxi::rflx::write(options.rflx, document ? &*document : nullptr, &table,
                   "mxi_find", false, &summary);
}

// What was written, or that nothing was.
void report_written(const Options &options, std::size_t rows) {
  const std::string &written =
      options.rflx.empty() ? options.output : options.rflx;
  if (rows == 0) {
    mxi::eprint("No reflections found; %s is a well formed table with no rows "
                "in it, which dials.index will refuse\n",
                written.c_str());
    return;
  }
  std::error_code ignored;
  const std::uintmax_t bytes = std::filesystem::file_size(written, ignored);
  mxi::print("Wrote %zu reflections to %s (%.1f MB%s)\n", rows, written.c_str(),
             static_cast<double>(bytes) / 1e6,
             options.shoeboxes ? ", most of it shoeboxes" : "");
}

// What becomes of one experiment's spots: written as the table, for one sweep,
// or kept to be written with the others', for several.
using Emit = std::function<std::int32_t(const mxi::spots::Labeller &,
                                        const mxi::series::Info &,
                                        const mxi::ExperimentImages &)>;

// One experiment's spots, from its own images: experiment `index` of the list,
// or the master file alone without one. Handed to `emit` where they would be
// written, then the timing reported.
std::int32_t find_one(Options options, std::size_t index, mxi::Timing &timing,
                      const Emit &emit) {
  std::unique_ptr<mxi::series::Series> source;
  mxi::series::Info info;
  // How many frames will be offered, when fewer than the file holds.
  std::uint64_t restricted = 0;
  mxi::ExperimentImages experiments;
  try {
    if (!options.experiments.empty())
      experiments = mxi::experiment_images(options.experiments, index);

    source = mxi::series::nxmx(options.master);

    // Before the series is opened, so that asking for a device that is not
    // there fails at once rather than after a wait.
    if (options.gpu) {
#ifdef MXI_GPU
      if (!mxi::gpu::available()) {
        throw std::runtime_error(std::string("--gpu was given but no usable ") +
                                 mxi::gpu::backend() + " device was found: " +
                                 mxi::gpu::unavailable_reason());
      }
      on_device = true;
#else
      throw std::runtime_error(
          "--gpu was given but this build has no GPU support; configure with "
          "-DMXI_METAL=ON or -DMXI_CUDA=ON");
#endif
    }

    info = open_series(source.get(), options);
    if (!options.experiments.empty())
      reconcile(experiments, info, &options);
    // The image set's untrusted regions (mxi_view), joined to the detector's
    // own mask before any frame is read.
    if (experiments.untrusted.is_array() &&
        !experiments.untrusted.as_array().empty()) {
      const std::vector<std::uint64_t> pixels =
          mxi::untrusted_pixels(experiments.untrusted, info.width, info.height);
      source->add_masked(pixels);
      mxi::print("Untrusted regions: %s, %zu pixels\n",
                 mxi::describe_untrusted(experiments.untrusted).c_str(),
                 pixels.size());
    }

    // Only the frames the scan covers. The array indices come from the
    // imageset's single_file_indices when it has them, which is what they are;
    // otherwise from image_range, whose image n is array index n - 1.
    //
    // Found by spot finding a scan of 1800 images of a 36000 image file, which
    // warned that z would be wrong outside the scan and then processed all
    // 36000 -- twenty times the work, and 1115233 spots of which a
    // twentieth belonged to the experiment it was asked about.
    if (experiments.has_scan) {
      std::uint64_t first = 0, last = 0;
      if (experiments.frames > 0) {
        first = static_cast<std::uint64_t>(experiments.first_index);
        last = static_cast<std::uint64_t>(experiments.last_index);
      } else {
        first = static_cast<std::uint64_t>(
            std::max<std::int64_t>(experiments.first_image - 1, 0));
        last = static_cast<std::uint64_t>(
            std::max<std::int64_t>(experiments.last_image - 1, 0));
      }
      if (!source->restrict_frames(first, last)) {
        throw std::runtime_error(
            "this series cannot be limited to the frames the .expt's scan "
            "covers, and finding spots on the rest would put them at z "
            "values the experiment does not have");
      }
      // What "done" means now: the scan's frames, not the file's. Left at the
      // file's count, the loop would wait for frames it has been told not to
      // offer until the timeout ended it.
      restricted = last - first + 1;
    }
  } catch (const std::exception &error) {
    mxi::eprint("%s\n", error.what());
    return 1;
  }

  if (restricted > 0 && restricted != info.images) {
    mxi::print("Reading %" PRIu64 " of the %" PRIu64
               " images, the ones the scan covers\n",
               static_cast<std::uint64_t>(restricted),
               static_cast<std::uint64_t>(info.images));
  }
  mxi::print(
      "Finding spots on %" PRIu64 " images\n",
      static_cast<std::uint64_t>(restricted > 0 ? restricted : info.images));
  mxi::detail("Series %s: %" PRIu64 " images of %" PRIu64 " x %" PRIu64
              " pixels (fast x slow), from %s, "
              "%d thread%s, "
              "on the %s\n",
              info.name.c_str(), static_cast<std::uint64_t>(info.images),
              static_cast<std::uint64_t>(info.width),
              static_cast<std::uint64_t>(info.height),
              source->describe().c_str(), options.threads,
              options.threads == 1 ? "" : "s", options.gpu ? "GPU" : "CPU");
  if (info.masked_pixels > 0)
    mxi::detail("%" PRIu64 " pixels masked by the detector's own pixel_mask\n",
                static_cast<std::uint64_t>(info.masked_pixels));
  mxi::detail(
      "Grouping in %s, %zu to %zu pixels a spot, peak within %.1f of the "
      "centroid\n",
      options.two_d ? "two dimensions" : "three dimensions",
      options.grouping.min_spot_size, options.grouping.max_spot_size,
      options.grouping.max_separation);

  // How many images each pixel was signal on: hot pixels (judge_hot).
  std::vector<std::uint32_t> signal_on(static_cast<std::size_t>(info.width) *
                                       static_cast<std::size_t>(info.height));
  mxi::spots::Labeller labeller(static_cast<std::size_t>(info.height),
                                static_cast<std::size_t>(info.width),
                                options.grouping);

  mxi::work::Queue<std::string> queue(
      static_cast<std::size_t>(options.threads) * 4);
  Chunk chunk;
  std::atomic<std::uint64_t> read{0};
  std::atomic<std::uint64_t> missing{0};
  std::atomic<std::uint64_t> failed{0};
  // The first frame's error, said once at the end if no frame could be read.
  std::mutex first_error_mutex;
  std::string first_error;
  std::atomic<std::int32_t> live{options.threads};
  const Clock::time_point began = Clock::now();
  // Each stage's time summed across the threads: set against threads x wall,
  // what the others waited on. Reading is the HDF5 chunk read; the threshold
  // includes, on a GPU, the transfers to and from it.
  mxi::ThreadSeconds t_read, t_decompress, t_threshold, t_group;
  // On CUDA the threshold's own split, from events around the upload and each
  // kernel, which do not change the schedule: summed across threads, it says
  // whether the card or the queue for it is the limit. Not taken on Metal,
  // where measuring the split serialises the stages it would measure.
  mxi::ThreadSeconds t_upload, t_stage0, t_stage1, t_stage2, t_fused,
      t_readback, t_sort;
#ifdef MXI_GPU
  const bool split = options.timing && options.gpu &&
                     std::string(mxi::gpu::backend()) == "CUDA";
  if (split)
    mxi::gpu::profile_stages(true);
#else
  const bool split = false;
#endif
  const double t_began = mxi::Timing::now();

  std::vector<std::thread> workers;
  for (std::int32_t i = 0; i < options.threads; i++) {
    workers.emplace_back([&, i] {
      // However this thread leaves, the last one out says so: a chunk in
      // flight is otherwise waited on forever.
      struct Leaving {
        std::atomic<std::int32_t> &live;
        Chunk &chunk;
        ~Leaving() {
          if (live.fetch_sub(1) == 1)
            chunk.abandon();
        }
      } leaving{live, chunk};

      std::unique_ptr<mxi::series::Reader> reader;
      try {
        reader = source->reader();
      } catch (const std::exception &error) {
        // The keys this thread would have taken are still in the queue for the
        // others; only if every thread fails does the run stop.
        mxi::eprint("thread %d: %s\n", i, error.what());
        return;
      }

      FrameBuffer buffer;
      FrameBuffer narrowed; // --gpu-force: a 32-bit frame as 16 bits
      mxi::series::Frame frame;
      std::string key;
      while (queue.pop(&key)) {
        Found found;
        bool have = false;
        try {
          const double r0 = mxi::Timing::now();
          const bool got = reader->read(key, &frame);
          t_read.add(mxi::Timing::now() - r0);
          if (!got) {
            missing++;
          } else {
            const std::size_t height = static_cast<std::size_t>(frame.height);
            const std::size_t width = static_cast<std::size_t>(frame.width);
            const std::size_t bytes =
                mxi::decompress::frame_bytes(height, width, frame.bit_depth);
            std::uint8_t *const pixels = buffer.get(bytes);
            const double d0 = mxi::Timing::now();
            mxi::decompress::image(frame.data, frame.algorithm, frame.bit_depth,
                                   height, width, {pixels, bytes});
            mxi::series::apply_pixel_mask(frame, {pixels, bytes});
            const double d1 = mxi::Timing::now();
            t_decompress.add(d1 - d0);
            found.number = frame.number;
            std::uint32_t depth = frame.bit_depth;
            const std::uint8_t *thresholded = pixels;
            if (options.gpu_force && depth == 32) {
              std::uint8_t *const into = narrowed.get(height * width * 2);
              std::uint32_t offending = 0;
              if (!narrow_to_16(pixels, height * width, into, &offending))
                throw std::runtime_error(
                    "image " + std::to_string(frame.number + 1) +
                    " has a count of " + std::to_string(offending) +
                    ", too large for 16 bits: --gpu-force cannot take this "
                    "data "
                    "(mxi max says which series fit)");
              thresholded = into;
              depth = 16;
            }
            switch (depth) {
            case 16:
              threshold<std::uint16_t>(thresholded, &found.pixels, height,
                                       width);
              break;
            case 32:
              threshold<std::uint32_t>(thresholded, &found.pixels, height,
                                       width);
              break;
            default:
              throw std::runtime_error("the threshold does not support " +
                                       std::to_string(frame.bit_depth) +
                                       "-bit data");
            }
            t_threshold.add(mxi::Timing::now() - d1);
#ifdef MXI_GPU
            if (split) {
              const mxi::gpu::StageTimes st =
                  mxi::gpu::last_stage_times(); // milliseconds
              t_upload.add(st.upload * 1e-3);
              t_stage0.add(st.stage0 * 1e-3);
              t_stage1.add(st.stage1 * 1e-3);
              t_stage2.add(st.stage2 * 1e-3);
              t_fused.add(st.fused * 1e-3);
              t_readback.add(st.copy * 1e-3);
              t_sort.add(st.sort * 1e-3);
            }
#endif
            have = true;
            read++;
          }
        } catch (const std::exception &error) {
          // The first few said; past that, a count. A series that cannot be
          // read fails the same way on every frame, and six hundred lines of
          // it hid the one that said why.
          const std::uint64_t n = failed++;
          have = false;
          if (n < 3)
            mxi::eprint("%s: %s\n", key.c_str(), error.what());
          else if (n == 3)
            mxi::eprint("(and more frames like them; counted below)\n");
          const std::lock_guard<std::mutex> lock(first_error_mutex);
          if (first_error.empty())
            first_error = error.what();
        }
        chunk.done(have ? &found : nullptr);
      }
    });
  }

  // Dispatch in chunks, group each chunk once it is complete. Keys are zero
  // padded decimal, so sorting them as text sorts them as numbers.
  std::set<std::string> dispatched;
  std::vector<std::string> waiting;
  Clock::time_point progressed = Clock::now();
  std::int64_t highest = 0;
  bool any = false;
  std::uint64_t grouped = 0;
  std::uint64_t out_of_order = 0;
  bool stop = false;

  // Progress through the series, where its length is known (a file, not a
  // stream): frames counted as their chunk finishes.
  const std::uint64_t frames_total = restricted > 0 ? restricted : info.images;
  std::uint64_t frames_done = 0;
  std::optional<mxi::Progress> progress;
  if (frames_total > 0 && frames_total >= options.progress_from)
    progress.emplace("finding spots", static_cast<double>(frames_total),
                     "images");

  const auto run = [&](std::size_t frames) {
    chunk.expect(frames);
    for (std::size_t i = 0; i < frames; i++) {
      if (!queue.push(waiting[i])) {
        // The queue only closes at the end, but if it ever did, the chunk
        // still has to be balanced.
        chunk.done(nullptr);
      }
    }
    waiting.erase(waiting.begin(),
                  waiting.begin() + static_cast<std::ptrdiff_t>(frames));

    const std::vector<Found> collected = chunk.collect();
    frames_done += frames;
    if (progress)
      progress->update(static_cast<double>(frames_done));
    if (chunk.abandoned()) {
      mxi::eprint("every thread has gone, so the rest of the series cannot be "
                  "read; stopping with what was grouped so far\n");
      failed++;
      stop = true;
    }
    for (const Found &found : collected) {
      const std::int64_t z = found.number + options.z_offset;
      if (any && z <= highest) {
        // Only reachable against a live source that published an image out of
        // order. Dropping it is better than throwing away the whole run, and
        // saying so is better than a quiet gap.
        out_of_order++;
        continue;
      }
      for (const SignalPixel &p : found.pixels)
        if (p.index < signal_on.size())
          ++signal_on[p.index];
      try {
        const double g0 = mxi::Timing::now();
        labeller.add(z, found.pixels);
        t_group.add(mxi::Timing::now() - g0);
      } catch (const std::exception &error) {
        mxi::eprint("grouping frame %" PRId64 ": %s\n",
                    static_cast<std::int64_t>(found.number), error.what());
        failed++;
        continue;
      }
      highest = z;
      any = true;
      grouped++;
    }
  };

  while (!stop) {
    std::size_t added = 0;
    try {
      for (const std::string &key : source->ready()) {
        if (!dispatched.insert(key).second)
          continue;
        waiting.push_back(key);
        added++;
      }
    } catch (const std::exception &error) {
      mxi::eprint("%s\n", error.what());
    }
    if (added > 0) {
      progressed = Clock::now();
      std::sort(waiting.begin(), waiting.end());
    }

    const bool complete = (restricted > 0 && dispatched.size() >= restricted) ||
                          (restricted == 0 && info.images > 0 &&
                           dispatched.size() >= info.images) ||
                          (source->finished() && added == 0);
    const bool timed_out = Clock::now() - progressed >
                           std::chrono::seconds(options.timeout_seconds);
    if (interrupted != 0 || complete || timed_out)
      stop = true;
    if (timed_out && !complete) {
      mxi::eprint("No new images for %d seconds, giving up\n",
                  options.timeout_seconds);
    }

    // Whole chunks while more may arrive; whatever is left once nothing can.
    while (waiting.size() >= options.chunk)
      run(options.chunk);
    if (stop && !waiting.empty())
      run(waiting.size());

    if (!stop)
      sleep_for(options.poll_milliseconds);
  }

  if (progress)
    progress->finish();
  queue.close();
  for (std::thread &worker : workers)
    worker.join();
  const double t_streamed = mxi::Timing::now();

  labeller.finish();
  const double t_finished = mxi::Timing::now();

  const double seconds =
      std::chrono::duration<double>(Clock::now() - began).count();
  const mxi::spots::Counts &counts = labeller.counts();

  mxi::detail("Thresholded %" PRIu64 " of %" PRIu64
              " images in %.1f s (%.1f images/s), "
              "%" PRIu64 " never written, %" PRIu64 " failures\n",
              static_cast<std::uint64_t>(read.load()),
              static_cast<std::uint64_t>(dispatched.size()), seconds,
              seconds > 0 ? read.load() / seconds : 0.0,
              static_cast<std::uint64_t>(missing.load()),
              static_cast<std::uint64_t>(failed.load()));
  // Images missing are data missing: said always, not under --verbose.
  if (!mxi::verbose() && (missing.load() > 0 || failed.load() > 0))
    mxi::print("Read %" PRIu64 " of %" PRIu64 " images: %" PRIu64
               " never written, %" PRIu64 " "
               "failures\n",
               static_cast<std::uint64_t>(read.load()),
               static_cast<std::uint64_t>(dispatched.size()),
               static_cast<std::uint64_t>(missing.load()),
               static_cast<std::uint64_t>(failed.load()));
  if (out_of_order > 0) {
    mxi::eprint("warning: %" PRIu64
                " frames arrived after a later one and were "
                "dropped\n",
                static_cast<std::uint64_t>(out_of_order));
  }
  // dials.find_spots' own summary, wording included, so that a log from this
  // and a log from that can be read the same way and compared line for line.
  // The two "Calculated" lines are one pass here rather than two, so their
  // counts are equal by construction; they are both printed anyway, because a
  // block that reads differently from the familiar one is the thing this
  // replaced.
  const std::uint64_t sized =
      counts.groups - counts.too_small - counts.too_large;

  mxi::print("Found %" PRIu64 " signal pixels on %" PRIu64 " frames\n",
             static_cast<std::uint64_t>(counts.signal_pixels),
             static_cast<std::uint64_t>(grouped));
  mxi::print("Extracted %" PRIu64 " spots\n",
             static_cast<std::uint64_t>(counts.groups));
  mxi::print("Removed %" PRIu64 " spots with size < %zu pixels\n",
             static_cast<std::uint64_t>(counts.too_small),
             options.grouping.min_spot_size);
  mxi::print("Removed %" PRIu64 " spots with size > %zu pixels\n",
             static_cast<std::uint64_t>(counts.too_large),
             options.grouping.max_spot_size);
  mxi::print("Calculated %" PRIu64 " spot centroids\n",
             static_cast<std::uint64_t>(sized));
  mxi::print("Calculated %" PRIu64 " spot intensities\n",
             static_cast<std::uint64_t>(sized));
  // Only when the filter ran: "Filtered 48 of 48" would otherwise read as a
  // filter that passed everything rather than one that was switched off.
  if (options.grouping.max_separation > 0.0) {
    mxi::print("Filtered %" PRIu64 " of %" PRIu64
               " spots by peak-centroid distance\n",
               static_cast<std::uint64_t>(counts.accepted),
               static_cast<std::uint64_t>(sized));
  }

  // Spots per image, as dials.find_spots draws it: the part of this report a
  // user reads at a glance. A spot's image is floor(z) + 1, image n starting at
  // z = n - 1, over the images the scan covers or, without a scan, the series.
  if (!labeller.spots().empty()) {
    std::int64_t first = 1;
    std::int64_t last = static_cast<std::int64_t>(info.images);
    if (experiments.has_scan) {
      first = experiments.first_image;
      last = experiments.last_image;
    }
    if (last >= first) {
      std::vector<std::size_t> per_image(
          static_cast<std::size_t>(last - first + 1), 0);
      for (const auto &spot : labeller.spots()) {
        const std::int64_t image =
            static_cast<std::int64_t>(std::floor(spot.position[2])) + 1;
        if (image >= first && image <= last)
          ++per_image[static_cast<std::size_t>(image - first)];
      }
      mxi::print("\nHistogram of spots per image:\n");
      for (const std::string &line :
           mxi::spots::spot_histogram(per_image, first))
        mxi::print("%s\n", line.c_str());
      mxi::print("\n");
      // Each sweep's, a list a sweep: the summary keeps them all.
      static mxi::json::Array per_sweep, firsts;
      mxi::json::Array each;
      for (std::size_t n : per_image)
        each.push_back(mxi::json::Value(static_cast<std::int64_t>(n)));
      per_sweep.push_back(mxi::json::Value(std::move(each)));
      firsts.push_back(mxi::json::Value(first));
      find_summary().set("first_image", mxi::json::Value(firsts));
      find_summary().set("spots_per_image", mxi::json::Value(per_sweep));
    }
    // By resolution, if the experiment's geometry can be read in full.
    if (!options.experiments.empty()) {
      try {
        const mxi::ExperimentList list =
            mxi::read_experiments(options.experiments);
        if (index < list.size() && list[index].detector.size() > 0) {
          const mxi::Experiment &e = list[index];
          const mxi::Vec3 s0 = e.beam.s0();
          std::vector<double> dstar;
          dstar.reserve(labeller.spots().size());
          for (const auto &spot : labeller.spots()) {
            const mxi::Vec3 lab =
                e.detector[0].lab_coord_px(spot.position[0], spot.position[1]);
            const mxi::Vec3 s1 = lab / lab.norm() / e.beam.wavelength;
            dstar.push_back((s1 - s0).norm());
          }
          report_resolution(dstar);
        }
      } catch (const std::exception &) {
        // No full geometry to be read: no table by resolution.
      }
    }
  }
  judge_hot(signal_on, static_cast<std::uint64_t>(grouped),
            static_cast<std::uint64_t>(info.width), options.hot_fraction,
            options.mask_hot, index);
  // A row a sweep, and the totals over them: each sweep had written over the
  // last's (Graeme's four, only one shown).
  {
    static std::vector<mxi::json::Value> c_sweep, c_first, c_images, c_pixels,
        c_spots;
    static std::int64_t all_pixels = 0, all_spots = 0;
    const std::int64_t run_first =
        experiments.has_scan
            ? static_cast<std::int64_t>(experiments.first_image)
            : 1;
    const std::int64_t run_last =
        experiments.has_scan ? static_cast<std::int64_t>(experiments.last_image)
                             : static_cast<std::int64_t>(info.images);
    c_sweep.push_back(mxi::json::Value(static_cast<std::int64_t>(index)));
    c_first.push_back(mxi::json::Value(static_cast<std::int64_t>(run_first)));
    c_images.push_back(
        mxi::json::Value(static_cast<std::int64_t>(run_last - run_first + 1)));
    c_pixels.push_back(
        mxi::json::Value(static_cast<std::int64_t>(counts.signal_pixels)));
    c_spots.push_back(
        mxi::json::Value(static_cast<std::int64_t>(counts.accepted)));
    all_pixels += static_cast<std::int64_t>(counts.signal_pixels);
    all_spots += static_cast<std::int64_t>(counts.accepted);
    find_summary().table(
        "each_sweep",
        {"sweep", "first_image", "images", "signal_pixels", "spots"},
        {c_sweep, c_first, c_images, c_pixels, c_spots});
    find_summary().set("signal_pixels", mxi::json::Value(all_pixels));
    find_summary().set("spots", mxi::json::Value(all_spots));
  }

  const double t_written_from = mxi::Timing::now();
  // No frame read at all: nothing to write, and an empty table would read as
  // images without spots. Said once, with why.
  if (read.load() == 0 && failed.load() > 0) {
    mxi::eprint(
        "mxi find: none of the %" PRIu64 " images could be read, so nothing "
        "is written. The first error: %s\n",
        static_cast<std::uint64_t>(dispatched.size()), first_error.c_str());
    return 1;
  }
  if (emit(labeller, info, experiments) != 0)
    return 1;

  if (options.timing) {
    const double wall = t_streamed - t_began;
    const double capacity = wall * options.threads;
    const auto share = [&](const mxi::ThreadSeconds &t) {
      char text[96];
      std::snprintf(text, sizeof text,
                    "%9.3f thread-s, %5.1f%% of the threads' time", t.seconds(),
                    capacity > 0.0 ? 100.0 * t.seconds() / capacity : 0.0);
      return std::string(text);
    };
    timing.add("reading and thresholding, wall", wall);
    timing.add("grouping what was left", t_finished - t_streamed);
    timing.add("writing", mxi::Timing::now() - t_written_from);
    timing.report(mxi::Stream::out);
    mxi::print("  across %d threads, %.3f s of wall each:\n", options.threads,
               wall);
    mxi::print("    %-22s %s\n", "reading (HDF5)", share(t_read).c_str());
    mxi::print("    %-22s %s\n", "decompressing", share(t_decompress).c_str());
    mxi::print("    %-22s %s\n",
               options.gpu ? "thresholding (GPU)" : "thresholding",
               share(t_threshold).c_str());
    if (split) {
      const double parts = t_upload.seconds() + t_stage0.seconds() +
                           t_stage1.seconds() + t_stage2.seconds() +
                           t_fused.seconds() + t_readback.seconds() +
                           t_sort.seconds();
      mxi::print("      %-20s %s\n", "uploading", share(t_upload).c_str());
      mxi::print("      %-20s %s\n", "stage 0, device",
                 share(t_stage0).c_str());
      mxi::print("      %-20s %s\n", "stage 1, device",
                 share(t_stage1).c_str());
      mxi::print("      %-20s %s\n", "stage 2, device",
                 share(t_stage2).c_str());
      if (t_fused.seconds() > 0.0)
        mxi::print("      %-20s %s\n", "fused kernel, device",
                   share(t_fused).c_str());
      mxi::print("      %-20s %s\n", "reading back", share(t_readback).c_str());
      mxi::print("      %-20s %s\n", "sorting", share(t_sort).c_str());
      mxi::ThreadSeconds rest;
      rest.add(t_threshold.seconds() - parts);
      mxi::print("      %-20s %s\n", "the rest: waiting", share(rest).c_str());
      mxi::print("      (the device's own work, upload and kernels, summed "
                 "over the frames: "
                 "%.3f s, in %.3f s of wall)\n",
                 t_upload.seconds() + t_stage0.seconds() + t_stage1.seconds() +
                     t_stage2.seconds() + t_fused.seconds(),
                 wall);
    } else if (options.gpu) {
      mxi::print(
          "      (the threshold's own split is taken on CUDA only: on Metal, "
          "measuring it serialises its stages)\n");
    }
    mxi::print("  and in the main thread: grouping %.3f s of the %.3f s wall\n",
               t_group.seconds(), wall);
  }

  return failed.load() == 0 ? 0 : 1;
}

namespace {

// The table one sweep's spots make, written as it always was.
std::int32_t write_one(const Options &options,
                       const mxi::spots::Labeller &labeller,
                       const mxi::series::Info &info,
                       const mxi::ExperimentImages &experiments) {
  try {
    mxi::SpotPart part;
    part.spots = &labeller.spots();
    part.pixels = &labeller.pixels();
    part.width = static_cast<std::size_t>(info.width);
    part.id = 0;
    part.identifier = experiments.identifier;
    write_table(options, {part}, options.shoeboxes);
  } catch (const std::exception &error) {
    mxi::eprint("%s\n", error.what());
    return 1;
  }
  report_written(options, labeller.spots().size());
  return 0;
}

} // namespace

// The program's entry, run by mxi (find) -- apps/mxi.cc.
int mxi_find_main(int argc, char **argv) {
  // Mirrored to mxi_find.log in the working directory, as DIALS writes
  // dials.find_spots.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_find.log");
  Options options;
  if (!parse_options(argc, argv, &options))
    return 2;
  // This run's step of the summary, after the steps its input carried: begun
  // here, before anything fills it.
  find_summary(options.experiments);
  // Nothing named, or a .rflx: one .rflx, its table written beside it first.
  {
    const std::string &o = options.output;
    const bool named_rflx =
        o.size() >= 5 && o.compare(o.size() - 5, 5, ".rflx") == 0;
    if (!options.output_named || named_rflx)
      options.rflx = options.output_named ? o : "strong.rflx";
  }
  if (options.mask_hot && options.rflx.empty()) {
    mxi::eprint("--mask-hot writes hot pixels into the experiment list's "
                "untrusted regions: give an .rflx or an experiment list, and "
                "write an .rflx\n");
    return 2;
  }
  // The whole run's clock, from here, for --timing.
  mxi::Timing timing(options.timing);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (options.sweeps <= 1) {
    return find_one(options, 0, timing,
                    [&](const mxi::spots::Labeller &labeller,
                        const mxi::series::Info &info,
                        const mxi::ExperimentImages &e) {
                      return write_one(options, labeller, info, e);
                    });
  }

  // Several sweeps, as dials.find_spots finds them: each experiment's spots
  // from its own images, one experiment after another, into one table, every
  // spot with its experiment's index as its id and each identifier in the
  // table's map.
  if (options.master_given) {
    mxi::eprint("%s holds %zu experiments, each reading its own images; a "
                "master file named as well cannot belong to them all -- leave "
                "it out\n",
                options.experiments.c_str(), options.sweeps);
    return 2;
  }
  struct Kept {
    std::vector<mxi::spots::Spot> spots;
    std::vector<mxi::spots::Pixel> pixels;
    std::size_t width = 0;
    std::string identifier;
  };
  std::vector<Kept> kept(options.sweeps);
  std::int32_t status = 0;
  for (std::size_t i = 0; i < options.sweeps; ++i) {
    mxi::ExperimentImages e;
    try {
      e = mxi::experiment_images(options.experiments, i);
    } catch (const std::exception &error) {
      mxi::eprint("%s\n", error.what());
      return 1;
    }
    if (!e.has_imageset || e.templated) {
      mxi::eprint("experiment %zu of %s does not name one NXmx file for its "
                  "images\n",
                  i, options.experiments.c_str());
      return 1;
    }
    Options one = options;
    one.master = e.image_file;
    mxi::print("\nExperiment %zu of %zu: images %s\n", i + 1, options.sweeps,
               one.master.c_str());
    status |= find_one(one, i, timing,
                       [&](const mxi::spots::Labeller &labeller,
                           const mxi::series::Info &info,
                           const mxi::ExperimentImages &x) {
                         kept[i].spots = labeller.spots();
                         kept[i].pixels = labeller.pixels();
                         kept[i].width = static_cast<std::size_t>(info.width);
                         kept[i].identifier = x.identifier;
                         return 0;
                       });
    if (status != 0)
      return status;
  }
  std::vector<mxi::SpotPart> parts;
  std::size_t total = 0;
  for (std::size_t i = 0; i < kept.size(); ++i) {
    mxi::SpotPart part;
    part.spots = &kept[i].spots;
    part.pixels = &kept[i].pixels;
    part.width = kept[i].width;
    part.id = static_cast<std::int32_t>(i);
    part.identifier = kept[i].identifier;
    parts.push_back(part);
    total += kept[i].spots.size();
  }
  try {
    write_table(options, parts, options.shoeboxes);
  } catch (const std::exception &error) {
    mxi::eprint("%s\n", error.what());
    return 1;
  }
  mxi::print("\n");
  for (std::size_t i = 0; i < kept.size(); ++i)
    mxi::print("  experiment %zu: %zu reflections\n", i, kept[i].spots.size());
  report_written(options, total);
  return 0;
}

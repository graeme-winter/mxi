// mxi_find -- the extended dispersion spot finder on an Apple
// GPU, writing a DIALS reflection table.
//
// The threshold in src/dext.{hh,cc} is a transcription of DIALS'
// DispersionExtendedThreshold and the kernels in src/dext_metal.{cc,metal} are
// a transcription of that. What this adds is everything DIALS does after the
// per-frame pixel list: three-dimensional grouping, centroids, and the file
// format -- src/dials_spots.{hh,cc} and src/refl.{hh,cc}.
//
//   dials.import /data/ins10_1_master.h5
//   mxi_find --gpu -e imported.expt
//   dials.index imported.expt strong.refl
//
// The .expt is dials.import's business and is read rather than written: the
// beam, the goniometer and the detector belong to dxtbx, and a second model of
// them here would be a second thing to keep in step. What is taken from it is
// the scan's image range, the panel size and the experiment identifier.
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
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../log_mirror.hh"
#include "../rflx.hh"
#include "../timing.hh"
#include "decompress.hh"
#include "dext.hh"
#include "dials_spots.hh"
#include "expt.hh"
#include "histogram.hh"
#include "queue.hh"
#include "refl.hh"
#include "series.hh"
#include "signal_pixel.hh"

#ifdef SPOTFINDER_GPU
#include "dext_gpu.hh"
#endif

namespace {

volatile std::sig_atomic_t interrupted = 0;

void on_signal(int /*signal*/) { interrupted = 1; }

using Clock = std::chrono::steady_clock;

struct Options {
  int threads = 0; // every core, as the other programs; -j for fewer
  int timeout_seconds = 60;
  int poll_milliseconds = 200;
  std::string master;                 // the NXmx HDF5 master file
  bool master_given = false;          // named on the command line
  std::size_t sweeps = 1;             // experiments in the list
  std::string experiments;            // -e: what dials.import wrote
  std::string output = "strong.refl"; // -o
  bool gpu = false;
  bool gpu_force =
      false; // 32-bit frames narrowed to 16 bits, for a GPU of 16 only
  bool timing = false;
  bool shoeboxes = true;
  bool two_d = false;
  bool z_offset_given = false;
  std::int64_t z_offset = 0;
  dials_spots::Options grouping;
  std::size_t chunk = 0; // frames collected before grouping; 0 picks one
};

// Which backend, and whether its arithmetic is the CPU's. A reflection table
// from a fast-math build is not one to compare against DIALS without knowing
// that, so it is in the one line that identifies the binary.
void report_version(const char *program) {
  std::printf("%s %s (%s%s)\n", program, SPOTFINDER_VERSION,
#ifdef SPOTFINDER_GPU
              gpu::backend(),
#else
              "no GPU",
#endif
#ifdef SPOTFINDER_FAST_MATH
              ", fast math"
#else
              ""
#endif
  );
}

// To standard output when it was asked for, and to standard error when it is
// the answer to a mistake: `mxi_find --help | less` showed nothing while it
// all went to standard error.
void usage(const char *program, std::FILE *to = stderr) {
  std::fprintf(
      to,
      "usage: %s [-j threads] [-g|--gpu] [-e imported.expt] [-o strong.refl]\n"
      "       [options] [master.nxs | imported.expt]\n"
      "\n"
      "  imported.expt      an experiment list, as -e: the images are the "
      "ones\n"
      "                     its imageset names\n"
      "  master.nxs         an NXmx HDF5 master file, or -x master.nxs.\n"
      "                     Optional when -e names an .expt: dials.import\n"
      "                     already recorded the file in its imageset block,\n"
      "                     and -x overrides it if the data has moved.\n"
      "  -e imported.expt   what dials.import wrote, for the scan range, the\n"
      "                     panel size and the experiment identifier\n"
      "  -o file            where to write the reflection table (strong.refl)\n"
      "  -j threads         frames read and thresholded at once (every core)\n"
      "  --timing           where the time goes: each stage's time summed "
      "across\n"
      "                     the threads, against the time they had\n"
      "  -g, --gpu          run the threshold on the GPU; 16-bit only under\n"
      "                     Metal, which has no double precision\n"
      "  --gpu-force        32-bit frames narrowed to 16 bits, so that a GPU "
      "of\n"
      "                     16 bits only takes them: the bad-pixel and\n"
      "                     the 16-bit one, every other count as it is, "
      "stopping "
      "at\n"
      "                     one of 0xFFFD or more. mxi_max says whether a "
      "series\n"
      "                     fits. The same spots as the 32-bit threshold\n"
      "  --no-shoeboxes     leave out the pixel data, which is most of the\n"
      "                     file and is not needed for indexing\n"
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
      "  --z-offset N       array index of image number 0; taken from -e when\n"
      "                     that is given, and 0 otherwise\n"
      "  --chunk N          frames held between groupings (a few per thread)\n"
      "  -t timeout         seconds to wait for the file to appear (default "
      "60)\n"
      "  -p poll-ms         interval between checks while waiting (default "
      "200)\n"
      "  --version          what this binary is, and what it was built with\n",
      program);
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
  int c = 0;
  while ((c = std::fgetc(file)) != EOF && std::isspace(c))
    ;
  std::fclose(file);
  return c == '{';
}

bool parse_options(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; i++) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "-j" && has_value) {
      options->threads = std::atoi(argv[++i]);
    } else if (flag == "-t" && has_value) {
      options->timeout_seconds = std::atoi(argv[++i]);
    } else if (flag == "-p" && has_value) {
      options->poll_milliseconds = std::atoi(argv[++i]);
    } else if (flag == "-x" && has_value) {
      options->master = argv[++i];
    } else if (flag == "-e" && has_value) {
      if (!options->experiments.empty()) {
        std::fprintf(stderr, "%s: two experiment lists, %s and %s\n", argv[0],
                     options->experiments.c_str(), argv[i + 1]);
        return false;
      }
      options->experiments = argv[++i];
    } else if (flag == "-o" && has_value) {
      options->output = argv[++i];
    } else if (flag == "-gpu") {
      // The old spelling, one dash for a long option: kept for scripts that
      // have it, for now, and said so.
      std::fprintf(
          stderr,
          "warning: -gpu is now --gpu, or -g; the old spelling will go\n");
      options->gpu = true;
    } else if (flag == "--subtract-background") {
      options->grouping.subtract_background = true;
    } else if (flag == "--gpu-force") {
      options->gpu_force = true;
    } else if (flag == "--gpu" || flag == "-g") {
      options->gpu = true;
    } else if (flag == "--timing") {
      options->timing = true;
    } else if (flag == "--no-shoeboxes") {
      options->shoeboxes = false;
    } else if (flag == "--2d") {
      options->two_d = true;
    } else if (flag == "--min-spot-size" && has_value) {
      options->grouping.min_spot_size =
          static_cast<std::size_t>(std::atoll(argv[++i]));
    } else if (flag == "--max-spot-size" && has_value) {
      options->grouping.max_spot_size =
          static_cast<std::size_t>(std::atoll(argv[++i]));
    } else if (flag == "--max-separation" && has_value) {
      options->grouping.max_separation = std::atof(argv[++i]);
    } else if (flag == "--z-offset" && has_value) {
      options->z_offset = std::atoll(argv[++i]);
      options->z_offset_given = true;
    } else if (flag == "--chunk" && has_value) {
      options->chunk = static_cast<std::size_t>(std::atoll(argv[++i]));
    } else if (flag == "--version") {
      report_version("mxi_find");
      std::exit(0);
    } else if (flag == "-h" || flag == "--help") {
      // Asked for, so not an error: it exited 2, like an unknown flag, which
      // made a script checking whether this is installed think it was broken.
      usage(argv[0], stdout);
      std::exit(0);
    } else if (!flag.empty() && flag[0] != '-' &&
               (looks_like_json(flag) || mxi::rflx::has_experiments(flag))) {
      // An experiment list given as the argument, as dials.find_spots takes
      // it: the images are then the ones its imageset names, as with -e. An
      // .expt, or a .rflx holding one (docs/rflx.md) -- HDF5, as a master is,
      // but with /experiments where a master has /entry.
      if (!options->experiments.empty()) {
        std::fprintf(stderr, "%s: two experiment lists, %s and %s\n", argv[0],
                     options->experiments.c_str(), flag.c_str());
        return false;
      }
      options->experiments = flag;
    } else if (!flag.empty() && flag[0] != '-' && options->master.empty()) {
      // The master file may be named without -x, since flagging the one file
      // this reads would be ceremony.
      options->master = flag;
    } else {
      usage(argv[0]);
      return false;
    }
  }
  if (options->threads < 0) {
    usage(argv[0]);
    return false;
  }
  if (options->threads == 0)
    options->threads =
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  // The master file may come from the .expt instead of the command line.
  // dials.import already recorded where the images are, in the imageset block,
  // so making the operator repeat it only creates an opportunity for the two to
  // disagree -- and spots found in one file and indexed against the geometry of
  // another is a mistake nothing downstream catches.
  //
  // An explicit -x still wins, because the .expt records an absolute path and a
  // dataset that has moved since import would otherwise be unusable.
  options->master_given = !options->master.empty();
  if (!options->experiments.empty()) {
    expt::Info info;
    try {
      info = expt::read(options->experiments);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      return false;
    }
    // Several sweeps each read their own images, named by their own image
    // sets, one at a time: main() takes each in turn -- counted whether or not
    // a master was named, or one named with several would read the first
    // experiment alone, silently.
    options->sweeps = std::max<std::size_t>(info.experiments, 1);
    if (!options->master.empty() || info.experiments > 1) {
      // Nothing to infer.
    } else if (!info.has_imageset) {
      std::fprintf(stderr,
                   "%s has no imageset, so it does not say where the images "
                   "are; name the master file, or pass -x\n",
                   options->experiments.c_str());
      return false;
    } else if (info.templated) {
      // Hashes stand for a numbered sequence of files, which this reads none
      // of. Saying so beats handing the path to HDF5 and reporting whatever it
      // makes of it.
      std::fprintf(stderr,
                   "%s names a file template, '%s', which is a numbered "
                   "sequence rather than one NXmx file; this reads NXmx only\n",
                   options->experiments.c_str(), info.image_file.c_str());
      return false;
    } else {
      options->master = info.image_file;
      std::fprintf(stdout, "Images: %s, from %s\n", options->master.c_str(),
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

void sleep_for(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// ---------------------------------------------------------------------------
// The frame buffer, and the threshold.
//
// The frame buffer is allocated through gpu::host_alloc when a device is in
// use, which on Metal is a shared MTLBuffer: the frame is decompressed straight
// into memory the kernels read and there is no copy in either direction.
// ---------------------------------------------------------------------------

#ifdef SPOTFINDER_GPU
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
#ifdef SPOTFINDER_GPU
      // Shared with the device on Metal, so the frame is decompressed straight
      // into memory the kernels read and there is no copy at all.
      if (on_device)
        data_ = static_cast<std::uint8_t *>(gpu::host_alloc(bytes));
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
#ifdef SPOTFINDER_GPU
    if (pinned_) {
      gpu::host_free(data_);
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
  int status = 0;
#ifdef SPOTFINDER_GPU
  if (on_device) {
    status = gpu::find<T>(pixels, *signal, height, width);
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

series::Info open_series(series::Series *source, const Options &options) {
  const Clock::time_point deadline =
      Clock::now() + std::chrono::seconds(options.timeout_seconds);
  bool announced = false;
  while (interrupted == 0) {
    series::Info info;
    if (source->try_open(&info))
      return info;
    if (Clock::now() > deadline)
      throw std::runtime_error("timed out waiting for " + source->describe());
    if (!announced) {
      std::fprintf(stdout, "Waiting for %s\n", source->describe().c_str());
      announced = true;
    }
    sleep_for(options.poll_milliseconds);
  }
  throw std::runtime_error("interrupted while waiting for a series");
}

// What the experiment list says, against what the series says. A mismatch here
// is worth stopping for: spot finding would succeed and index nothing, and the
// reason would not be visible in either file.
void reconcile(const expt::Info &experiments, const series::Info &series,
               Options *options) {
  std::fprintf(stdout, "Experiments: %s\n", describe(experiments).c_str());

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
    throw std::runtime_error(
        "the panel in " + options->experiments + " is " +
        std::to_string(experiments.image_fast) + " x " +
        std::to_string(experiments.image_slow) + " and the frames are " +
        std::to_string(series.width) + " x " + std::to_string(series.height) +
        " (fast x slow); these are not the same images");
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
    std::fprintf(stderr,
                 "warning: %s lists %llu frames in its imageset and %lld in "
                 "its scan; z follows the scan\n",
                 options->experiments.c_str(),
                 static_cast<unsigned long long>(experiments.frames),
                 static_cast<long long>(experiments.images()));
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
      std::fprintf(stdout,
                   "The scan starts at image %lld, so z starts at %lld\n",
                   static_cast<long long>(experiments.first_image),
                   static_cast<long long>(options->z_offset));
    }
  }
}

} // namespace

// What was written, or that nothing was.
void report_written(const Options &options, std::size_t rows) {
  if (rows == 0) {
    std::fprintf(stderr,
                 "No reflections found; %s is a well formed table with no rows "
                 "in it, which dials.index will refuse\n",
                 options.output.c_str());
    return;
  }
  std::error_code ignored;
  const std::uintmax_t bytes =
      std::filesystem::file_size(options.output, ignored);
  std::fprintf(stdout, "Wrote %zu reflections to %s (%.1f MB%s)\n", rows,
               options.output.c_str(), static_cast<double>(bytes) / 1e6,
               options.shoeboxes ? ", most of it shoeboxes" : "");
}

// What becomes of one experiment's spots: written as the table, for one sweep,
// or kept to be written with the others', for several.
using Emit = std::function<int(const dials_spots::Labeller &,
                               const series::Info &, const expt::Info &)>;

// One experiment's spots, from its own images: experiment `index` of the list,
// or the master file alone without one. Handed to `emit` where they would be
// written, then the timing reported.
int find_one(Options options, std::size_t index, mxi::Timing &timing,
             const Emit &emit) {
  std::unique_ptr<series::Series> source;
  series::Info info;
  // How many frames will be offered, when fewer than the file holds.
  std::uint64_t restricted = 0;
  expt::Info experiments;
  try {
    if (!options.experiments.empty())
      experiments = expt::read(options.experiments, index);

    source = series::nxmx(options.master);

    // Before the series is opened, so that asking for a device that is not
    // there fails at once rather than after a wait.
    if (options.gpu) {
#ifdef SPOTFINDER_GPU
      if (!gpu::available()) {
        throw std::runtime_error(
            std::string("--gpu was given but no usable ") + gpu::backend() +
            " device was found: " + gpu::unavailable_reason());
      }
      on_device = true;
#else
      throw std::runtime_error(
          "--gpu was given but this build has no GPU support; configure with "
          "-DSPOTFINDER_METAL=ON or -DSPOTFINDER_CUDA=ON");
#endif
    }

    info = open_series(source.get(), options);
    if (!options.experiments.empty())
      reconcile(experiments, info, &options);

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
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }

  if (restricted > 0 && restricted != info.images) {
    std::fprintf(stdout,
                 "Reading %llu of the %llu images, the ones the scan covers\n",
                 static_cast<unsigned long long>(restricted),
                 static_cast<unsigned long long>(info.images));
  }
  std::fprintf(
      stdout,
      "Series %s: %llu images of %llu x %llu pixels (fast x slow), from %s, "
      "%d thread%s, "
      "on the %s\n",
      info.name.c_str(), static_cast<unsigned long long>(info.images),
      static_cast<unsigned long long>(info.width),
      static_cast<unsigned long long>(info.height), source->describe().c_str(),
      options.threads, options.threads == 1 ? "" : "s",
      options.gpu ? "GPU" : "CPU");
  if (info.masked_pixels > 0)
    std::fprintf(stdout,
                 "%llu pixels masked by the detector's own pixel_mask\n",
                 static_cast<unsigned long long>(info.masked_pixels));
  std::fprintf(
      stdout,
      "Grouping in %s, %zu to %zu pixels a spot, peak within %.1f of the "
      "centroid\n",
      options.two_d ? "two dimensions" : "three dimensions",
      options.grouping.min_spot_size, options.grouping.max_spot_size,
      options.grouping.max_separation);

  dials_spots::Labeller labeller(static_cast<std::size_t>(info.height),
                                 static_cast<std::size_t>(info.width),
                                 options.grouping);

  work::Queue<std::string> queue(static_cast<std::size_t>(options.threads) * 4);
  Chunk chunk;
  std::atomic<std::uint64_t> read{0};
  std::atomic<std::uint64_t> missing{0};
  std::atomic<std::uint64_t> failed{0};
  std::atomic<int> live{options.threads};
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
#ifdef SPOTFINDER_GPU
  const bool split =
      options.timing && options.gpu && std::string(gpu::backend()) == "CUDA";
  if (split)
    gpu::profile_stages(true);
#else
  const bool split = false;
#endif
  const double t_began = mxi::Timing::now();

  std::vector<std::thread> workers;
  for (int i = 0; i < options.threads; i++) {
    workers.emplace_back([&, i] {
      // However this thread leaves, the last one out says so: a chunk in
      // flight is otherwise waited on forever.
      struct Leaving {
        std::atomic<int> &live;
        Chunk &chunk;
        ~Leaving() {
          if (live.fetch_sub(1) == 1)
            chunk.abandon();
        }
      } leaving{live, chunk};

      std::unique_ptr<series::Reader> reader;
      try {
        reader = source->reader();
      } catch (const std::exception &error) {
        // The keys this thread would have taken are still in the queue for the
        // others; only if every thread fails does the run stop.
        std::fprintf(stderr, "thread %d: %s\n", i, error.what());
        return;
      }

      FrameBuffer buffer;
      FrameBuffer narrowed; // --gpu-force: a 32-bit frame as 16 bits
      series::Frame frame;
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
                decompress::frame_bytes(height, width, frame.bit_depth);
            std::uint8_t *const pixels = buffer.get(bytes);
            const double d0 = mxi::Timing::now();
            decompress::image(frame.data, frame.algorithm, frame.bit_depth,
                              height, width, {pixels, bytes});
            series::apply_pixel_mask(frame, {pixels, bytes});
            const double d1 = mxi::Timing::now();
            t_decompress.add(d1 - d0);
            found.number = frame.number;
            unsigned depth = frame.bit_depth;
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
                    "(mxi_max says which series fit)");
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
#ifdef SPOTFINDER_GPU
            if (split) {
              const gpu::StageTimes st =
                  gpu::last_stage_times(); // milliseconds
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
          failed++;
          have = false;
          std::fprintf(stderr, "%s: %s\n", key.c_str(), error.what());
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
    if (chunk.abandoned()) {
      std::fprintf(stderr,
                   "every thread has gone, so the rest of the series cannot be "
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
      try {
        const double g0 = mxi::Timing::now();
        labeller.add(z, found.pixels);
        t_group.add(mxi::Timing::now() - g0);
      } catch (const std::exception &error) {
        std::fprintf(stderr, "grouping frame %lld: %s\n",
                     static_cast<long long>(found.number), error.what());
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
      std::fprintf(stderr, "%s\n", error.what());
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
      std::fprintf(stderr, "No new images for %d seconds, giving up\n",
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

  queue.close();
  for (std::thread &worker : workers)
    worker.join();
  const double t_streamed = mxi::Timing::now();

  labeller.finish();
  const double t_finished = mxi::Timing::now();

  const double seconds =
      std::chrono::duration<double>(Clock::now() - began).count();
  const dials_spots::Counts &counts = labeller.counts();

  std::fprintf(stdout,
               "Thresholded %llu of %llu images in %.1f s (%.1f images/s), "
               "%llu never written, %llu failures\n",
               static_cast<unsigned long long>(read.load()),
               static_cast<unsigned long long>(dispatched.size()), seconds,
               seconds > 0 ? read.load() / seconds : 0.0,
               static_cast<unsigned long long>(missing.load()),
               static_cast<unsigned long long>(failed.load()));
  if (out_of_order > 0) {
    std::fprintf(stderr,
                 "warning: %llu frames arrived after a later one and were "
                 "dropped\n",
                 static_cast<unsigned long long>(out_of_order));
  }
  // dials.find_spots' own summary, wording included, so that a log from this
  // and a log from that can be read the same way and compared line for line.
  // The two "Calculated" lines are one pass here rather than two, so their
  // counts are equal by construction; they are both printed anyway, because a
  // block that reads differently from the familiar one is the thing this
  // replaced.
  const std::uint64_t sized =
      counts.groups - counts.too_small - counts.too_large;

  std::fprintf(stdout, "Found %llu signal pixels on %llu frames\n",
               static_cast<unsigned long long>(counts.signal_pixels),
               static_cast<unsigned long long>(grouped));
  std::fprintf(stdout, "Extracted %llu spots\n",
               static_cast<unsigned long long>(counts.groups));
  std::fprintf(stdout, "Removed %llu spots with size < %zu pixels\n",
               static_cast<unsigned long long>(counts.too_small),
               options.grouping.min_spot_size);
  std::fprintf(stdout, "Removed %llu spots with size > %zu pixels\n",
               static_cast<unsigned long long>(counts.too_large),
               options.grouping.max_spot_size);
  std::fprintf(stdout, "Calculated %llu spot centroids\n",
               static_cast<unsigned long long>(sized));
  std::fprintf(stdout, "Calculated %llu spot intensities\n",
               static_cast<unsigned long long>(sized));
  // Only when the filter ran: "Filtered 48 of 48" would otherwise read as a
  // filter that passed everything rather than one that was switched off.
  if (options.grouping.max_separation > 0.0) {
    std::fprintf(stdout,
                 "Filtered %llu of %llu spots by peak-centroid distance\n",
                 static_cast<unsigned long long>(counts.accepted),
                 static_cast<unsigned long long>(sized));
  }

  // Spots per image, as dials.find_spots draws it: the part of this report a
  // user reads at a glance. A spot's image is floor(z) + 1, image n starting at
  // z = n - 1, over the images the scan covers or, without a scan, the series.
  if (!labeller.spots().empty()) {
    long long first = 1;
    long long last = static_cast<long long>(info.images);
    if (experiments.has_scan) {
      first = experiments.first_image;
      last = experiments.last_image;
    }
    if (last >= first) {
      std::vector<std::size_t> per_image(
          static_cast<std::size_t>(last - first + 1), 0);
      for (const auto &spot : labeller.spots()) {
        const long long image =
            static_cast<long long>(std::floor(spot.position[2])) + 1;
        if (image >= first && image <= last)
          ++per_image[static_cast<std::size_t>(image - first)];
      }
      std::fprintf(stdout, "\nHistogram of spots per image:\n");
      for (const std::string &line : spots::spot_histogram(per_image, first))
        std::fprintf(stdout, "%s\n", line.c_str());
      std::fprintf(stdout, "\n");
    }
  }

  const double t_written_from = mxi::Timing::now();
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
    timing.report(stdout);
    std::fprintf(stdout, "  across %d threads, %.3f s of wall each:\n",
                 options.threads, wall);
    std::fprintf(stdout, "    %-22s %s\n", "reading (HDF5)",
                 share(t_read).c_str());
    std::fprintf(stdout, "    %-22s %s\n", "decompressing",
                 share(t_decompress).c_str());
    std::fprintf(stdout, "    %-22s %s\n",
                 options.gpu ? "thresholding (GPU)" : "thresholding",
                 share(t_threshold).c_str());
    if (split) {
      const double parts = t_upload.seconds() + t_stage0.seconds() +
                           t_stage1.seconds() + t_stage2.seconds() +
                           t_fused.seconds() + t_readback.seconds() +
                           t_sort.seconds();
      std::fprintf(stdout, "      %-20s %s\n", "uploading",
                   share(t_upload).c_str());
      std::fprintf(stdout, "      %-20s %s\n", "stage 0, device",
                   share(t_stage0).c_str());
      std::fprintf(stdout, "      %-20s %s\n", "stage 1, device",
                   share(t_stage1).c_str());
      std::fprintf(stdout, "      %-20s %s\n", "stage 2, device",
                   share(t_stage2).c_str());
      if (t_fused.seconds() > 0.0)
        std::fprintf(stdout, "      %-20s %s\n", "fused kernel, device",
                     share(t_fused).c_str());
      std::fprintf(stdout, "      %-20s %s\n", "reading back",
                   share(t_readback).c_str());
      std::fprintf(stdout, "      %-20s %s\n", "sorting",
                   share(t_sort).c_str());
      mxi::ThreadSeconds rest;
      rest.add(t_threshold.seconds() - parts);
      std::fprintf(stdout, "      %-20s %s\n", "the rest: waiting",
                   share(rest).c_str());
      std::fprintf(stdout,
                   "      (the device's own work, upload and kernels, summed "
                   "over the frames: "
                   "%.3f s, in %.3f s of wall)\n",
                   t_upload.seconds() + t_stage0.seconds() +
                       t_stage1.seconds() + t_stage2.seconds() +
                       t_fused.seconds(),
                   wall);
    } else if (options.gpu) {
      std::fprintf(
          stdout,
          "      (the threshold's own split is taken on CUDA only: on Metal, "
          "measuring it serialises its stages)\n");
    }
    std::fprintf(
        stdout,
        "  and in the main thread: grouping %.3f s of the %.3f s wall\n",
        t_group.seconds(), wall);
  }

  return failed.load() == 0 ? 0 : 1;
}

namespace {

// The table one sweep's spots make, written as it always was.
int write_one(const Options &options, const dials_spots::Labeller &labeller,
              const series::Info &info, const expt::Info &experiments) {
  refl::Options writing;
  writing.identifier = experiments.identifier;
  writing.shoeboxes = options.shoeboxes;
  try {
    refl::write(options.output, labeller.spots(), labeller.pixels(),
                static_cast<std::size_t>(info.width), writing);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  report_written(options, labeller.spots().size());
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  // Mirrored to mxi_find.log in the working directory, as DIALS writes
  // dials.find_spots.log; not for a run that only asks for help.
  if (!mxi::only_asks_for_help(argc, argv))
    mxi::mirror_to_log("mxi_find.log");
  Options options;
  if (!parse_options(argc, argv, &options))
    return 2;
  // The whole run's clock, from here, for --timing.
  mxi::Timing timing(options.timing);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (options.sweeps <= 1) {
    return find_one(options, 0, timing,
                    [&](const dials_spots::Labeller &labeller,
                        const series::Info &info, const expt::Info &e) {
                      return write_one(options, labeller, info, e);
                    });
  }

  // Several sweeps, as dials.find_spots finds them: each experiment's spots
  // from its own images, one experiment after another, into one table, every
  // spot with its experiment's index as its id and each identifier in the
  // table's map.
  if (options.master_given) {
    std::fprintf(stderr,
                 "%s holds %zu experiments, each reading its own images; a "
                 "master file named as well cannot belong to them all -- leave "
                 "it out\n",
                 options.experiments.c_str(), options.sweeps);
    return 2;
  }
  struct Kept {
    std::vector<dials_spots::Spot> spots;
    std::vector<dials_spots::Pixel> pixels;
    std::size_t width = 0;
    std::string identifier;
  };
  std::vector<Kept> kept(options.sweeps);
  int status = 0;
  for (std::size_t i = 0; i < options.sweeps; ++i) {
    expt::Info e;
    try {
      e = expt::read(options.experiments, i);
    } catch (const std::exception &error) {
      std::fprintf(stderr, "%s\n", error.what());
      return 1;
    }
    if (!e.has_imageset || e.templated) {
      std::fprintf(stderr,
                   "experiment %zu of %s does not name one NXmx file for its "
                   "images\n",
                   i, options.experiments.c_str());
      return 1;
    }
    Options one = options;
    one.master = e.image_file;
    std::fprintf(stdout, "\nExperiment %zu of %zu: images %s\n", i + 1,
                 options.sweeps, one.master.c_str());
    status |= find_one(one, i, timing,
                       [&](const dials_spots::Labeller &labeller,
                           const series::Info &info, const expt::Info &x) {
                         kept[i].spots = labeller.spots();
                         kept[i].pixels = labeller.pixels();
                         kept[i].width = static_cast<std::size_t>(info.width);
                         kept[i].identifier = x.identifier;
                         return 0;
                       });
    if (status != 0)
      return status;
  }
  std::vector<refl::Part> parts;
  std::size_t total = 0;
  for (std::size_t i = 0; i < kept.size(); ++i) {
    refl::Part part;
    part.spots = &kept[i].spots;
    part.pixels = &kept[i].pixels;
    part.width = kept[i].width;
    part.id = static_cast<int>(i);
    part.identifier = kept[i].identifier;
    parts.push_back(part);
    total += kept[i].spots.size();
  }
  refl::Options writing;
  writing.shoeboxes = options.shoeboxes;
  try {
    refl::write_parts(options.output, parts, writing);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::fprintf(stdout, "\n");
  for (std::size_t i = 0; i < kept.size(); ++i)
    std::fprintf(stdout, "  experiment %zu: %zu reflections\n", i,
                 kept[i].spots.size());
  report_written(options, total);
  return 0;
}

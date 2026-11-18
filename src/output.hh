#pragma once

// What mxi writes for a person to read: to the terminal and, while a log is
// open, to the log too, in the same call (docs/windows.md, The log mirror).
//
// mxi::print and mxi::eprint in place of printf and fprintf(stderr, ...):
// each formats once and writes the text to the terminal's stdout or stderr
// and to the open log, under one lock, so that threads' lines do not
// interleave, the log flushed at each call -- nothing waits anywhere to be
// lost in a crash, as nothing did in the mirror this replaces, which made
// stdout and stderr themselves streams writing to both (fopencookie, funopen:
// glibc's and BSD's, which Windows has not, nor lets stdout be replaced).
// stdout is flushed at each newline, piped or not, so that a step's output
// reaches mxi ui's log as it is written; and before anything goes to stderr,
// so that the two keep their order on a terminal.
//
// Not mirrored: what another library writes straight to stderr (HDF5's own
// error stack, where not turned off) -- on the terminal, not in the log.
//
// Header-only, so the spot finder, which does not link the pipeline's
// library, can use it too.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__MINGW32__)
// MinGW checks "printf" against Microsoft's old C runtime, which knows no
// %zu; mxi's formats are C99's, as MinGW's own stdio gives them
// (__USE_MINGW_ANSI_STDIO), and as Microsoft's current runtime does.
#define MXI_PRINTF_LIKE(format_at, args_at)                                    \
  __attribute__((format(gnu_printf, format_at, args_at)))
#elif defined(__GNUC__) || defined(__clang__)
#define MXI_PRINTF_LIKE(format_at, args_at)                                    \
  __attribute__((format(printf, format_at, args_at)))
#else
#define MXI_PRINTF_LIKE(format_at, args_at)
#endif

namespace mxi {

//: Where a line goes on the terminal: standard output or standard error.
enum class Stream : std::uint8_t { out, err };

namespace output_detail {

struct State {
  std::mutex lock;
  std::vector<std::FILE *> logs; //: the open logs, the last the one written
  std::string banner;            //: each log's first lines (set_banner)
  bool banner_shown = false;     //: on the terminal yet, at the first log
  int quiet = 0; //: QuietTerminal's depth: above 0, stdout's lines to the log
};

inline State &state() {
  static State s;
  return s;
}

inline void emit(Stream to, const char *text, std::size_t n) {
  State &s = state();
  const std::lock_guard<std::mutex> hold(s.lock);
  if (to == Stream::out && s.quiet > 0) {
    // Quietened (QuietTerminal): the log only.
  } else if (to == Stream::out) {
    std::fwrite(text, 1, n, stdout);
    if (std::memchr(text, '\n', n) != nullptr)
      std::fflush(stdout);
  } else {
    std::fflush(stdout);
    std::fwrite(text, 1, n, stderr);
    std::fflush(stderr);
  }
  if (!s.logs.empty()) {
    std::fwrite(text, 1, n, s.logs.back());
    std::fflush(s.logs.back());
  }
}

inline void vemit(Stream to, const char *format, std::va_list args) {
  char small[1024];
  std::va_list again;
  va_copy(again, args);
  const std::int32_t n = std::vsnprintf(small, sizeof small, format, again);
  va_end(again);
  if (n < 0)
    return;
  if (static_cast<std::size_t>(n) < sizeof small) {
    emit(to, small, static_cast<std::size_t>(n));
    return;
  }
  std::string big(static_cast<std::size_t>(n) + 1, '\0');
  std::vsnprintf(big.data(), big.size(), format, args);
  emit(to, big.data(), static_cast<std::size_t>(n));
}

} // namespace output_detail

//: While one lives, what goes to stdout goes to the open log alone; errors
//: still to the terminal, and a progress bar, written there directly. For a
//: step run inside another for one thing it finds -- mxi untrusted --hot's
//: spot finding, whose tables were not what was asked for (Graeme, 18
//: November).
class QuietTerminal {
public:
  QuietTerminal() {
    const std::lock_guard<std::mutex> hold(output_detail::state().lock);
    ++output_detail::state().quiet;
  }
  ~QuietTerminal() {
    const std::lock_guard<std::mutex> hold(output_detail::state().lock);
    --output_detail::state().quiet;
  }
  QuietTerminal(const QuietTerminal &) = delete;
  QuietTerminal &operator=(const QuietTerminal &) = delete;
};

//: printf, to the terminal's stdout and the open log.
inline void print(const char *format, ...) MXI_PRINTF_LIKE(1, 2);
inline void print(const char *format, ...) {
  std::va_list args;
  va_start(args, format);
  output_detail::vemit(Stream::out, format, args);
  va_end(args);
}

//: fprintf(stderr, ...), to the terminal's stderr and the open log.
inline void eprint(const char *format, ...) MXI_PRINTF_LIKE(1, 2);
inline void eprint(const char *format, ...) {
  std::va_list args;
  va_start(args, format);
  output_detail::vemit(Stream::err, format, args);
  va_end(args);
}

//: printf to either, for what is written to one or the other by its caller.
inline void print_to(Stream to, const char *format, ...) MXI_PRINTF_LIKE(2, 3);
inline void print_to(Stream to, const char *format, ...) {
  std::va_list args;
  va_start(args, format);
  output_detail::vemit(to, format, args);
  va_end(args);
}

//: Text as it is, as fputs: no formatting.
inline void put(Stream to, const char *text) {
  output_detail::emit(to, text, std::strlen(text));
}

//: The terminal's stdout flushed -- a line still being built (a progress
//: bar's), which has no newline to flush it.
inline void flush() {
  const std::lock_guard<std::mutex> hold(output_detail::state().lock);
  std::fflush(stdout);
}

//: A log opened: what is printed from now goes to it too, until it is closed.
//: Logs nest: the last opened is written, the one before it again once the
//: last is closed (mxi process, running each step in its own process now,
//: in code later -- docs/windows.md).
//: A log opened begins with the banner, if one is set: what mxi this is and
//: where it ran (mxi's main sets it). The first log opened shows it on the
//: terminal too, once: so a run with a record -- every processing command
//: keeps a log -- says what made it, and mxi show, whose lines are often read
//: by a program, says nothing more. A step's log under mxi process has it as
//: well, the terminal not again.
inline bool open_log(const char *path) {
  std::FILE *f = std::fopen(path, "wb");
  if (f == nullptr)
    return false;
  output_detail::State &s = output_detail::state();
  const std::lock_guard<std::mutex> hold(s.lock);
  if (!s.banner.empty()) {
    if (!s.banner_shown) {
      std::fputs(s.banner.c_str(), stdout);
      s.banner_shown = true;
    }
    std::fputs(s.banner.c_str(), f);
  }
  s.logs.push_back(f);
  return true;
}

//: The banner each log begins with, its newlines its own; empty for none.
inline void set_banner(const std::string &text) {
  output_detail::State &s = output_detail::state();
  const std::lock_guard<std::mutex> hold(s.lock);
  s.banner = text;
}

//: How many logs are open: mxi process notes it before a step, and closes
//: what the step opened after it (close_logs_to).
inline std::size_t log_depth() {
  output_detail::State &s = output_detail::state();
  const std::lock_guard<std::mutex> hold(s.lock);
  return s.logs.size();
}

//: Every log opened since there were `depth`, closed, the last first.
inline void close_logs_to(std::size_t depth) {
  output_detail::State &s = output_detail::state();
  const std::lock_guard<std::mutex> hold(s.lock);
  while (s.logs.size() > depth) {
    std::fclose(s.logs.back());
    s.logs.pop_back();
  }
}

//: The last log opened, closed; the one before it written again.
inline void close_log() {
  output_detail::State &s = output_detail::state();
  const std::lock_guard<std::mutex> hold(s.lock);
  if (s.logs.empty())
    return;
  std::fclose(s.logs.back());
  s.logs.pop_back();
}

} // namespace mxi

// Reading and writing DIALS reflection tables, hand-rolled msgpack.
//
// The layout, confirmed against files written by DIALS 3.x and by
// mxi_find:
//
//   [ "dials::af::reflection_table", 2, { identifiers, nrows, data } ]
//   data[name] = [ type_name, [ nrows, blob ] ]
//
// Text is msgpack `str`, payloads are `bin`, blobs are little-endian with the
// components of a compound type adjacent. The count inside a payload is the
// number of ROWS, not scalars, and is checked rather than trusted, because the
// two disagreeing is what a truncated file looks like.
//
// Element widths: int 4, std::size_t 8, double 8, bool 1, vec2<double> 16,
// vec3<double> 24, int6 24, cctbx::miller::index<> 12.
//
// Columns of a type this does not understand -- a shoebox, most notably -- are
// kept as their raw bytes and written back unchanged.
//
// They used to be dropped, on the argument that nothing here can subset a
// shoebox and one that silently stopped matching its table would be worse than
// its absence. The argument is sound and the conclusion was not: indexing and
// refinement do not remove rows, they add columns and set flags, so the bytes
// are still correct and dropping them turned an 84 MB table into a 20 MB one
// that dials.integrate refuses with "shoebox data missing from reflection
// table".
//
// The condition is therefore checked rather than assumed. An opaque column is
// written back only if the table still has the row count it was read with, and
// writing one that does not throws rather than dropping it: a shoebox that no
// longer matches its table is the thing the original argument was right about,
// and doing it silently a second time for a better reason would be no better.

#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mxi {

// The `flags` column is a bitmask, and dials.* filters on it. A table whose
// flags are never set processes perfectly and then cannot be selected,
// filtered or plotted by anything downstream, because every DIALS tool asks
// the flags which reflections it is looking at rather than inspecting the
// Miller indices.
//
// These values are dxtbx's, and the two that are used here were confirmed
// against a real DIALS indexed.refl: its indexed reflections carry 36, which
// is strong | indexed.
namespace flag {
constexpr std::int64_t kPredicted = 1 << 0;
constexpr std::int64_t kObserved = 1 << 1;
constexpr std::int64_t kIndexed = 1 << 2;
constexpr std::int64_t kUsedInRefinement = 1 << 3;
constexpr std::int64_t kStrong = 1 << 5;
//: Set on reflections the refinement drew in and then rejected. Identified
//: from a real DIALS indexed.refl rather than guessed: 8230 rows carry bit 17,
//: every one of them indexed, none of them also marked used_in_refinement, and
//: their median |xyzcal - xyzobs| is 0.843 px against 0.310 for the rest.
constexpr std::int64_t kCentroidOutlier = 1 << 17;
//: Identified from a DIALS integrated.refl rather than guessed: its rows carry
//: flag 769, which is predicted | 256 | 512, and the two bits split 21972 and
//: 20688 ways across reflections that have summation and profile-fitted
//: intensities respectively.
constexpr std::int64_t kIntegratedSum = 1 << 8;
constexpr std::int64_t kIntegratedPrf = 1 << 9;
//: From DIALS' own Flags enum, dials/array_family/reflection_table.h, where
//: they are ForegroundIncludesBadPixels, BackgroundIncludesBadPixels and
//: FailedDuringSummation. Read from the source rather than remembered: a
//: first guess at bit 19 took it for an exclusion flag, which it is not.
//:
//: DIALS sets the first and the third on a reflection whose foreground reaches
//: a masked pixel -- 95.6 and 96 per cent of the gap-crossing reflections of a
//: real integration -- and does NOT set kIntegratedSum on it, because a sum
//: over a foreground with pixels missing is not that reflection's intensity.
//: dials.scale's combined intensity needs both kIntegratedSum and
//: kIntegratedPrf, so those reflections never reach scaling.
constexpr std::int64_t kForegroundIncludesBadPixels = 1 << 14;
constexpr std::int64_t kBackgroundIncludesBadPixels = 1 << 15;
constexpr std::int64_t kFailedDuringSummation = 1 << 19;
} // namespace flag

class ReflError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct Column {
  std::string type;      // the C++ type name as it appears in the file
  std::size_t width = 1; // components per row
  bool integral = false; // which of the two stores below is in use
  std::vector<double> reals;
  std::vector<std::int64_t> ints;

  std::size_t rows() const {
    return (integral ? ints.size() : reals.size()) / (width ? width : 1);
  }
  double real(std::size_t row, std::size_t k = 0) const {
    return reals[row * width + k];
  }
  std::int64_t integer(std::size_t row, std::size_t k = 0) const {
    return ints[row * width + k];
  }
};

class Table {
public:
  std::size_t nrows = 0;
  std::map<std::size_t, std::string> identifiers;
  int version = 2;

  bool has(const std::string &name) const { return columns_.count(name) > 0; }
  const Column &at(const std::string &name) const;
  std::vector<std::string> names() const;
  const std::vector<std::string> &dropped() const { return dropped_; }

  //: A column whose type this package does not decode, kept verbatim.
  struct Opaque {
    std::string type;
    std::string bytes;
    std::size_t rows = 0;
  };
  const std::map<std::string, Opaque> &opaque() const { return opaque_; }
  void set_opaque(const std::string &name, Opaque value) {
    opaque_[name] = std::move(value);
  }
  void remove_opaque(const std::string &name) { opaque_.erase(name); }

  Column &real_column(const std::string &name, const std::string &type,
                      std::size_t width);
  // NOTE: these REPLACE an existing column of the same name with a zeroed one.
  // That is what a derived column wants -- xyzcal.px and the rest are
  // recomputed in full, and leaving stale values in the rows that are skipped
  // would be worse than clearing them. It is wrong for any column that has to
  // be read before it is written, and `flags` is one: setting the indexed bit
  // this way silently threw away the strong bit that dials.find_spots had set,
  // and the loss is invisible until something downstream filters on it.
  Column &int_column(const std::string &name, const std::string &type,
                     std::size_t width);

  // Returns the existing column if there is one, so it can be modified rather
  // than replaced. Creates a zeroed column if not.
  Column &modify_int_column(const std::string &name, const std::string &type,
                            std::size_t width);
  void set(const std::string &name, Column column) {
    columns_[name] = std::move(column);
  }

  void validate() const;

private:
  std::map<std::string, Column> columns_;
  std::vector<std::string> dropped_;
  std::map<std::string, Opaque> opaque_;
  friend Table read_reflections(const std::string &);
};

Table read_reflections(const std::string &path);
//: A msgpack table already in memory, as read_reflections decodes a file's:
//: for mxi_find, whose table goes into a .rflx without a file between.
Table decode_reflections(const std::string &raw);

//: The given rows of a table, in the order given, with every decoded column
//: and the identifiers. Undecoded columns -- shoeboxes, and anything else kept
//: verbatim as bytes -- are left out, since bytes cannot be split by row.
Table select_rows(const Table &table, const std::vector<std::size_t> &rows);
//: Tables of the same columns one after another, as several sweeps' are
//: joined: every decoded column concatenated, the identifier maps merged.
//: Refused -- not guessed at -- for tables whose columns differ in name, type
//: or width, and for any column kept as undecoded bytes, which cannot be split
//: into rows to be checked.
Table concat_rows(const std::vector<Table> &tables);
//: Throws ReflError when a binary column of `size` bytes cannot be written:
//: msgpack's bin32 describes at most 2^32 - 1, and a larger one used to be
//: written with its length wrapped, leaving everything after it unreadable.
//: Separate from the writer so the rule can be tested at its boundary without
//: building four gigabytes to do it.
void check_blob_size(std::uint64_t size);

void write_reflections(const std::string &path, const Table &table);

//: Whether row `i` carries a real predicted position.
//:
//: Not xyzcal != 0. A row that was never predicted has the column allocated
//: and never written, and holds whatever was in the memory -- on a real DIALS
//: file, denormals around 1e-320, which compare unequal to zero. Feeding those
//: to the reflecting-range likelihood put the estimate out by a factor of
//: four.
bool has_prediction(const Table &table, std::size_t row);

} // namespace mxi

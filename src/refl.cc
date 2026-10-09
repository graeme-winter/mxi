#include "refl.hh"

#include <bit>
#include <cstring>
#include <fstream>
#include <sstream>
#include <type_traits>

namespace mxi {

namespace {

constexpr const char *kTag = "dials::af::reflection_table";

struct TypeInfo {
  std::size_t element_bytes;
  std::size_t width;
  bool integral;
  bool is_unsigned;
};

// The one place the on-disk element widths live. Everything else derives from
// it, so a new type is one line here and nowhere else.
const std::map<std::string, TypeInfo> &type_table() {
  static const std::map<std::string, TypeInfo> types = {
      {"int", {4, 1, true, false}},
      {"std::size_t", {8, 1, true, true}},
      {"double", {8, 1, false, false}},
      {"bool", {1, 1, true, true}},
      {"vec2<double>", {8, 2, false, false}},
      {"vec3<double>", {8, 3, false, false}},
      {"mat3<double>", {8, 9, false, false}},
      {"int6", {4, 6, true, false}},
      {"cctbx::miller::index<>", {4, 3, true, false}},
      {"tiny<int,2>", {4, 2, true, false}},
  };
  return types;
}

// ------------------------------------------------------------------ reading

class Reader {
public:
  Reader(const std::uint8_t *data, std::size_t size)
      : p_(data), end_(data + size) {}

  std::uint8_t peek() const {
    if (p_ >= end_)
      throw ReflError("unexpected end of file");
    return *p_;
  }
  std::uint8_t byte() {
    if (p_ >= end_)
      throw ReflError("unexpected end of file");
    return *p_++;
  }
  void need(std::size_t n) const {
    if (static_cast<std::size_t>(end_ - p_) < n) {
      throw ReflError("unexpected end of file");
    }
  }
  std::uint64_t big_endian(std::size_t n) {
    need(n);
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
      v = (v << 8) | *p_++;
    return v;
  }

  std::size_t array_header() {
    const std::uint8_t c = byte();
    if ((c & 0xF0) == 0x90)
      return c & 0x0F;
    if (c == 0xDC)
      return static_cast<std::size_t>(big_endian(2));
    if (c == 0xDD)
      return static_cast<std::size_t>(big_endian(4));
    throw ReflError("expected an array");
  }

  std::size_t map_header() {
    const std::uint8_t c = byte();
    if ((c & 0xF0) == 0x80)
      return c & 0x0F;
    if (c == 0xDE)
      return static_cast<std::size_t>(big_endian(2));
    if (c == 0xDF)
      return static_cast<std::size_t>(big_endian(4));
    throw ReflError("expected a map");
  }

  std::string text() {
    const std::uint8_t c = byte();
    std::size_t n = 0;
    if ((c & 0xE0) == 0xA0)
      n = c & 0x1F;
    else if (c == 0xD9 || c == 0xC4)
      n = static_cast<std::size_t>(big_endian(1));
    else if (c == 0xDA || c == 0xC5)
      n = static_cast<std::size_t>(big_endian(2));
    else if (c == 0xDB || c == 0xC6)
      n = static_cast<std::size_t>(big_endian(4));
    else
      throw ReflError("expected a string");
    need(n);
    std::string out(reinterpret_cast<const char *>(p_), n);
    p_ += n;
    return out;
  }

  std::int64_t integer() {
    const std::uint8_t c = byte();
    if (c <= 0x7F)
      return c;
    if (c >= 0xE0)
      return static_cast<std::int8_t>(c);
    switch (c) {
    case 0xCC:
      return static_cast<std::int64_t>(big_endian(1));
    case 0xCD:
      return static_cast<std::int64_t>(big_endian(2));
    case 0xCE:
      return static_cast<std::int64_t>(big_endian(4));
    case 0xCF:
      return static_cast<std::int64_t>(big_endian(8));
    case 0xD0:
      return static_cast<std::int8_t>(big_endian(1));
    case 0xD1:
      return static_cast<std::int16_t>(big_endian(2));
    case 0xD2:
      return static_cast<std::int32_t>(big_endian(4));
    case 0xD3:
      return static_cast<std::int64_t>(big_endian(8));
    default:
      throw ReflError("expected an integer");
    }
  }

  // Returns a view, not a copy: the blobs are most of the file.
  std::pair<const std::uint8_t *, std::size_t> blob() {
    const std::uint8_t c = byte();
    std::size_t n = 0;
    if (c == 0xC4)
      n = static_cast<std::size_t>(big_endian(1));
    else if (c == 0xC5)
      n = static_cast<std::size_t>(big_endian(2));
    else if (c == 0xC6)
      n = static_cast<std::size_t>(big_endian(4));
    else if ((c & 0xE0) == 0xA0)
      n = c & 0x1F;
    else if (c == 0xD9)
      n = static_cast<std::size_t>(big_endian(1));
    else if (c == 0xDA)
      n = static_cast<std::size_t>(big_endian(2));
    else if (c == 0xDB)
      n = static_cast<std::size_t>(big_endian(4));
    else
      throw ReflError("expected a binary payload");
    need(n);
    const std::uint8_t *start = p_;
    p_ += n;
    return {start, n};
  }

  // Step over any value, for the parts of a document we do not consume.
  void skip() {
    const std::uint8_t c = peek();
    if ((c & 0xF0) == 0x90 || c == 0xDC || c == 0xDD) {
      const std::size_t n = array_header();
      for (std::size_t i = 0; i < n; ++i)
        skip();
    } else if ((c & 0xF0) == 0x80 || c == 0xDE || c == 0xDF) {
      const std::size_t n = map_header();
      for (std::size_t i = 0; i < 2 * n; ++i)
        skip();
    } else if (c == 0xC0 || c == 0xC2 || c == 0xC3) {
      byte();
    } else if (c == 0xCA) {
      byte();
      big_endian(4);
    } else if (c == 0xCB) {
      byte();
      big_endian(8);
    } else if (c == 0xC4 || c == 0xC5 || c == 0xC6) {
      blob();
    } else if ((c & 0xE0) == 0xA0 || c == 0xD9 || c == 0xDA || c == 0xDB) {
      text();
    } else {
      integer();
    }
  }

private:
  const std::uint8_t *p_;
  const std::uint8_t *end_;
};

// Used only on a big-endian machine, where the file's bytes are not the
// doubles.
[[maybe_unused]] double read_double_le(const std::uint8_t *p) {
  std::uint64_t bits = 0;
  for (int i = 7; i >= 0; --i)
    bits = (bits << 8) | p[static_cast<std::size_t>(i)];
  double out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

std::int64_t read_int_le(const std::uint8_t *p, std::size_t bytes,
                         bool is_unsigned) {
  std::uint64_t bits = 0;
  for (std::size_t i = bytes; i-- > 0;)
    bits = (bits << 8) | p[i];
  if (is_unsigned)
    return static_cast<std::int64_t>(bits);
  switch (bytes) {
  case 1:
    return static_cast<std::int8_t>(bits);
  case 2:
    return static_cast<std::int16_t>(bits);
  case 4:
    return static_cast<std::int32_t>(bits);
  default:
    return static_cast<std::int64_t>(bits);
  }
}

// ------------------------------------------------------------------ writing

void put(std::string &out, std::uint8_t b) {
  out.push_back(static_cast<char>(b));
}

void put_big_endian(std::string &out, std::uint64_t v, std::size_t n) {
  for (std::size_t i = n; i-- > 0;)
    put(out, static_cast<std::uint8_t>(v >> (8 * i)));
}

void put_text(std::string &out, const std::string &s) {
  if (s.size() < 32) {
    put(out, static_cast<std::uint8_t>(0xA0 | s.size()));
  } else if (s.size() < 256) {
    put(out, 0xD9);
    put_big_endian(out, s.size(), 1);
  } else {
    put(out, 0xDA);
    put_big_endian(out, s.size(), 2);
  }
  out += s;
}

void put_uint(std::string &out, std::uint64_t v) {
  if (v < 128) {
    put(out, static_cast<std::uint8_t>(v));
  } else if (v < 256) {
    put(out, 0xCC);
    put_big_endian(out, v, 1);
  } else if (v < 65536) {
    put(out, 0xCD);
    put_big_endian(out, v, 2);
  } else if (v < 4294967296ULL) {
    put(out, 0xCE);
    put_big_endian(out, v, 4);
  } else {
    put(out, 0xCF);
    put_big_endian(out, v, 8);
  }
}

//: msgpack's largest binary type is bin32, so four gigabytes is the most a
//: single column can hold. Above that the length silently wrapped and the
//: bytes were written anyway: a 4.87 GB table declared its shoebox column as
//: 529638180 bytes, which is the true size less exactly 2^32, and everything
//: past that point was unreachable. The file looked fine and was not.
//:
//: There is no bin64 to reach for, so this cannot be fixed by writing it
//: differently -- the column has to be smaller. Shoeboxes are the only thing
//: that gets near it, at about 34 kB a reflection.
constexpr std::uint64_t kBlobLimit = 0xFFFFFFFFull;

} // namespace

void check_blob_size(std::uint64_t size) {
  if (size > kBlobLimit) {
    throw ReflError(
        "a column of " + std::to_string(size) +
        " bytes cannot be written: msgpack's binary type describes at most 4 "
        "GB, and there is no larger one. With shoeboxes at about 34 kB a "
        "reflection that is around 126000 of them, so write a slice of the "
        "scan instead.");
  }
}

namespace {

//: A binary's header, for a binary of `size` bytes that follows it.
void put_blob_header(std::string &out, std::size_t size) {
  check_blob_size(size);
  if (size < 256) {
    put(out, 0xC4);
    put_big_endian(out, size, 1);
  } else if (size < 65536) {
    put(out, 0xC5);
    put_big_endian(out, size, 2);
  } else {
    put(out, 0xC6);
    put_big_endian(out, size, 4);
  }
}

//: A column's values as the file has them, little-endian, straight onto `out`.
//: On a little-endian machine -- x86-64 and Apple silicon both -- a double's
//: bytes in memory are the file's, so a column of them is one copy; an integer
//: narrower than the int64 it is kept in is one copy of its low bytes. It was a
//: push_back a byte, each checking the string's capacity, into a buffer then
//: copied again onto `out`: 0.88 s for a table of 630000 rows. Anywhere else
//: the bytes are taken out by shifting, as they were.
[[maybe_unused]] void put_reals(std::string &out, const double *values,
                                std::size_t count) {
  if constexpr (std::endian::native == std::endian::little) {
    // Appended, not resized and then copied into: a resize zero-fills first.
    out.append(reinterpret_cast<const char *>(values), count * sizeof(double));
  } else {
    const std::size_t at = out.size();
    out.resize(at + count * sizeof(double));
    char *to = out.data() + at;
    for (std::size_t i = 0; i < count; ++i) {
      std::uint64_t bits;
      std::memcpy(&bits, &values[i], sizeof(bits));
      for (std::size_t b = 0; b < 8; ++b)
        to[i * 8 + b] = static_cast<char>((bits >> (8 * b)) & 0xFF);
    }
  }
}

void put_ints(std::string &out, const std::vector<std::int64_t> &values,
              std::size_t count, std::size_t bytes) {
  const std::size_t at = out.size();
  out.resize(at + count * bytes);
  char *to = out.data() + at;
  if constexpr (std::endian::native == std::endian::little) {
    if (bytes == sizeof(std::int64_t)) {
      if (count > 0)
        std::memcpy(to, values.data(), count * bytes);
    } else {
      for (std::size_t i = 0; i < count; ++i)
        std::memcpy(to + i * bytes, &values[i], bytes); // the low bytes
    }
  } else {
    for (std::size_t i = 0; i < count; ++i) {
      const auto bits = static_cast<std::uint64_t>(values[i]);
      for (std::size_t b = 0; b < bytes; ++b)
        to[i * bytes + b] = static_cast<char>((bits >> (8 * b)) & 0xFF);
    }
  }
}

void put_map_header(std::string &out, std::size_t n) {
  if (n < 16) {
    put(out, static_cast<std::uint8_t>(0x80 | n));
  } else if (n < 65536) {
    put(out, 0xDE);
    put_big_endian(out, n, 2);
  } else {
    put(out, 0xDF);
    put_big_endian(out, n, 4);
  }
}

} // namespace

const Column &Table::at(const std::string &name) const {
  auto it = columns_.find(name);
  if (it == columns_.end()) {
    std::ostringstream have;
    for (const auto &c : columns_)
      have << (have.tellp() ? ", " : "") << c.first;
    throw ReflError("no column '" + name + "'; have " + have.str());
  }
  return it->second;
}

std::vector<std::string> Table::names() const {
  std::vector<std::string> out;
  for (const auto &c : columns_)
    out.push_back(c.first);
  return out;
}

Column &Table::real_column(const std::string &name, const std::string &type,
                           std::size_t width) {
  Column c;
  c.type = type;
  c.width = width;
  c.integral = false;
  c.reals.assign(nrows * width, 0.0);
  columns_[name] = std::move(c);
  return columns_[name];
}

Column &Table::int_column(const std::string &name, const std::string &type,
                          std::size_t width) {
  Column c;
  c.type = type;
  c.width = width;
  c.integral = true;
  c.ints.assign(nrows * width, 0);
  columns_[name] = std::move(c);
  return columns_[name];
}

bool has_prediction(const Table &table, std::size_t row) {
  if (!table.has("miller_index") || !table.has("xyzcal.px"))
    return false;
  const Column &miller = table.at("miller_index");
  if (!miller.integer(row, 0) && !miller.integer(row, 1) &&
      !miller.integer(row, 2)) {
    return false;
  }
  const Column &cal = table.at("xyzcal.px");
  bool written = false;
  for (std::size_t k = 0; k < 3; ++k) {
    const double v = std::abs(cal.real(row, k));
    if (v > 0.0 && v < 1e-30)
      return false; // never written
    if (v != 0.0)
      written = true;
  }
  return written;
}

Column &Table::modify_int_column(const std::string &name,
                                 const std::string &type, std::size_t width) {
  auto it = columns_.find(name);
  if (it != columns_.end() && it->second.integral &&
      it->second.width == width) {
    return it->second;
  }
  return int_column(name, type, width);
}

void Table::validate() const {
  for (const auto &entry : columns_) {
    if (entry.second.rows() != nrows) {
      throw ReflError("column '" + entry.first + "' has " +
                      std::to_string(entry.second.rows()) +
                      " rows, table has " + std::to_string(nrows));
    }
  }
}

Table read_reflections(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw ReflError("cannot open " + path);
  // Size, resize, one read. The obvious
  //
  //   std::string raw((std::istreambuf_iterator<char>(in)), {});
  //
  // goes through the stream a character at a time and regrows the string as it
  // goes: on a hundred megabyte table that is 0.25 seconds against 0.05, and
  // it was a third of what mxi_refine spent on a scan-varying refinement.
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0)
    throw ReflError("cannot size " + path);
  in.seekg(0, std::ios::beg);
  std::string raw(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    in.read(raw.data(), size);
    if (in.gcount() != size) {
      throw ReflError("short read on " + path + ": " +
                      std::to_string(in.gcount()) + " of " +
                      std::to_string(size) + " bytes");
    }
  }

  Reader r(reinterpret_cast<const std::uint8_t *>(raw.data()), raw.size());
  const std::size_t top = r.array_header();
  if (top != 2 && top != 3) {
    throw ReflError("not a reflection table: top level is an array of " +
                    std::to_string(top));
  }
  const std::string tag = r.text();
  if (tag != kTag)
    throw ReflError("unexpected tag '" + tag + "'");

  Table table;
  if (top == 3)
    table.version = static_cast<int>(r.integer());

  const std::size_t entries = r.map_header();
  bool seen_data = false;
  // The body map is walked rather than indexed, because the key order is not
  // guaranteed and nrows must be known before the columns are decoded.
  struct Pending {
    std::string name, type;
    const std::uint8_t *data;
    std::size_t bytes;
    std::size_t declared;
  };
  std::vector<Pending> pending;

  for (std::size_t i = 0; i < entries; ++i) {
    const std::string key = r.text();
    if (key == "nrows") {
      table.nrows = static_cast<std::size_t>(r.integer());
    } else if (key == "identifiers" || key == "experiment_identifiers") {
      const std::size_t n = r.map_header();
      for (std::size_t k = 0; k < n; ++k) {
        const auto id = static_cast<std::size_t>(r.integer());
        table.identifiers[id] = r.text();
      }
    } else if (key == "data") {
      seen_data = true;
      const std::size_t n = r.map_header();
      for (std::size_t k = 0; k < n; ++k) {
        const std::string name = r.text();
        if (r.array_header() != 2)
          throw ReflError("column is not [type, payload]");
        const std::string type = r.text();
        std::size_t declared = 0;
        if (r.peek() == 0x92) {
          r.array_header();
          declared = static_cast<std::size_t>(r.integer());
        }
        const auto payload = r.blob();
        pending.push_back(
            {name, type, payload.first, payload.second, declared});
      }
    } else {
      r.skip();
    }
  }
  if (!seen_data)
    throw ReflError("no 'data' map in the reflection table");

  for (const Pending &p : pending) {
    auto info = type_table().find(p.type);
    if (info == type_table().end()) {
      // Not decoded, but kept: the bytes are correct for this table and will
      // still be correct when it is written back, so long as no rows have been
      // removed. That is checked at write time.
      Table::Opaque keep;
      keep.type = p.type;
      keep.bytes.assign(reinterpret_cast<const char *>(p.data), p.bytes);
      keep.rows = p.declared ? p.declared : table.nrows;
      table.set_opaque(p.name, std::move(keep));
      continue;
    }
    if (p.declared && p.declared != table.nrows) {
      throw ReflError("column '" + p.name + "' declares " +
                      std::to_string(p.declared) + " rows, table declares " +
                      std::to_string(table.nrows));
    }
    const std::size_t stride = info->second.element_bytes * info->second.width;
    if (p.bytes != stride * table.nrows) {
      throw ReflError("column '" + p.name + "' has " + std::to_string(p.bytes) +
                      " bytes, expected " +
                      std::to_string(stride * table.nrows));
    }
    Column c;
    c.type = p.type;
    c.width = info->second.width;
    c.integral = info->second.integral;
    const std::size_t count = table.nrows * c.width;
    if (c.integral) {
      c.ints.resize(count);
      const std::size_t bytes = info->second.element_bytes;
      const bool is_unsigned = info->second.is_unsigned;
      // On a little-endian machine each value is one load of its own width,
      // widened as read_int_le widens it -- sign-extended, or not if unsigned.
      // A byte at a time, and a switch a value, anywhere else.
      if constexpr (std::endian::native == std::endian::little) {
        const auto widen = [&](auto narrow) {
          using N = decltype(narrow);
          using U = std::make_unsigned_t<N>;
          for (std::size_t i = 0; i < count; ++i) {
            N v;
            std::memcpy(&v, p.data + i * sizeof(N), sizeof(N));
            c.ints[i] = is_unsigned
                            ? static_cast<std::int64_t>(static_cast<U>(v))
                            : static_cast<std::int64_t>(v);
          }
        };
        if (bytes == 1)
          widen(std::int8_t{});
        else if (bytes == 2)
          widen(std::int16_t{});
        else if (bytes == 4)
          widen(std::int32_t{});
        else if (bytes == 8)
          widen(std::int64_t{});
        else
          for (std::size_t i = 0; i < count; ++i)
            c.ints[i] = read_int_le(p.data + i * bytes, bytes, is_unsigned);
      } else {
        for (std::size_t i = 0; i < count; ++i)
          c.ints[i] = read_int_le(p.data + i * bytes, bytes, is_unsigned);
      }
    } else {
      c.reals.resize(count);
      if constexpr (std::endian::native == std::endian::little) {
        // The file's bytes are the doubles.
        if (count > 0)
          std::memcpy(c.reals.data(), p.data, count * sizeof(double));
      } else {
        for (std::size_t i = 0; i < count; ++i)
          c.reals[i] = read_double_le(p.data + i * 8);
      }
    }
    table.set(p.name, std::move(c));
  }
  table.validate();
  return table;
}

void write_reflections(const std::string &path, const Table &table) {
  table.validate();
  const std::vector<std::string> names = table.names();
  // Everything that can refuse the table is checked before the file is
  // opened, so a refused table leaves no file behind.
  for (const std::string &name : names) {
    const Column &c = table.at(name);
    if (type_table().find(c.type) == type_table().end()) {
      throw ReflError("cannot write column '" + name + "' of type '" + c.type +
                      "'");
    }
  }
  // Opaque columns are written back only while they still describe this table.
  // A shoebox whose row count no longer matches is exactly the silent
  // corruption that dropping them was meant to avoid.
  std::vector<std::string> opaque_names;
  for (const auto &entry : table.opaque()) {
    if (entry.second.rows != table.nrows) {
      // Refused rather than dropped. Dropping a shoebox is what produced a
      // 20 MB table out of an 84 MB one and an integration that would not
      // start; doing it silently a second time, for a better reason, would be
      // no better. Whoever changed the row count has to say what should happen
      // to a column this package cannot subset.
      throw ReflError(
          "column '" + entry.first + "' of type '" + entry.second.type +
          "' was read with " + std::to_string(entry.second.rows) +
          " rows and the table now has " + std::to_string(table.nrows) +
          "; this package cannot subset that type, so it cannot be written "
          "back. Remove it deliberately if that is what you want.");
    }
    check_blob_size(entry.second.bytes.size());
    opaque_names.push_back(entry.first);
  }

  // Straight to the file, a column at a time: the headers through a small
  // buffer, a column of doubles from its own storage, an opaque column from
  // where it already is. The whole file was built in one string first -- 261
  // MB for 630000 rows, every page of it touched on allocation and copied again
  // on the way out -- which was most of what writing cost once the bytes were
  // no longer appended one at a time.
  std::ofstream file(path, std::ios::binary);
  if (!file)
    throw ReflError("cannot write " + path);
  std::string out;
  const auto flush = [&]() {
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    out.clear();
  };
  const auto raw = [&](const char *data, std::size_t size) {
    flush();
    if (size > 0)
      file.write(data, static_cast<std::streamsize>(size));
  };

  put(out, 0x93);
  put_text(out, kTag);
  put_uint(out, static_cast<std::uint64_t>(table.version));
  put_map_header(out, 3);

  put_text(out, "identifiers");
  put_map_header(out, table.identifiers.size());
  for (const auto &entry : table.identifiers) {
    put_uint(out, entry.first);
    put_text(out, entry.second);
  }

  put_text(out, "nrows");
  put_uint(out, table.nrows);

  put_text(out, "data");
  put_map_header(out, names.size() + opaque_names.size());
  for (const std::string &name : names) {
    const Column &c = table.at(name);
    const auto info = type_table().find(c.type);
    put_text(out, name);
    put(out, 0x92);
    put_text(out, c.type);
    put(out, 0x92);
    put_uint(out, table.nrows);
    const std::size_t count = table.nrows * c.width;
    if (c.integral) {
      put_blob_header(out, count * info->second.element_bytes);
      put_ints(out, c.ints, count, info->second.element_bytes);
    } else {
      put_blob_header(out, count * sizeof(double));
      if constexpr (std::endian::native == std::endian::little) {
        raw(reinterpret_cast<const char *>(c.reals.data()),
            count * sizeof(double));
      } else {
        put_reals(out, c.reals.data(), count);
      }
    }
    flush();
  }

  for (const std::string &name : opaque_names) {
    const Table::Opaque &keep = table.opaque().at(name);
    put_text(out, name);
    put(out, 0x92);
    put_text(out, keep.type);
    put(out, 0x92);
    put_uint(out, table.nrows);
    put_blob_header(out, keep.bytes.size());
    raw(keep.bytes.data(), keep.bytes.size());
  }
  flush();
  file.close();
  if (!file)
    throw ReflError("error writing " + path);
}

Table select_rows(const Table &table, const std::vector<std::size_t> &rows) {
  Table out;
  out.identifiers = table.identifiers;
  out.version = table.version;
  out.nrows = rows.size();
  for (const std::string &name : table.names()) {
    const Column &from = table.at(name);
    Column to;
    to.type = from.type;
    to.width = from.width;
    to.integral = from.integral;
    const std::size_t w = from.width ? from.width : 1;
    for (std::size_t r : rows) {
      if (r >= table.nrows)
        throw ReflError("select_rows: row " + std::to_string(r) +
                        " of a table of " + std::to_string(table.nrows));
      for (std::size_t k = 0; k < w; ++k) {
        if (from.integral)
          to.ints.push_back(from.ints[r * w + k]);
        else
          to.reals.push_back(from.reals[r * w + k]);
      }
    }
    out.set(name, std::move(to));
  }
  return out;
}

Table concat_rows(const std::vector<Table> &tables) {
  Table out;
  if (tables.empty())
    return out;
  const std::vector<std::string> names = tables.front().names();
  out.version = tables.front().version;
  for (const Table &t : tables) {
    if (!t.opaque().empty())
      throw ReflError("concat_rows: the column " + t.opaque().begin()->first +
                      " is kept as bytes, and cannot be joined row by row");
    if (t.names() != names)
      throw ReflError("concat_rows: the tables do not have the same columns");
    for (const auto &[id, identifier] : t.identifiers)
      out.identifiers[id] = identifier;
    out.nrows += t.nrows;
  }
  for (const std::string &name : names) {
    const Column &first = tables.front().at(name);
    Column to;
    to.type = first.type;
    to.width = first.width;
    to.integral = first.integral;
    for (const Table &t : tables) {
      const Column &from = t.at(name);
      if (from.type != first.type || from.width != first.width ||
          from.integral != first.integral)
        throw ReflError("concat_rows: the column " + name +
                        " differs in type between the tables");
      if (from.integral)
        to.ints.insert(to.ints.end(), from.ints.begin(), from.ints.end());
      else
        to.reals.insert(to.reals.end(), from.reals.begin(), from.reals.end());
    }
    out.set(name, std::move(to));
  }
  return out;
}

} // namespace mxi

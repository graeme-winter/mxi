// Reflection tables written as HDF5: DIALS's own format, its default since
// dials/dials#3255 (2025), and dxtbx-h5's (graeme-winter/dxtbx-h5,
// docs/HANDOVER.md), which shares its columns. Read into the Table a msgpack
// table reads into, each column given the type name DIALS gives it in msgpack,
// so that everything downstream, writing included, is as it was.
//
// Where the tables are. DIALS (dials/util/table_as_hdf5_file.py): the root's
// file_type is "dials_processed_data" and file_version 1; the tables are the
// groups under the LAST group of /dials -- /dials/processing by default, but
// the writer takes the name -- in the order written, group_0, group_1 and on.
// dxtbx-h5: /reflections/0, /reflections/1 and on, which is preferred where
// present; its /dials, if any, is hard links to the same.
//
// Each table group has identifiers and experiment_ids attributes, parallel;
// each column is a dataset of one row a reflection -- (N, 2), (N, 3), (N, 6) or
// (N, 3, 3) for a vector, an index, a box or a matrix -- its type a dataset's
// dials_type attribute where there is one (dxtbx-h5), else inferred from shape
// and dtype; columns mxi has no type for (float, uint8, strings, frame-sliced
// shoeboxes) are left out. A shoebox column is a group: shoebox_data and
// shoebox_background float32, shoebox_mask uint8, all concatenated, z slowest,
// then y, then x; bbox and panel per reflection. Read into the shoebox column a
// msgpack table carries.
//
// DIALS compresses shoeboxes with LZ4 (HDF5 filter 32004), which HDF5 reads
// only with the plugin; without it the chunks are read raw and decompressed
// here, with the LZ4 the spot finder builds. Compiled only where HDF5 is found.

#include <hdf5.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef MXI_HAVE_LZ4
#include <lz4.h>
#endif

#include "refl.hh"
#include "shoebox.hh"

namespace mxi {

namespace {

struct Handle {
  hid_t id;
  herr_t (*close)(hid_t);
  Handle(hid_t i, herr_t (*c)(hid_t)) : id(i), close(c) {}
  ~Handle() {
    if (id >= 0)
      close(id);
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};

bool exists(hid_t file, const std::string &path) {
  std::size_t at = 1;
  while (at <= path.size()) {
    const std::size_t slash = path.find('/', at);
    const std::string part =
        path.substr(0, slash == std::string::npos ? path.size() : slash);
    if (H5Lexists(file, part.c_str(), H5P_DEFAULT) <= 0)
      return false;
    if (slash == std::string::npos)
      break;
    at = slash + 1;
  }
  return true;
}

std::string string_attribute_1(hid_t object, const char *name) {
  if (H5Aexists(object, name) <= 0)
    return "";
  Handle a(H5Aopen(object, name, H5P_DEFAULT), H5Aclose);
  Handle type(H5Aget_type(a.id), H5Tclose);
  if (H5Tget_class(type.id) != H5T_STRING)
    return "";
  if (H5Tis_variable_str(type.id) > 0) {
    // In the file's character set: h5py writes UTF-8, and HDF5 converts
    // between character sets no more than it reads one as the other.
    Handle memory(H5Tcopy(H5T_C_S1), H5Tclose);
    H5Tset_size(memory.id, H5T_VARIABLE);
    H5Tset_cset(memory.id, H5Tget_cset(type.id));
    char *value = nullptr;
    if (H5Aread(a.id, memory.id, &value) < 0 || !value)
      return "";
    std::string out(value);
    H5free_memory(value);
    return out;
  }
  std::vector<char> buffer(H5Tget_size(type.id) + 1, '\0');
  if (H5Aread(a.id, type.id, buffer.data()) < 0)
    return "";
  return std::string(buffer.data());
}

std::vector<std::int64_t> int_attribute(hid_t group, const char *name) {
  if (H5Aexists(group, name) <= 0)
    return {};
  Handle a(H5Aopen(group, name, H5P_DEFAULT), H5Aclose);
  Handle space(H5Aget_space(a.id), H5Sclose);
  const hssize_t n = H5Sget_simple_extent_npoints(space.id);
  std::vector<std::int64_t> out(static_cast<std::size_t>(n > 0 ? n : 0));
  if (!out.empty() && H5Aread(a.id, H5T_NATIVE_INT64, out.data()) < 0)
    throw ReflError(std::string("cannot read the attribute ") + name);
  return out;
}

std::vector<std::string> string_attribute(hid_t group, const char *name) {
  if (H5Aexists(group, name) <= 0)
    return {};
  Handle a(H5Aopen(group, name, H5P_DEFAULT), H5Aclose);
  Handle space(H5Aget_space(a.id), H5Sclose);
  const hssize_t n = H5Sget_simple_extent_npoints(space.id);
  if (n <= 0)
    return {};
  Handle file_type(H5Aget_type(a.id), H5Tclose);
  std::vector<std::string> out;
  if (H5Tis_variable_str(file_type.id) > 0) {
    Handle memory(H5Tcopy(H5T_C_S1), H5Tclose);
    H5Tset_size(memory.id, H5T_VARIABLE);
    H5Tset_cset(memory.id, H5Tget_cset(file_type.id));
    std::vector<char *> values(static_cast<std::size_t>(n), nullptr);
    if (H5Aread(a.id, memory.id, values.data()) < 0)
      throw ReflError(std::string("cannot read the attribute ") + name);
    for (char *v : values)
      out.emplace_back(v ? v : "");
    H5Dvlen_reclaim(memory.id, space.id, H5P_DEFAULT, values.data());
  } else {
    const std::size_t size = H5Tget_size(file_type.id);
    std::vector<char> buffer(size * static_cast<std::size_t>(n));
    Handle memory(H5Tcopy(file_type.id), H5Tclose);
    if (H5Aread(a.id, memory.id, buffer.data()) < 0)
      throw ReflError(std::string("cannot read the attribute ") + name);
    for (hssize_t i = 0; i < n; ++i) {
      std::string s(buffer.data() + i * static_cast<hssize_t>(size), size);
      out.push_back(s.substr(0, s.find('\0')));
    }
  }
  return out;
}

// A group's members, in the order written where the group tracks it (DIALS's
// writer and h5py's track_order), else in name order with any trailing number
// compared as a number, so that group_10 follows group_9.
std::vector<std::string> members(hid_t group) {
  H5G_info_t info;
  if (H5Gget_info(group, &info) < 0)
    return {};
  std::vector<std::string> out;
  bool by_creation = true;
  for (hsize_t k = 0; k < info.nlinks; ++k) {
    char name[1024];
    if (H5Lget_name_by_idx(group, ".", H5_INDEX_CRT_ORDER, H5_ITER_INC, k, name,
                           sizeof name, H5P_DEFAULT) < 0) {
      by_creation = false;
      break;
    }
    out.emplace_back(name);
  }
  if (by_creation)
    return out;
  out.clear();
  for (hsize_t k = 0; k < info.nlinks; ++k) {
    char name[1024];
    if (H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, k, name,
                           sizeof name, H5P_DEFAULT) >= 0)
      out.emplace_back(name);
  }
  const auto split = [](const std::string &s) {
    std::size_t i = s.size();
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9')
      --i;
    return std::make_pair(s.substr(0, i),
                          i < s.size() ? std::stoll(s.substr(i)) : -1LL);
  };
  std::sort(out.begin(), out.end(),
            [&](const std::string &a, const std::string &b) {
              return split(a) < split(b);
            });
  return out;
}

bool is_group(hid_t parent, const std::string &name) {
  H5O_info_t info;
#if H5_VERSION_GE(1, 12, 0)
  if (H5Oget_info_by_name3(parent, name.c_str(), &info, H5O_INFO_BASIC,
                           H5P_DEFAULT) < 0)
#else
  if (H5Oget_info_by_name2(parent, name.c_str(), &info, H5O_INFO_BASIC,
                           H5P_DEFAULT) < 0)
#endif
    return false;
  return info.type == H5O_TYPE_GROUP;
}

#ifdef MXI_HAVE_LZ4
std::uint64_t be(const std::uint8_t *p, int bytes) {
  std::uint64_t v = 0;
  for (int i = 0; i < bytes; ++i)
    v = (v << 8) | p[i];
  return v;
}

// HDF5's LZ4 filter, 32004: the uncompressed size in eight bytes and the block
// size in four, big-endian, then each block as four bytes of its compressed
// size and the bytes -- stored as they are where compressing did not help.
void lz4_chunk(const std::vector<std::uint8_t> &in,
               std::vector<std::uint8_t> *out) {
  if (in.size() < 12)
    throw ReflError("an LZ4 chunk shorter than its header");
  const std::uint64_t total = be(in.data(), 8);
  const std::uint64_t block = be(in.data() + 8, 4);
  out->assign(total, 0);
  std::size_t at = 12;
  std::uint64_t done = 0;
  while (done < total) {
    const std::uint64_t want =
        std::min<std::uint64_t>(block ? block : total, total - done);
    if (at + 4 > in.size())
      throw ReflError("an LZ4 chunk ends in a block's header");
    const std::uint64_t size = be(in.data() + at, 4);
    at += 4;
    if (at + size > in.size())
      throw ReflError("an LZ4 chunk ends in a block");
    if (size == want) {
      std::memcpy(out->data() + done, in.data() + at, size);
    } else {
      const int got =
          LZ4_decompress_safe(reinterpret_cast<const char *>(in.data() + at),
                              reinterpret_cast<char *>(out->data() + done),
                              static_cast<int>(size), static_cast<int>(want));
      if (got < 0 || static_cast<std::uint64_t>(got) != want)
        throw ReflError("an LZ4 block that does not decompress");
    }
    at += size;
    done += want;
  }
}

herr_t read_chunk(hid_t dataset, const hsize_t *offset, std::uint32_t *filters,
                  void *buffer, std::size_t capacity) {
#ifdef SPOTFINDER_H5DREAD_CHUNK_TAKES_SIZE
  std::size_t size = capacity;
  return H5Dread_chunk(dataset, H5P_DEFAULT, offset, filters, buffer, &size);
#else
  (void)capacity;
  return H5Dread_chunk(dataset, H5P_DEFAULT, offset, filters, buffer);
#endif
}

// A dataset compressed with LZ4 and no plugin to hand: its chunks read raw,
// decompressed here, placed, and converted to `memory`'s type.
bool read_lz4_by_chunks(hid_t data, hid_t memory, void *out) {
  Handle dcpl(H5Dget_create_plist(data), H5Pclose);
  bool lz4 = false;
  const int nfilters = H5Pget_nfilters(dcpl.id);
  for (int i = 0; i < nfilters; ++i) {
    unsigned flags = 0;
    std::size_t nelements = 0;
    unsigned values[8];
    unsigned config = 0;
    char name[64];
    nelements = 8;
    if (H5Pget_filter2(dcpl.id, static_cast<unsigned>(i), &flags, &nelements,
                       values, sizeof name, name, &config) == 32004)
      lz4 = true;
  }
  if (!lz4 || nfilters != 1)
    return false;
  Handle space(H5Dget_space(data), H5Sclose);
  const int rank = H5Sget_simple_extent_ndims(space.id);
  std::vector<hsize_t> dims(static_cast<std::size_t>(rank)),
      chunk(static_cast<std::size_t>(rank));
  H5Sget_simple_extent_dims(space.id, dims.data(), nullptr);
  if (H5Pget_chunk(dcpl.id, rank, chunk.data()) != rank)
    return false;
  Handle file_type(H5Dget_type(data), H5Tclose);
  const std::size_t element = H5Tget_size(file_type.id);
  if (rank > 2)
    return false; // shoeboxes' arrays are one or two dimensional
  std::size_t total = 1;
  for (int k = 0; k < rank; ++k)
    total *= dims[static_cast<std::size_t>(k)];
  // A row's width, and a chunk's: h5py chunks an (N, 6) bbox as (n, 2), across
  // the row, so a chunk is placed row by row at its own column.
  const std::size_t row = rank == 2 ? dims[1] : 1;
  const std::size_t chunk_row = rank == 2 ? chunk[1] : 1;
  const std::size_t memory_size = H5Tget_size(memory);
  std::vector<std::uint8_t> raw(total * std::max(element, memory_size));
  hsize_t n_chunks = 0;
  // The dataset's own dataspace, not H5S_ALL, which HDF5 1.10 refuses here.
  if (H5Dget_num_chunks(data, space.id, &n_chunks) < 0)
    return false;
  std::vector<std::uint8_t> packed, unpacked;
  for (hsize_t c = 0; c < n_chunks; ++c) {
    std::vector<hsize_t> offset(static_cast<std::size_t>(rank));
    unsigned filter_mask = 0;
    haddr_t address = 0;
    hsize_t size = 0;
    if (H5Dget_chunk_info(data, space.id, c, offset.data(), &filter_mask,
                          &address, &size) < 0)
      return false;
    packed.resize(size);
    std::uint32_t filters = 0;
    if (read_chunk(data, offset.data(), &filters, packed.data(), size) < 0)
      return false;
    if (filters & 1u)
      unpacked = packed; // the filter skipped for this chunk
    else
      lz4_chunk(packed, &unpacked);
    // A chunk decompresses to its whole extent, edges padded.
    if (unpacked.size() < chunk[0] * chunk_row * element)
      throw ReflError("an LZ4 chunk shorter than its extent");
    const std::size_t first = static_cast<std::size_t>(offset[0]);
    const std::size_t column =
        rank == 2 ? static_cast<std::size_t>(offset[1]) : 0;
    const std::size_t rows = std::min<std::size_t>(chunk[0], dims[0] - first);
    const std::size_t across = std::min<std::size_t>(chunk_row, row - column);
    for (std::size_t r = 0; r < rows; ++r)
      std::memcpy(raw.data() + ((first + r) * row + column) * element,
                  unpacked.data() + r * chunk_row * element, across * element);
  }
  if (H5Tconvert(file_type.id, memory, total, raw.data(), nullptr,
                 H5P_DEFAULT) < 0)
    return false;
  std::memcpy(out, raw.data(), total * memory_size);
  return true;
}
#endif

// A whole dataset into `out`, of `memory`'s type: by HDF5 where it can, which
// is every filter it has; else, for LZ4 without the plugin, chunk by chunk.
void read_all(hid_t data, hid_t memory, void *out, const std::string &what) {
  if (H5Dread(data, memory, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) >= 0)
    return;
#ifdef MXI_HAVE_LZ4
  if (read_lz4_by_chunks(data, memory, out))
    return;
#endif
  throw ReflError("cannot read " + what +
                  ": compressed with a filter neither HDF5 nor mxi has here");
}

std::size_t rows_of(hid_t data, std::size_t *width, int *rank_out = nullptr) {
  Handle space(H5Dget_space(data), H5Sclose);
  const int rank = H5Sget_simple_extent_ndims(space.id);
  if (rank < 1 || rank > 3)
    throw ReflError("a column that is not one row a reflection");
  hsize_t dims[3] = {0, 1, 1};
  H5Sget_simple_extent_dims(space.id, dims, nullptr);
  *width = static_cast<std::size_t>(dims[1] * (rank == 3 ? dims[2] : 1));
  if (rank == 1)
    *width = 1;
  if (rank_out)
    *rank_out = rank;
  return static_cast<std::size_t>(dims[0]);
}

// DIALS's msgpack name for a column: from dials_type where the writer gave it
// (dxtbx-h5), else from its dtype and shape. Empty for a column mxi has no
// type for, which is left out.
std::string type_name(hid_t data, std::size_t width, int rank) {
  const std::string given = string_attribute_1(data, "dials_type");
  if (!given.empty()) {
    static const std::map<std::string, std::string> names = {
        {"bool", "bool"},
        {"int", "int"},
        {"size_t", "std::size_t"},
        {"double", "double"},
        {"vec2_double", "vec2<double>"},
        {"vec3_double", "vec3<double>"},
        {"mat3_double", "mat3<double>"},
        {"miller_index", "cctbx::miller::index<>"},
        {"int6", "int6"}};
    const auto it = names.find(given);
    return it == names.end() ? std::string() : it->second;
  }
  Handle type(H5Dget_type(data), H5Tclose);
  const H5T_class_t cls = H5Tget_class(type.id);
  const std::size_t size = H5Tget_size(type.id);
  if (cls == H5T_FLOAT && size == 8) {
    if (width == 1)
      return "double";
    if (width == 2)
      return "vec2<double>";
    if (width == 3 && rank == 2)
      return "vec3<double>";
    if (width == 9)
      return "mat3<double>";
    return "";
  }
  if (cls == H5T_ENUM && width == 1 && size == 1)
    return "bool"; // h5py's bool
  if (cls == H5T_INTEGER) {
    const bool is_unsigned = H5Tget_sign(type.id) == H5T_SGN_NONE;
    if (width == 3 && rank == 2 && !is_unsigned)
      return "cctbx::miller::index<>"; // there is no vec3 of integers
    if (width == 6 && !is_unsigned)
      return "int6";
    if (width == 1 && size == 8 && is_unsigned)
      return "std::size_t";
    if (width == 1 && size == 4 && !is_unsigned)
      return "int";
  }
  return ""; // float32, uint8, strings and the rest: no type here
}

// A column's values, appended.
void read_column(hid_t data, const std::string &name, const std::string &kind,
                 std::size_t n, std::size_t width, Column *c) {
  c->type = kind;
  c->width = width;
  Handle type(H5Dget_type(data), H5Tclose);
  if (kind == "bool" && H5Tget_class(type.id) == H5T_ENUM) {
    // h5py's bool: an enumeration of one byte, which HDF5 will not convert to
    // an integer, so read as the bytes it is.
    Handle native(H5Tget_native_type(type.id, H5T_DIR_ASCEND), H5Tclose);
    std::vector<std::int8_t> values(n);
    if (n)
      read_all(data, native.id, values.data(), "the column " + name);
    c->integral = true;
    for (std::int8_t v : values)
      c->ints.push_back(v);
    return;
  }
  if (H5Tget_class(type.id) == H5T_FLOAT) {
    std::vector<double> values(n * width);
    if (n)
      read_all(data, H5T_NATIVE_DOUBLE, values.data(), "the column " + name);
    c->integral = false;
    c->reals.insert(c->reals.end(), values.begin(), values.end());
    return;
  }
  std::vector<std::int64_t> values(n * width);
  if (n)
    read_all(data, H5T_NATIVE_INT64, values.data(), "the column " + name);
  c->integral = true;
  c->ints.insert(c->ints.end(), values.begin(), values.end());
}

// A shoebox group's shoeboxes, appended.
void read_shoeboxes(hid_t group, std::vector<Shoebox> *out) {
  const auto open = [&](const char *name) {
    const hid_t d = H5Dopen2(group, name, H5P_DEFAULT);
    if (d < 0)
      throw ReflError(std::string("a shoebox group without ") + name);
    return d;
  };
  Handle bbox_d(open("bbox"), H5Dclose), panel_d(open("panel"), H5Dclose),
      data_d(open("shoebox_data"), H5Dclose),
      background_d(open("shoebox_background"), H5Dclose),
      mask_d(open("shoebox_mask"), H5Dclose);
  std::size_t width = 0;
  const std::size_t n = rows_of(bbox_d.id, &width);
  if (width != 6)
    throw ReflError("a shoebox group's bbox is not six a row");
  std::vector<std::int32_t> bbox(n * 6);
  std::vector<std::int64_t> panel(n);
  if (n) {
    read_all(bbox_d.id, H5T_NATIVE_INT32, bbox.data(), "the shoeboxes' bbox");
    read_all(panel_d.id, H5T_NATIVE_INT64, panel.data(),
             "the shoeboxes' panel");
  }
  std::size_t w = 0;
  const std::size_t pixels = rows_of(data_d.id, &w);
  std::vector<float> data(pixels), background(pixels);
  std::vector<std::uint8_t> mask(pixels);
  if (pixels) {
    read_all(data_d.id, H5T_NATIVE_FLOAT, data.data(), "shoebox_data");
    read_all(background_d.id, H5T_NATIVE_FLOAT, background.data(),
             "shoebox_background");
    read_all(mask_d.id, H5T_NATIVE_UINT8, mask.data(), "shoebox_mask");
  }
  std::size_t at = 0;
  for (std::size_t i = 0; i < n; ++i) {
    Shoebox box;
    box.panel = static_cast<std::int32_t>(panel[i]);
    std::copy(bbox.begin() + static_cast<long>(i * 6),
              bbox.begin() + static_cast<long>(i * 6 + 6), box.bbox);
    box.flag = 2;
    const std::size_t size = box.size();
    if (at + size > pixels)
      throw ReflError("the shoeboxes' pixels end before their boxes do");
    box.data.assign(data.begin() + static_cast<long>(at),
                    data.begin() + static_cast<long>(at + size));
    box.background.assign(background.begin() + static_cast<long>(at),
                          background.begin() + static_cast<long>(at + size));
    box.mask.assign(mask.begin() + static_cast<long>(at),
                    mask.begin() + static_cast<long>(at + size));
    at += size;
    out->push_back(std::move(box));
  }
  if (at != pixels)
    throw ReflError("the shoeboxes' pixels run past their boxes");
}

// One table group's columns appended to `columns`, gathered over every group
// and set on the table once at the end: Table::real_column and int_column
// replace a column with a zeroed one, which would lose the groups before.
void read_group(hid_t file, const std::string &path, Table *table,
                std::map<std::string, Column> *columns,
                std::vector<Shoebox> *shoeboxes, bool *has_shoeboxes,
                bool first) {
  Handle group(H5Gopen2(file, path.c_str(), H5P_DEFAULT), H5Gclose);
  if (group.id < 0)
    throw ReflError("cannot open " + path);
  const std::vector<std::int64_t> ids =
      int_attribute(group.id, "experiment_ids");
  const std::vector<std::string> identifiers =
      string_attribute(group.id, "identifiers");
  for (std::size_t i = 0; i < ids.size() && i < identifiers.size(); ++i)
    table->identifiers[static_cast<std::size_t>(ids[i])] = identifiers[i];

  std::size_t rows = 0;
  bool rows_known = false;
  const std::vector<std::int64_t> nrows = int_attribute(group.id, "nrows");
  if (!nrows.empty()) {
    rows = static_cast<std::size_t>(nrows[0]);
    rows_known = true;
  }
  for (const std::string &name : members(group.id)) {
    if (is_group(group.id, name)) {
      Handle sub(H5Gopen2(group.id, name.c_str(), H5P_DEFAULT), H5Gclose);
      if (name == "shoebox" &&
          H5Lexists(sub.id, "shoebox_data", H5P_DEFAULT) > 0) {
        if (!first && !*has_shoeboxes)
          throw ReflError("shoeboxes in one table group and not another");
        const std::size_t before = shoeboxes->size();
        read_shoeboxes(sub.id, shoeboxes);
        *has_shoeboxes = true;
        const std::size_t n = shoeboxes->size() - before;
        if (rows_known && n != rows)
          throw ReflError("the shoeboxes number " + std::to_string(n) +
                          " where the rows are " + std::to_string(rows));
        rows = n;
        rows_known = true;
      }
      continue; // frame-sliced shoeboxes and anything else: not read here
    }
    Handle data(H5Dopen2(group.id, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (data.id < 0)
      continue;
    std::size_t width = 0;
    int rank = 0;
    const std::size_t n = rows_of(data.id, &width, &rank);
    if (rows_known && n != rows)
      throw ReflError("the HDF5 column " + name + " has " + std::to_string(n) +
                      " rows where the others have " + std::to_string(rows));
    rows = n;
    rows_known = true;
    const std::string kind = type_name(data.id, width, rank);
    if (kind.empty())
      continue;
    if (!first && !columns->count(name))
      throw ReflError("the HDF5 column " + name +
                      " is in one table group and not the others");
    read_column(data.id, name, kind, n, width, &(*columns)[name]);
  }
  table->nrows += rows;
}

// The table groups, in order: dxtbx-h5's /reflections/N, else DIALS's groups
// under the last group of /dials.
std::vector<std::string> table_groups(hid_t file, const std::string &path) {
  std::vector<std::string> out;
  if (exists(file, "/reflections") && is_group(file, "/reflections")) {
    Handle g(H5Gopen2(file, "/reflections", H5P_DEFAULT), H5Gclose);
    for (const std::string &m : members(g.id))
      if (is_group(g.id, m))
        out.push_back("/reflections/" + m);
    return out;
  }
  if (exists(file, "/dials")) {
    const std::string type = string_attribute_1(file, "file_type");
    if (type != "dials_processed_data")
      throw ReflError(path + " has /dials but its file_type is '" + type +
                      "', not dials_processed_data");
    const std::vector<std::int64_t> version =
        int_attribute(file, "file_version");
    if (version.empty() || version[0] != 1)
      throw ReflError(path + "'s file_version is not 1, the one known here");
    Handle dials(H5Gopen2(file, "/dials", H5P_DEFAULT), H5Gclose);
    const std::vector<std::string> processes = members(dials.id);
    if (processes.empty())
      return out;
    // The last, the most recent, as DIALS's reader takes it.
    const std::string process = "/dials/" + processes.back();
    Handle p(H5Gopen2(file, process.c_str(), H5P_DEFAULT), H5Gclose);
    for (const std::string &m : members(p.id))
      if (is_group(p.id, m))
        out.push_back(process + "/" + m);
  }
  return out;
}

} // namespace

Table read_reflections_hdf5(const std::string &path) {
  H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
  Handle file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  if (file.id < 0)
    throw ReflError("cannot open " + path + " as HDF5");
  const std::vector<std::string> groups = table_groups(file.id, path);
  if (groups.empty())
    throw ReflError(path + " is HDF5 but holds no reflection table: neither "
                           "/reflections/N nor a group under /dials");
  Table table;
  std::map<std::string, Column> columns;
  std::vector<Shoebox> shoeboxes;
  bool has_shoeboxes = false;
  for (std::size_t g = 0; g < groups.size(); ++g)
    read_group(file.id, groups[g], &table, &columns, &shoeboxes, &has_shoeboxes,
               g == 0);
  for (auto &[name, column] : columns) {
    if (column.rows() != table.nrows)
      throw ReflError("the HDF5 column " + name +
                      " is missing from a table group, or short");
    table.set(name, std::move(column));
  }
  if (has_shoeboxes) {
    Table::Opaque column;
    column.type = "Shoebox<>";
    column.rows = shoeboxes.size();
    column.bytes = encode_shoeboxes(shoeboxes);
    table.set_opaque("shoebox", std::move(column));
  }
  return table;
}

} // namespace mxi

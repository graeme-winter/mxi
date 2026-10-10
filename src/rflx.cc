// .rflx in dxtbx-h5's layout: see rflx.hh, docs/rflx.md, and dxtbx-h5's
// docs/HANDOVER.md, whose section numbers the comments here cite.

#include "rflx.hh"

#include <hdf5.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "shoebox.hh"

namespace mxi {
namespace rflx {

namespace {

// HDF5 is not thread safe as built by most distributions: every call from here
// under one lock. The frame reader holds its own around its own calls, which
// run in phases these do not.
std::mutex &lock() {
  static std::mutex m;
  return m;
}

struct H {
  hid_t id;
  herr_t (*close)(hid_t);
  H(hid_t i, herr_t (*c)(hid_t)) : id(i), close(c) {}
  ~H() {
    if (id >= 0)
      close(id);
  }
  H(const H &) = delete;
  H &operator=(const H &) = delete;
  bool ok() const { return id >= 0; }
};

hid_t checked(hid_t id, const std::string &what) {
  if (id < 0)
    throw Error("HDF5: " + what);
  return id;
}

void check(herr_t status, const std::string &what) {
  if (status < 0)
    throw Error("HDF5: " + what);
}

// Silence HDF5's printed error stack while probing for things that may not be
// there; errors are reported here, by name, instead.
struct Quiet {
  H5E_auto2_t func = nullptr;
  void *data = nullptr;
  Quiet() {
    H5Eget_auto2(H5E_DEFAULT, &func, &data);
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
  }
  ~Quiet() { H5Eset_auto2(H5E_DEFAULT, func, data); }
};

// ------------------------------------------------------------------ types

hid_t utf8_string() {
  const hid_t t = H5Tcopy(H5T_C_S1);
  H5Tset_size(t, H5T_VARIABLE);
  H5Tset_cset(t, H5T_CSET_UTF8);
  return t;
}

// h5py's bool: an enumeration of int8, FALSE 0 and TRUE 1.
hid_t bool_type() {
  const hid_t t = H5Tenum_create(H5T_NATIVE_INT8);
  const std::int8_t no = 0, yes = 1;
  H5Tenum_insert(t, "FALSE", &no);
  H5Tenum_insert(t, "TRUE", &yes);
  return t;
}

// ------------------------------------------------------------------ names

// HANDOVER 2.2: % first, then /, then a leading underscore; the empty key %00.
std::string escape(const std::string &key) {
  if (key.empty())
    return "%00";
  std::string out;
  for (char c : key) {
    if (c == '%')
      out += "%25";
    else if (c == '/')
      out += "%2F";
    else
      out += c;
  }
  if (!out.empty() && out[0] == '_')
    out = "%5F" + out.substr(1);
  return out;
}

std::string unescape(const std::string &name) {
  if (name == "%00")
    return "";
  std::string out;
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '%' && i + 2 < name.size()) {
      const std::string code = name.substr(i + 1, 2);
      if (code == "25") {
        out += '%';
        i += 2;
        continue;
      }
      if (code == "2F") {
        out += '/';
        i += 2;
        continue;
      }
      if (code == "5F") {
        out += '_';
        i += 2;
        continue;
      }
    }
    out += name[i];
  }
  return out;
}

bool reserved(const std::string &name) {
  return name == "__type__" || name == "__keys__" || name == "__len__";
}

// ------------------------------------------------------------------ the tree

enum class Leaf { None, Bool, Int, Float, String, Mixed };

// A value's shape and leaf type if it is a rectangular array of one leaf type
// (a scalar is rank 0); Mixed if it is not.
struct Shape {
  std::vector<hsize_t> dims;
  Leaf leaf = Leaf::None;
};

Leaf leaf_of(const json::Value &v) {
  switch (v.type()) {
  case json::Value::Type::Bool:
    return Leaf::Bool;
  case json::Value::Type::Number:
    return v.is_integral() ? Leaf::Int : Leaf::Float;
  case json::Value::Type::String:
    return Leaf::String;
  default:
    return Leaf::Mixed; // null, objects: not an array's leaf
  }
}

void shape_of(const json::Value &v, std::size_t depth, Shape *s) {
  if (!v.is_array()) {
    const Leaf l = leaf_of(v);
    if (depth != s->dims.size())
      s->leaf = Leaf::Mixed;
    else if (s->leaf == Leaf::None)
      s->leaf = l;
    else if (s->leaf != l)
      s->leaf = Leaf::Mixed;
    return;
  }
  if (depth == s->dims.size()) {
    if (s->leaf != Leaf::None) { // a leaf seen at this depth already
      s->leaf = Leaf::Mixed;
      return;
    }
    s->dims.push_back(v.size());
  } else if (depth > s->dims.size() || s->dims[depth] != v.size()) {
    s->leaf = Leaf::Mixed;
    return;
  }
  for (const json::Value &e : v.as_array()) {
    shape_of(e, depth + 1, s);
    if (s->leaf == Leaf::Mixed)
      return;
  }
}

void flatten(const json::Value &v, std::vector<const json::Value *> *out) {
  if (v.is_array()) {
    for (const json::Value &e : v.as_array())
      flatten(e, out);
  } else {
    out->push_back(&v);
  }
}

std::size_t elements(const Shape &s) {
  std::size_t n = 1;
  for (hsize_t d : s.dims)
    n *= static_cast<std::size_t>(d);
  return n;
}

hid_t creation(const std::vector<hsize_t> &dims);

// Writes a rectangular value, or a scalar, or null, under `name` in `parent`,
// as dxtbx-h5's encoder decides: an attribute for 1 to 64 elements whose
// strings, if any, are 1024 characters or fewer -- HDF5's attributes are small
// -- else a dataset, compressed from 256 elements as the tables' columns are.
// An empty array is a dataset, as there.
void write_leaf(hid_t parent, const std::string &name, const json::Value &v,
                const Shape &s) {
  const std::size_t n = elements(s);
  bool as_attribute = v.is_null() || (n >= 1 && n <= 64);
  if (as_attribute && !v.is_null() && s.leaf == Leaf::String) {
    std::vector<const json::Value *> flat;
    flatten(v, &flat);
    for (const json::Value *e : flat)
      as_attribute = as_attribute && e->as_string().size() <= 1024;
  }
  hid_t space;
  if (v.is_null())
    space = H5Screate(H5S_NULL);
  else if (s.dims.empty())
    space = H5Screate(H5S_SCALAR);
  else
    space = H5Screate_simple(static_cast<int>(s.dims.size()), s.dims.data(),
                             nullptr);
  H sp(space, H5Sclose);
  // An empty array: shape (0,) of int64, its dtype not significant.
  const Leaf leaf =
      v.is_null() ? Leaf::Int : (s.leaf == Leaf::None ? Leaf::Int : s.leaf);
  hid_t type_id;
  switch (leaf) {
  case Leaf::Bool:
    type_id = bool_type();
    break;
  case Leaf::Int:
    type_id = H5Tcopy(H5T_NATIVE_INT64);
    break;
  case Leaf::Float:
    type_id = H5Tcopy(H5T_NATIVE_DOUBLE);
    break;
  case Leaf::String:
    type_id = utf8_string();
    break;
  default:
    throw Error("a value that is no array's leaf, under " + name);
  }
  H type(type_id, H5Tclose);
  hid_t target;
  if (as_attribute)
    target = checked(H5Acreate2(parent, name.c_str(), type.id, sp.id,
                                H5P_DEFAULT, H5P_DEFAULT),
                     "creating attribute " + name);
  else {
    H dcpl(s.dims.empty() ? H5Pcreate(H5P_DATASET_CREATE) : creation(s.dims),
           H5Pclose);
    target = checked(H5Dcreate2(parent, name.c_str(), type.id, sp.id,
                                H5P_DEFAULT, dcpl.id, H5P_DEFAULT),
                     "creating dataset " + name);
  }
  H t(target, as_attribute ? H5Aclose : H5Dclose);
  if (v.is_null() || n == 0)
    return;
  std::vector<const json::Value *> flat;
  flatten(v, &flat);
  const auto put = [&](const void *buffer) {
    if (as_attribute)
      check(H5Awrite(t.id, type.id, buffer), "writing attribute " + name);
    else
      check(H5Dwrite(t.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer),
            "writing dataset " + name);
  };
  switch (leaf) {
  case Leaf::Bool: {
    std::vector<std::int8_t> b;
    for (const json::Value *e : flat)
      b.push_back(e->as_bool() ? 1 : 0);
    put(b.data());
    break;
  }
  case Leaf::Int: {
    std::vector<std::int64_t> b;
    for (const json::Value *e : flat)
      b.push_back(static_cast<std::int64_t>(std::llround(e->as_number())));
    put(b.data());
    break;
  }
  case Leaf::Float: {
    std::vector<double> b;
    for (const json::Value *e : flat)
      b.push_back(e->as_number());
    put(b.data());
    break;
  }
  default: {
    std::vector<const char *> b;
    for (const json::Value *e : flat)
      b.push_back(e->as_string().c_str());
    put(b.data());
    break;
  }
  }
}

void write_strings_attribute(hid_t object, const char *name,
                             const std::vector<std::string> &values) {
  H type(utf8_string(), H5Tclose);
  const hsize_t n = values.size();
  H space(H5Screate_simple(1, &n, nullptr), H5Sclose);
  H a(checked(
          H5Acreate2(object, name, type.id, space.id, H5P_DEFAULT, H5P_DEFAULT),
          std::string("creating attribute ") + name),
      H5Aclose);
  std::vector<const char *> pointers;
  for (const std::string &s : values)
    pointers.push_back(s.c_str());
  if (n)
    check(H5Awrite(a.id, type.id, pointers.data()),
          std::string("writing attribute ") + name);
}

void write_string_attribute(hid_t object, const char *name,
                            const std::string &value) {
  H type(utf8_string(), H5Tclose);
  H space(H5Screate(H5S_SCALAR), H5Sclose);
  H a(checked(
          H5Acreate2(object, name, type.id, space.id, H5P_DEFAULT, H5P_DEFAULT),
          std::string("creating attribute ") + name),
      H5Aclose);
  const char *p = value.c_str();
  check(H5Awrite(a.id, type.id, &p), std::string("writing attribute ") + name);
}

void write_int64_attribute(hid_t object, const char *name, std::int64_t value) {
  H space(H5Screate(H5S_SCALAR), H5Sclose);
  H a(checked(H5Acreate2(object, name, H5T_NATIVE_INT64, space.id, H5P_DEFAULT,
                         H5P_DEFAULT),
              std::string("creating attribute ") + name),
      H5Aclose);
  check(H5Awrite(a.id, H5T_NATIVE_INT64, &value),
        std::string("writing attribute ") + name);
}

void write_value(hid_t parent, const std::string &name, const json::Value &v);

// A dict as a group: __type__ "dict", __keys__ its keys as they are.
void write_dict(hid_t group, const json::Object &object) {
  write_string_attribute(group, "__type__", "dict");
  std::vector<std::string> keys;
  for (const auto &[key, value] : object)
    keys.push_back(key);
  write_strings_attribute(group, "__keys__", keys);
  for (const auto &[key, value] : object)
    write_value(group, escape(key), value);
}

// A ragged or mixed list as a group: __type__ "list", __len__, children 0, 1...
void write_list(hid_t group, const json::Array &array) {
  write_string_attribute(group, "__type__", "list");
  write_int64_attribute(group, "__len__",
                        static_cast<std::int64_t>(array.size()));
  for (std::size_t i = 0; i < array.size(); ++i)
    write_value(group, std::to_string(i), array[i]);
}

void write_value(hid_t parent, const std::string &name, const json::Value &v) {
  if (v.is_object()) {
    H g(checked(H5Gcreate2(parent, name.c_str(), H5P_DEFAULT, H5P_DEFAULT,
                           H5P_DEFAULT),
                "creating group " + name),
        H5Gclose);
    write_dict(g.id, v.as_object());
    return;
  }
  if (v.is_array()) {
    Shape s;
    shape_of(v, 0, &s);
    if (v.size() == 0) {
      write_leaf(parent, name, v, Shape{{0}, Leaf::Int});
      return;
    }
    if (s.leaf != Leaf::Mixed && s.leaf != Leaf::None) {
      write_leaf(parent, name, v, s);
      return;
    }
    H g(checked(H5Gcreate2(parent, name.c_str(), H5P_DEFAULT, H5P_DEFAULT,
                           H5P_DEFAULT),
                "creating group " + name),
        H5Gclose);
    write_list(g.id, v.as_array());
    return;
  }
  write_leaf(parent, name, v, Shape{{}, leaf_of(v)});
}

// Reading: a value's type from its dtype and dataspace alone (HANDOVER 2.2).

json::Value element(const std::vector<std::uint8_t> &buffer, std::size_t i,
                    H5T_class_t cls, hid_t memory) {
  const std::size_t size = H5Tget_size(memory);
  const std::uint8_t *p = buffer.data() + i * size;
  if (cls == H5T_ENUM) {
    return json::Value(*reinterpret_cast<const std::int8_t *>(p) != 0);
  }
  if (cls == H5T_INTEGER) {
    std::int64_t v;
    std::memcpy(&v, p, sizeof v);
    return json::Value(static_cast<long long>(v));
  }
  if (cls == H5T_FLOAT) {
    double v;
    std::memcpy(&v, p, sizeof v);
    return json::Value(v);
  }
  // strings
  const char *s;
  std::memcpy(&s, p, sizeof s);
  return json::Value(std::string(s ? s : ""));
}

json::Value nest(const std::vector<std::uint8_t> &buffer, H5T_class_t cls,
                 hid_t memory, const std::vector<hsize_t> &dims,
                 std::size_t depth, std::size_t *at) {
  if (depth == dims.size())
    return element(buffer, (*at)++, cls, memory);
  json::Array out;
  for (hsize_t i = 0; i < dims[depth]; ++i)
    out.push_back(nest(buffer, cls, memory, dims, depth + 1, at));
  return json::Value(std::move(out));
}

// An attribute or dataset, by its id, read as JSON.
json::Value read_leaf(hid_t object, bool attribute) {
  H type(attribute ? H5Aget_type(object) : H5Dget_type(object), H5Tclose);
  H space(attribute ? H5Aget_space(object) : H5Dget_space(object), H5Sclose);
  if (H5Sget_simple_extent_type(space.id) == H5S_NULL)
    return json::Value();
  const H5T_class_t cls = H5Tget_class(type.id);
  const int rank = H5Sget_simple_extent_ndims(space.id);
  std::vector<hsize_t> dims(static_cast<std::size_t>(std::max(rank, 0)));
  if (rank > 0)
    H5Sget_simple_extent_dims(space.id, dims.data(), nullptr);
  std::size_t n = 1;
  for (hsize_t d : dims)
    n *= static_cast<std::size_t>(d);
  hid_t memory_id;
  if (cls == H5T_ENUM)
    memory_id = H5Tget_native_type(type.id, H5T_DIR_ASCEND);
  else if (cls == H5T_INTEGER)
    memory_id = H5Tcopy(H5T_NATIVE_INT64);
  else if (cls == H5T_FLOAT)
    memory_id = H5Tcopy(H5T_NATIVE_DOUBLE);
  else if (cls == H5T_STRING) {
    memory_id = utf8_string();
    H5Tset_cset(memory_id, H5Tget_cset(type.id));
  } else
    throw Error("a value of a type the format has no place for");
  H memory(memory_id, H5Tclose);
  std::vector<std::uint8_t> buffer(n * H5Tget_size(memory.id) + 1);
  if (cls == H5T_STRING && H5Tis_variable_str(type.id) <= 0) {
    // fixed-length strings: read as they are, then as std::strings
    const std::size_t size = H5Tget_size(type.id);
    std::vector<char> fixed(n * size);
    if (n)
      check(attribute ? H5Aread(object, type.id, fixed.data())
                      : H5Dread(object, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                fixed.data()),
            "reading strings");
    std::vector<std::string> strings;
    for (std::size_t i = 0; i < n; ++i) {
      std::string s(fixed.data() + i * size, size);
      strings.push_back(s.substr(0, s.find('\0')));
    }
    std::vector<const char *> pointers;
    for (const std::string &s : strings)
      pointers.push_back(s.c_str());
    std::vector<std::uint8_t> as_pointers(n * sizeof(const char *));
    std::memcpy(as_pointers.data(), pointers.data(), as_pointers.size());
    H pointer_type(utf8_string(), H5Tclose);
    std::size_t at = 0;
    return nest(as_pointers, cls, pointer_type.id, dims, 0, &at);
  }
  if (n)
    check(attribute ? H5Aread(object, memory.id, buffer.data())
                    : H5Dread(object, memory.id, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                              buffer.data()),
          "reading a value");
  std::size_t at = 0;
  json::Value out = nest(buffer, cls, memory.id, dims, 0, &at);
  if (cls == H5T_STRING && n) {
    H reclaim_space(attribute ? H5Aget_space(object) : H5Dget_space(object),
                    H5Sclose);
    H5Dvlen_reclaim(memory.id, reclaim_space.id, H5P_DEFAULT, buffer.data());
  }
  return out;
}

std::string string_attribute(hid_t object, const char *name) {
  if (H5Aexists(object, name) <= 0)
    return "";
  H a(H5Aopen(object, name, H5P_DEFAULT), H5Aclose);
  const json::Value v = read_leaf(a.id, true);
  return v.is_string() ? v.as_string() : "";
}

std::vector<std::string> strings_attribute(hid_t object, const char *name) {
  std::vector<std::string> out;
  if (H5Aexists(object, name) <= 0)
    return out;
  H a(H5Aopen(object, name, H5P_DEFAULT), H5Aclose);
  const json::Value v = read_leaf(a.id, true);
  if (v.is_array())
    for (const json::Value &e : v.as_array())
      out.push_back(e.as_string());
  else if (v.is_string())
    out.push_back(v.as_string());
  return out;
}

std::vector<std::int64_t> ints_attribute(hid_t object, const char *name) {
  std::vector<std::int64_t> out;
  if (H5Aexists(object, name) <= 0)
    return out;
  H a(H5Aopen(object, name, H5P_DEFAULT), H5Aclose);
  const json::Value v = read_leaf(a.id, true);
  if (v.is_array())
    for (const json::Value &e : v.as_array())
      out.push_back(static_cast<std::int64_t>(std::llround(e.as_number())));
  else if (v.is_number())
    out.push_back(static_cast<std::int64_t>(std::llround(v.as_number())));
  return out;
}

bool is_group(hid_t parent, const std::string &name) {
  if (H5Lexists(parent, name.c_str(), H5P_DEFAULT) <= 0)
    return false;
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

std::vector<std::string> links(hid_t group) {
  H5G_info_t info;
  std::vector<std::string> out;
  if (H5Gget_info(group, &info) < 0)
    return out;
  for (hsize_t k = 0; k < info.nlinks; ++k) {
    char name[1024];
    if (H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, k, name,
                           sizeof name, H5P_DEFAULT) >= 0)
      out.emplace_back(name);
  }
  return out;
}

std::vector<std::string> attribute_names(hid_t object) {
  std::vector<std::string> out;
  H5O_info_t info;
#if H5_VERSION_GE(1, 12, 0)
  if (H5Oget_info3(object, &info, H5O_INFO_NUM_ATTRS) < 0)
#else
  if (H5Oget_info2(object, &info, H5O_INFO_NUM_ATTRS) < 0)
#endif
    return out;
  for (hsize_t k = 0; k < info.num_attrs; ++k) {
    char name[1024];
    if (H5Aget_name_by_idx(object, ".", H5_INDEX_NAME, H5_ITER_INC, k, name,
                           sizeof name, H5P_DEFAULT) >= 0)
      out.emplace_back(name);
  }
  return out;
}

json::Value read_group(hid_t group);

// The value under `name` in `group`: an attribute, a dataset or a group.
json::Value read_member(hid_t group, const std::string &name) {
  if (H5Aexists(group, name.c_str()) > 0) {
    H a(H5Aopen(group, name.c_str(), H5P_DEFAULT), H5Aclose);
    return read_leaf(a.id, true);
  }
  if (H5Lexists(group, name.c_str(), H5P_DEFAULT) <= 0)
    throw Error("no value " + name);
  if (is_group(group, name)) {
    H g(H5Gopen2(group, name.c_str(), H5P_DEFAULT), H5Gclose);
    return read_group(g.id);
  }
  H d(checked(H5Dopen2(group, name.c_str(), H5P_DEFAULT), "opening " + name),
      H5Dclose);
  return read_leaf(d.id, false);
}

json::Value read_group(hid_t group) {
  const std::string type = string_attribute(group, "__type__");
  if (type == "list") {
    const std::vector<std::int64_t> len = ints_attribute(group, "__len__");
    const std::int64_t n = len.empty() ? 0 : len[0];
    json::Array out;
    for (std::int64_t i = 0; i < n; ++i)
      out.push_back(read_member(group, std::to_string(i)));
    return json::Value(std::move(out));
  }
  // A dict: keys from __keys__ if there, else every child and non-reserved
  // attribute, sorted, their names unescaped.
  json::Object out;
  if (H5Aexists(group, "__keys__") > 0) {
    for (const std::string &key : strings_attribute(group, "__keys__"))
      out[key] = read_member(group, escape(key));
  } else {
    std::vector<std::string> names = links(group);
    for (const std::string &a : attribute_names(group))
      if (!reserved(a))
        names.push_back(a);
    std::sort(names.begin(), names.end());
    for (const std::string &name : names)
      out[unescape(name)] = read_member(group, name);
  }
  return json::Value(std::move(out));
}

// ------------------------------------------------------------------ tables

// HANDOVER 3.3: a column's DIALS type name, its dials_type, its element type
// in the file, and whether its rows are (N, 3, 3).
struct ColumnKind {
  const char *dials; // the msgpack type name, as mxi's Table carries it
  const char *tag;   // dials_type
  bool integral;
  bool matrix;
};

const ColumnKind *kind_of_type(const std::string &type) {
  static const ColumnKind kinds[] = {
      {"bool", "bool", true, false},
      {"int", "int", true, false},
      {"std::size_t", "size_t", true, false},
      {"double", "double", false, false},
      {"vec2<double>", "vec2_double", false, false},
      {"vec3<double>", "vec3_double", false, false},
      {"mat3<double>", "mat3_double", false, true},
      {"cctbx::miller::index<>", "miller_index", true, false},
      {"int6", "int6", true, false},
      // Not in the HANDOVER's table: written as (N, 2) int32 with its type
      // named, so that it reads back as itself.
      {"tiny<int,2>", "tiny_int_2", true, false},
  };
  for (const ColumnKind &k : kinds)
    if (type == k.dials)
      return &k;
  return nullptr;
}

hid_t native_for(const std::string &type) {
  if (type == "bool")
    return bool_type();
  if (type == "std::size_t")
    return H5Tcopy(H5T_NATIVE_UINT64);
  if (type == "int" || type == "int6" || type == "cctbx::miller::index<>" ||
      type == "tiny<int,2>")
    return H5Tcopy(H5T_NATIVE_INT32);
  return H5Tcopy(H5T_NATIVE_DOUBLE);
}

// A dataset's creation properties: gzip at level 1 with the shuffle filter for
// 256 elements or more, as dxtbx-h5 writes (HANDOVER 3.3), chunked by rows.
hid_t creation(const std::vector<hsize_t> &dims) {
  std::size_t n = 1, row = 1;
  for (std::size_t k = 0; k < dims.size(); ++k) {
    n *= static_cast<std::size_t>(dims[k]);
    if (k > 0)
      row *= static_cast<std::size_t>(dims[k]);
  }
  const hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
  if (n >= 256) {
    std::vector<hsize_t> chunk = dims;
    const std::size_t rows =
        std::max<std::size_t>(1, 65536 / std::max<std::size_t>(row, 1));
    chunk[0] = std::min<hsize_t>(dims[0], rows);
    H5Pset_chunk(dcpl, static_cast<int>(chunk.size()), chunk.data());
    H5Pset_shuffle(dcpl);
    H5Pset_deflate(dcpl, 1);
  }
  return dcpl;
}

void write_dataset(hid_t group, const std::string &name,
                   const std::vector<hsize_t> &dims, hid_t file_type,
                   hid_t memory_type, const void *data) {
  H space(H5Screate_simple(static_cast<int>(dims.size()), dims.data(), nullptr),
          H5Sclose);
  H dcpl(creation(dims), H5Pclose);
  H d(checked(H5Dcreate2(group, name.c_str(), file_type, space.id, H5P_DEFAULT,
                         dcpl.id, H5P_DEFAULT),
              "creating column " + name),
      H5Dclose);
  bool empty = false;
  for (hsize_t x : dims)
    empty = empty || x == 0;
  if (!empty)
    check(H5Dwrite(d.id, memory_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data),
          "writing column " + name);
}

void write_column(hid_t group, const std::string &name, const Column &c,
                  std::size_t rows) {
  const ColumnKind *kind = kind_of_type(c.type);
  if (!kind)
    throw Error("the column " + name + " is of type " + c.type +
                ", which a .rflx has no place for");
  std::vector<hsize_t> dims{rows};
  if (kind->matrix) {
    dims.push_back(3);
    dims.push_back(3);
  } else if (c.width > 1) {
    dims.push_back(c.width);
  }
  H file_type(native_for(c.type), H5Tclose);
  if (c.type == "bool") {
    std::vector<std::int8_t> b;
    for (std::int64_t v : c.ints)
      b.push_back(v != 0 ? 1 : 0);
    write_dataset(group, name, dims, file_type.id, file_type.id, b.data());
  } else if (c.type == "std::size_t") {
    std::vector<std::uint64_t> b;
    for (std::int64_t v : c.ints)
      b.push_back(static_cast<std::uint64_t>(v));
    write_dataset(group, name, dims, file_type.id, H5T_NATIVE_UINT64, b.data());
  } else if (kind->integral) {
    std::vector<std::int32_t> b;
    for (std::int64_t v : c.ints)
      b.push_back(static_cast<std::int32_t>(v));
    write_dataset(group, name, dims, file_type.id, H5T_NATIVE_INT32, b.data());
  } else {
    write_dataset(group, name, dims, file_type.id, H5T_NATIVE_DOUBLE,
                  c.reals.data());
  }
  H d(checked(H5Dopen2(group, name.c_str(), H5P_DEFAULT), "reopening " + name),
      H5Dclose);
  write_string_attribute(d.id, "dials_type", kind->tag);
}

void write_shoeboxes(hid_t group, const Table::Opaque &opaque) {
  Table holder;
  holder.nrows = opaque.rows;
  holder.set_opaque("shoebox", opaque);
  const std::vector<Shoebox> boxes = decode_shoeboxes(holder);
  H g(checked(
          H5Gcreate2(group, "shoebox", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
          "creating the shoebox group"),
      H5Gclose);
  write_string_attribute(g.id, "dials_type", "shoebox");
  const hsize_t n = boxes.size();
  std::vector<std::int32_t> bbox;
  std::vector<std::uint64_t> panel;
  std::vector<float> data, background;
  std::vector<std::uint8_t> mask;
  for (const Shoebox &b : boxes) {
    bbox.insert(bbox.end(), b.bbox, b.bbox + 6);
    panel.push_back(static_cast<std::uint64_t>(b.panel));
    data.insert(data.end(), b.data.begin(), b.data.end());
    background.insert(background.end(), b.background.begin(),
                      b.background.end());
    mask.insert(mask.end(), b.mask.begin(), b.mask.end());
  }
  const hsize_t m = data.size();
  write_dataset(g.id, "bbox", {n, 6}, H5T_NATIVE_INT32, H5T_NATIVE_INT32,
                bbox.data());
  write_dataset(g.id, "panel", {n}, H5T_NATIVE_UINT64, H5T_NATIVE_UINT64,
                panel.data());
  write_dataset(g.id, "shoebox_data", {m}, H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT,
                data.data());
  write_dataset(g.id, "shoebox_background", {m}, H5T_NATIVE_FLOAT,
                H5T_NATIVE_FLOAT, background.data());
  write_dataset(g.id, "shoebox_mask", {m}, H5T_NATIVE_UINT8, H5T_NATIVE_UINT8,
                mask.data());
}

void write_table(hid_t file, const Table &table) {
  H reflections(checked(H5Gcreate2(file, "reflections", H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT),
                        "creating /reflections"),
                H5Gclose);
  H g(checked(H5Gcreate2(reflections.id, "0", H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT),
              "creating /reflections/0"),
      H5Gclose);
  std::vector<std::string> identifiers;
  std::vector<std::uint64_t> ids;
  for (const auto &[id, identifier] : table.identifiers) {
    ids.push_back(id);
    identifiers.push_back(identifier);
  }
  write_strings_attribute(g.id, "identifiers", identifiers);
  {
    const hsize_t n = ids.size();
    H space(H5Screate_simple(1, &n, nullptr), H5Sclose);
    H a(checked(H5Acreate2(g.id, "experiment_ids", H5T_NATIVE_UINT64, space.id,
                           H5P_DEFAULT, H5P_DEFAULT),
                "creating experiment_ids"),
        H5Aclose);
    if (n)
      check(H5Awrite(a.id, H5T_NATIVE_UINT64, ids.data()),
            "writing experiment_ids");
  }
  write_int64_attribute(g.id, "nrows", static_cast<std::int64_t>(table.nrows));
  for (const std::string &name : table.names())
    write_column(g.id, name, table.at(name), table.nrows);
  for (const auto &[name, opaque] : table.opaque()) {
    if (name == "shoebox")
      write_shoeboxes(g.id, opaque);
    else
      throw Error("the column " + name + " is of type " + opaque.type +
                  ", which a .rflx has no place for");
  }
}

// Reading a table group: each dataset's type by dials_type where given, else
// by dtype and shape (HANDOVER 3.3).
std::string type_of(hid_t d, std::size_t width, int rank) {
  const std::string tag = string_attribute(d, "dials_type");
  if (!tag.empty()) {
    static const std::map<std::string, std::string> names = {
        {"bool", "bool"},
        {"int", "int"},
        {"size_t", "std::size_t"},
        {"double", "double"},
        {"vec2_double", "vec2<double>"},
        {"vec3_double", "vec3<double>"},
        {"mat3_double", "mat3<double>"},
        {"miller_index", "cctbx::miller::index<>"},
        {"int6", "int6"},
        {"tiny_int_2", "tiny<int,2>"}};
    const auto it = names.find(tag);
    return it == names.end() ? std::string() : it->second;
  }
  H type(H5Dget_type(d), H5Tclose);
  const H5T_class_t cls = H5Tget_class(type.id);
  const std::size_t size = H5Tget_size(type.id);
  if (cls == H5T_ENUM && width == 1 && size == 1)
    return "bool";
  if (cls == H5T_FLOAT && size == 8) {
    if (width == 1)
      return "double";
    if (width == 2)
      return "vec2<double>";
    if (width == 3 && rank == 2)
      return "vec3<double>";
    if (width == 9 && rank == 3)
      return "mat3<double>";
  }
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
  return ""; // float32, uint8, strings: no type in mxi's tables
}

std::size_t rows_of(hid_t d, std::size_t *width, int *rank) {
  H space(H5Dget_space(d), H5Sclose);
  *rank = H5Sget_simple_extent_ndims(space.id);
  if (*rank < 1 || *rank > 3)
    throw Error("a column not one row a reflection");
  hsize_t dims[3] = {0, 1, 1};
  H5Sget_simple_extent_dims(space.id, dims, nullptr);
  *width = 1;
  for (int k = 1; k < *rank; ++k)
    *width *= static_cast<std::size_t>(dims[k]);
  return static_cast<std::size_t>(dims[0]);
}

void read_column(hid_t d, const std::string &type, std::size_t n,
                 std::size_t width, Column *c) {
  c->type = type;
  c->width = width;
  H file_type(H5Dget_type(d), H5Tclose);
  if (H5Tget_class(file_type.id) == H5T_ENUM) {
    H native(H5Tget_native_type(file_type.id, H5T_DIR_ASCEND), H5Tclose);
    std::vector<std::int8_t> b(n * width);
    if (!b.empty())
      check(H5Dread(d, native.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, b.data()),
            "reading a bool column");
    c->integral = true;
    for (std::int8_t v : b)
      c->ints.push_back(v);
    return;
  }
  if (H5Tget_class(file_type.id) == H5T_FLOAT) {
    std::vector<double> b(n * width);
    if (!b.empty())
      check(H5Dread(d, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    b.data()),
            "reading a column");
    c->integral = false;
    c->reals.insert(c->reals.end(), b.begin(), b.end());
    return;
  }
  std::vector<std::int64_t> b(n * width);
  if (!b.empty())
    check(H5Dread(d, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, b.data()),
          "reading a column");
  c->integral = true;
  c->ints.insert(c->ints.end(), b.begin(), b.end());
}

void read_shoeboxes(hid_t g, std::vector<Shoebox> *out) {
  const auto open = [&](const char *name) {
    return checked(H5Dopen2(g, name, H5P_DEFAULT),
                   std::string("a shoebox group without ") + name);
  };
  H bbox_d(open("bbox"), H5Dclose), panel_d(open("panel"), H5Dclose),
      data_d(open("shoebox_data"), H5Dclose),
      background_d(open("shoebox_background"), H5Dclose),
      mask_d(open("shoebox_mask"), H5Dclose);
  std::size_t width = 0;
  int rank = 0;
  const std::size_t n = rows_of(bbox_d.id, &width, &rank);
  if (width != 6)
    throw Error("a shoebox group's bbox is not six a row");
  std::vector<std::int32_t> bbox(n * 6);
  std::vector<std::int64_t> panel(n);
  if (n) {
    check(H5Dread(bbox_d.id, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  bbox.data()),
          "reading the shoeboxes' bbox");
    check(H5Dread(panel_d.id, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  panel.data()),
          "reading the shoeboxes' panel");
  }
  std::size_t w = 0;
  const std::size_t pixels = rows_of(data_d.id, &w, &rank);
  std::vector<float> data(pixels), background(pixels);
  std::vector<std::uint8_t> mask(pixels);
  if (pixels) {
    check(H5Dread(data_d.id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  data.data()),
          "reading shoebox_data");
    check(H5Dread(background_d.id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL,
                  H5P_DEFAULT, background.data()),
          "reading shoebox_background");
    check(H5Dread(mask_d.id, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  mask.data()),
          "reading shoebox_mask");
  }
  std::size_t at = 0;
  for (std::size_t i = 0; i < n; ++i) {
    Shoebox b;
    b.panel = static_cast<std::int32_t>(panel[i]);
    std::copy(bbox.begin() + static_cast<long>(i * 6),
              bbox.begin() + static_cast<long>(i * 6 + 6), b.bbox);
    b.flag = 2;
    const std::size_t size = b.size();
    if (at + size > pixels)
      throw Error("the shoeboxes' pixels end before their boxes do");
    b.data.assign(data.begin() + static_cast<long>(at),
                  data.begin() + static_cast<long>(at + size));
    b.background.assign(background.begin() + static_cast<long>(at),
                        background.begin() + static_cast<long>(at + size));
    b.mask.assign(mask.begin() + static_cast<long>(at),
                  mask.begin() + static_cast<long>(at + size));
    at += size;
    out->push_back(std::move(b));
  }
  if (at != pixels)
    throw Error("the shoeboxes' pixels run past their boxes");
}

std::time_t now() { return std::time(nullptr); }

} // namespace

bool is_hdf5(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  char signature[8] = {};
  in.read(signature, sizeof signature);
  return in.gcount() == 8 &&
         std::memcmp(signature, "\x89HDF\r\n\x1a\n", 8) == 0;
}

bool has_experiments(const std::string &path) {
  if (!is_hdf5(path))
    return false;
  const std::lock_guard<std::mutex> guard(lock());
  const Quiet quiet;
  H file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  return file.ok() && is_group(file.id, "experiments");
}

std::vector<std::string> as_pair(const std::vector<std::string> &inputs) {
  if (inputs.size() == 1 && is_hdf5(inputs[0]))
    return {inputs[0], inputs[0]};
  return inputs;
}

std::optional<json::Value> read_experiments(const std::string &path) {
  const std::lock_guard<std::mutex> guard(lock());
  const Quiet quiet;
  H file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  if (!file.ok())
    throw Error("cannot open " + path + " as HDF5");
  if (!is_group(file.id, "experiments"))
    return std::nullopt;
  H g(H5Gopen2(file.id, "experiments", H5P_DEFAULT), H5Gclose);
  return read_group(g.id);
}

std::optional<Table> read_reflections(const std::string &path,
                                      std::vector<std::string> *skipped) {
  const std::lock_guard<std::mutex> guard(lock());
  const Quiet quiet;
  H file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  if (!file.ok())
    throw Error("cannot open " + path + " as HDF5");
  if (!is_group(file.id, "reflections"))
    return std::nullopt;
  H top(H5Gopen2(file.id, "reflections", H5P_DEFAULT), H5Gclose);
  // The tables in order: 0, 1 and on, as numbers.
  std::vector<std::string> names;
  for (const std::string &n : links(top.id))
    if (is_group(top.id, n))
      names.push_back(n);
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b) {
              return a.size() != b.size() ? a.size() < b.size() : a < b;
            });
  if (names.empty())
    return std::nullopt;
  Table table;
  std::map<std::string, Column> columns;
  std::vector<Shoebox> shoeboxes;
  bool has_shoeboxes = false;
  for (std::size_t t = 0; t < names.size(); ++t) {
    H g(H5Gopen2(top.id, names[t].c_str(), H5P_DEFAULT), H5Gclose);
    const std::vector<std::int64_t> ids =
        ints_attribute(g.id, "experiment_ids");
    const std::vector<std::string> identifiers =
        strings_attribute(g.id, "identifiers");
    for (std::size_t i = 0; i < ids.size() && i < identifiers.size(); ++i)
      table.identifiers[static_cast<std::size_t>(ids[i])] = identifiers[i];
    std::size_t rows = 0;
    bool known = false;
    const std::vector<std::int64_t> nrows = ints_attribute(g.id, "nrows");
    if (!nrows.empty()) {
      rows = static_cast<std::size_t>(nrows[0]);
      known = true;
    }
    for (const std::string &name : links(g.id)) {
      if (is_group(g.id, name)) {
        H sub(H5Gopen2(g.id, name.c_str(), H5P_DEFAULT), H5Gclose);
        if (H5Lexists(sub.id, "shoebox_data", H5P_DEFAULT) <= 0)
          continue;
        if (name != "shoebox")
          throw Error("a shoebox column named " + name +
                      ": mxi's tables have one, named shoebox");
        if (t > 0 && !has_shoeboxes)
          throw Error("shoeboxes in one table and not another");
        const std::size_t before = shoeboxes.size();
        read_shoeboxes(sub.id, &shoeboxes);
        has_shoeboxes = true;
        const std::size_t n = shoeboxes.size() - before;
        if (known && n != rows)
          throw Error("the shoeboxes number " + std::to_string(n) +
                      " where the table's rows are " + std::to_string(rows));
        rows = n;
        known = true;
        continue;
      }
      H d(H5Dopen2(g.id, name.c_str(), H5P_DEFAULT), H5Dclose);
      if (!d.ok())
        continue;
      std::size_t width = 0;
      int rank = 0;
      const std::size_t n = rows_of(d.id, &width, &rank);
      if (known && n != rows)
        throw Error("the column " + name + " has " + std::to_string(n) +
                    " rows where the table has " + std::to_string(rows));
      rows = n;
      known = true;
      const std::string type = type_of(d.id, width, rank);
      if (type.empty()) { // a column mxi has no type for: left out
        if (skipped &&
            std::find(skipped->begin(), skipped->end(), name) == skipped->end())
          skipped->push_back(name);
        continue;
      }
      if (t > 0 && !columns.count(name))
        throw Error("the column " + name + " is in one table and not another");
      read_column(d.id, type, n, width, &columns[name]);
    }
    table.nrows += rows;
  }
  for (auto &[name, column] : columns) {
    if (column.rows() != table.nrows)
      throw Error("the column " + name + " is missing from a table, or short");
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

void write(const std::string &path, const json::Value *experiments,
           const Table *reflections, const std::string &creator) {
  const std::lock_guard<std::mutex> guard(lock());
  const Quiet quiet;
  const std::string partial = path + ".part";
  {
    H file(H5Fcreate(partial.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
           H5Fclose);
    if (!file.ok())
      throw Error("cannot create " + partial);
    write_string_attribute(file.id, "format", "dxtbx_h5");
    write_int64_attribute(file.id, "format_version", 1);
    write_string_attribute(file.id, "creator", creator);
    char stamp[32];
    const std::time_t t = now();
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    write_string_attribute(file.id, "timestamp", stamp);
    if (experiments) {
      if (!experiments->is_object())
        throw Error("an experiment list that is not a dictionary");
      H g(checked(H5Gcreate2(file.id, "experiments", H5P_DEFAULT, H5P_DEFAULT,
                             H5P_DEFAULT),
                  "creating /experiments"),
          H5Gclose);
      write_dict(g.id, experiments->as_object());
    }
    if (reflections)
      write_table(file.id, *reflections);
  }
  // Whole or not at all: the file appears under its name only once written.
  if (std::rename(partial.c_str(), path.c_str()) != 0)
    throw Error("cannot move " + partial + " to " + path);
}

} // namespace rflx
} // namespace mxi

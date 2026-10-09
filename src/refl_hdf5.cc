// Reflection tables written as HDF5, as DIALS writes them from 2025 on: each
// data set a group /dials/processing/group_N, its experiment ids and their
// identifiers as attributes, each column a dataset of one row a reflection --
// three or six values a row for a vector or a box. Read into the Table that a
// msgpack table reads into, each column given the type name DIALS gives it in
// msgpack, so that everything downstream, writing included, is as it was.
// Compiled only where HDF5 is found (CMakeLists.txt).

#include <hdf5.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "refl.hh"

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

// DIALS's msgpack name for a column of this element class, width and name.
std::string type_name(const std::string &name, H5T_class_t cls,
                      bool is_unsigned, std::size_t width) {
  if (name == "miller_index")
    return "cctbx::miller::index<>";
  if (cls == H5T_FLOAT) {
    if (width == 1)
      return "double";
    if (width == 2)
      return "vec2<double>";
    if (width == 3)
      return "vec3<double>";
    if (width == 9)
      return "mat3<double>";
  } else if (cls == H5T_ENUM && width == 1) {
    return "bool";
  } else if (cls == H5T_INTEGER) {
    if (width == 6)
      return "int6";
    if (width == 1)
      return is_unsigned ? "std::size_t" : "int";
  }
  throw ReflError("the HDF5 column " + name + " is of a kind not known here, " +
                  std::to_string(width) + " values a row");
}

// One group's columns appended to `columns`, gathered over every group and set
// on the table once at the end: Table::real_column and int_column replace a
// column with a zeroed one, which would lose the groups before.
void read_group(hid_t file, const std::string &path, Table *table,
                std::map<std::string, Column> *columns, bool first) {
  Handle group(H5Gopen2(file, path.c_str(), H5P_DEFAULT), H5Gclose);
  if (group.id < 0)
    throw ReflError("cannot open " + path);
  const std::vector<std::int64_t> ids =
      int_attribute(group.id, "experiment_ids");
  const std::vector<std::string> identifiers =
      string_attribute(group.id, "identifiers");
  for (std::size_t i = 0; i < ids.size() && i < identifiers.size(); ++i)
    table->identifiers[static_cast<std::size_t>(ids[i])] = identifiers[i];

  H5G_info_t info;
  if (H5Gget_info(group.id, &info) < 0)
    throw ReflError("cannot list " + path);
  std::size_t rows = 0;
  bool rows_known = false;
  for (hsize_t k = 0; k < info.nlinks; ++k) {
    char name[1024];
    if (H5Lget_name_by_idx(group.id, ".", H5_INDEX_NAME, H5_ITER_INC, k, name,
                           sizeof name, H5P_DEFAULT) < 0)
      continue;
    Handle data(H5Dopen2(group.id, name, H5P_DEFAULT), H5Dclose);
    if (data.id < 0)
      continue;
    Handle space(H5Dget_space(data.id), H5Sclose);
    hsize_t dims[2] = {0, 1};
    const int rank = H5Sget_simple_extent_ndims(space.id);
    if (rank < 1 || rank > 2)
      throw ReflError("the HDF5 column " + std::string(name) +
                      " is not one row a reflection");
    H5Sget_simple_extent_dims(space.id, dims, nullptr);
    const std::size_t n = static_cast<std::size_t>(dims[0]);
    const std::size_t width = rank == 2 ? static_cast<std::size_t>(dims[1]) : 1;
    if (rows_known && n != rows)
      throw ReflError("the HDF5 column " + std::string(name) + " has " +
                      std::to_string(n) + " rows where the others have " +
                      std::to_string(rows));
    rows = n;
    rows_known = true;
    Handle type(H5Dget_type(data.id), H5Tclose);
    const H5T_class_t cls = H5Tget_class(type.id);
    const bool is_unsigned =
        cls == H5T_INTEGER && H5Tget_sign(type.id) == H5T_SGN_NONE;
    const std::string column_name(name);
    const std::string kind = type_name(column_name, cls, is_unsigned, width);
    if (!first && !columns->count(column_name))
      throw ReflError("the HDF5 column " + column_name +
                      " is in one group and not the others");
    if (cls == H5T_FLOAT) {
      std::vector<double> values(n * width);
      if (n && H5Dread(data.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                       H5P_DEFAULT, values.data()) < 0)
        throw ReflError("cannot read the HDF5 column " + column_name);
      Column &c = (*columns)[column_name];
      c.type = kind;
      c.width = width;
      c.integral = false;
      c.reals.insert(c.reals.end(), values.begin(), values.end());
    } else if (cls == H5T_ENUM) {
      // h5py's bool: an enum of one byte, which HDF5 will not convert to an
      // integer, so read as the bytes it is.
      Handle native(H5Tget_native_type(type.id, H5T_DIR_ASCEND), H5Tclose);
      if (H5Tget_size(native.id) != 1)
        throw ReflError("the HDF5 column " + column_name +
                        " is an enumeration wider than a bool");
      std::vector<std::int8_t> values(n * width);
      if (n && H5Dread(data.id, native.id, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       values.data()) < 0)
        throw ReflError("cannot read the HDF5 column " + column_name);
      Column &c = (*columns)[column_name];
      c.type = kind;
      c.width = width;
      c.integral = true;
      for (std::int8_t v : values)
        c.ints.push_back(v);
    } else {
      std::vector<std::int64_t> values(n * width);
      if (n && H5Dread(data.id, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       values.data()) < 0)
        throw ReflError("cannot read the HDF5 column " + column_name);
      Column &c = (*columns)[column_name];
      c.type = kind;
      c.width = width;
      c.integral = true;
      c.ints.insert(c.ints.end(), values.begin(), values.end());
    }
  }
  table->nrows += rows;
}

} // namespace

Table read_reflections_hdf5(const std::string &path) {
  H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
  Handle file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
  if (file.id < 0)
    throw ReflError("cannot open " + path + " as HDF5");
  Table table;
  std::map<std::string, Column> columns;
  int groups = 0;
  for (int g = 0;; ++g) {
    const std::string group = "/dials/processing/group_" + std::to_string(g);
    if (!exists(file.id, group))
      break;
    read_group(file.id, group, &table, &columns, g == 0);
    ++groups;
  }
  for (auto &[name, column] : columns) {
    if (column.rows() != table.nrows)
      throw ReflError("the HDF5 column " + name +
                      " is missing from a group, or short");
    table.set(name, std::move(column));
  }
  if (groups == 0)
    throw ReflError(path +
                    " is HDF5 but holds no /dials/processing/group_0: not a "
                    "DIALS reflection table");
  return table;
}

} // namespace mxi

// .rflx, dxtbx-h5's layout (docs/rflx.md): the tree, the tables and the file,
// each to its own and back, as the HANDOVER's conformance checklist asks.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/json.hh"
#include "../src/refl.hh"
#include "../src/rflx.hh"
#include "../src/shoebox.hh"
#include "check.hh"

namespace mxi {

namespace {

std::string scratch(const char *name) {
  return std::string("/tmp/mxi_test_rflx_") + name;
}

std::string bytes_of(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

json::Value awkward_tree() {
  json::Object o;
  o["__id__"] = json::Value("ExperimentList"); // a leading underscore
  o["a/b"] = json::Value("slash");             // a slash
  o["100%"] = json::Value("percent");          // a percent
  o[""] = json::Value("the empty key");        // the empty key
  o["zero_int"] = json::Value(0);              // 0, not 0.0
  o["zero_float"] = json::Value(0.0);          // 0.0, not 0
  o["flag"] = json::Value(true);
  o["nothing"] = json::Value(); // null
  o["empty_list"] = json::Value(json::Array{});
  o["empty_dict"] = json::Value(json::Object{});
  o["matrix"] = json::Value(
      json::Array{json::Value(json::Array{json::Value(1.5), json::Value(2.5),
                                          json::Value(3.5)}),
                  json::Value(json::Array{json::Value(4.5), json::Value(5.5),
                                          json::Value(6.5)})});
  o["ragged"] = json::Value(
      json::Array{json::Value(json::Array{json::Value(1)}),
                  json::Value(json::Array{json::Value(2), json::Value(3)})});
  o["mixed"] = json::Value(json::Array{json::Value(0), json::Value(1.5)});
  o["dicts"] = json::Value(
      json::Array{json::Value(json::Object{{"k", json::Value(1)}}),
                  json::Value(json::Object{{"k", json::Value("two")}})});
  o["strings"] = json::Value(json::Array{json::Value("a"), json::Value("b")});
  json::Array long_list;
  for (int i = 0; i < 100; ++i)
    long_list.push_back(json::Value(i * 0.25)); // past 64: a dataset
  o["long"] = json::Value(long_list);
  o["nested"] = json::Value(json::Object{
      {"deeper", json::Value(json::Object{{"x", json::Value(-7)}})}});
  return json::Value(o);
}

Table every_column() {
  Table t;
  t.nrows = 3;
  t.identifiers[0] = "first";
  t.identifiers[1] = "second";
  t.int_column("id", "int", 1).ints = {0, 1, 1};
  t.int_column("flags", "std::size_t", 1).ints = {
      1, (std::int64_t(1) << 40) + 5, 2};
  t.int_column("entering", "bool", 1).ints = {1, 0, 1};
  t.real_column("d", "double", 1).reals = {2.5, 3.5, 4.5};
  t.real_column("v2", "vec2<double>", 2).reals = {1, 2, 3, 4, 5, 6};
  t.real_column("xyz", "vec3<double>", 3).reals = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  std::vector<double> m(27);
  for (std::size_t i = 0; i < m.size(); ++i)
    m[i] = 0.5 * static_cast<double>(i);
  t.real_column("matrix", "mat3<double>", 9).reals = m;
  t.int_column("miller_index", "cctbx::miller::index<>", 3).ints = {
      1, -2, 3, 0, 0, 1, -5, 4, 2};
  t.int_column("bbox", "int6", 6).ints = {0, 2, 0, 1, 0, 1, 3, 5, 3,
                                          4, 2, 3, 6, 7, 6, 8, 1, 2};
  t.int_column("pair", "tiny<int,2>", 2).ints = {1, 2, 3, 4, 5, 6};
  std::vector<Shoebox> boxes;
  for (int r = 0; r < 3; ++r) {
    Shoebox b;
    b.panel = r;
    const std::int32_t box[6] = {r, r + 2, 0, 1, 0, 1 + r};
    std::copy(box, box + 6, b.bbox);
    b.flag = 2;
    for (std::size_t k = 0; k < b.size(); ++k) {
      b.data.push_back(static_cast<float>(10 * r + k));
      b.mask.push_back(static_cast<std::uint8_t>(k % 3 + 1));
      b.background.push_back(static_cast<float>(0.5 * k));
    }
    boxes.push_back(std::move(b));
  }
  Table::Opaque column;
  column.type = "Shoebox<>";
  column.rows = boxes.size();
  column.bytes = encode_shoeboxes(boxes);
  t.set_opaque("shoebox", column);
  return t;
}

} // namespace

TEST(an_experiment_tree_comes_back_as_it_went_in) {
  // HANDOVER 2.2: keys escaped and their spelling kept, integers and floats
  // apart, null, empty lists and dicts, rectangular arrays as arrays, ragged
  // and mixed ones as groups, past 64 elements a dataset -- all back the same.
  const json::Value tree = awkward_tree();
  const std::string path = scratch("tree.rflx");
  rflx::write(path, &tree, nullptr, "mxi_tests");
  check::is_true(rflx::is_hdf5(path), "an HDF5 file");
  const std::optional<json::Value> back = rflx::read_experiments(path);
  check::is_true(back.has_value(), "the experiments read back");
  const std::string got = json::dump(*back), want = json::dump(tree);
  check::is_true(got == want, "the same tree:\n" + got + "\nagainst\n" + want);
  check::is_true((*back)["zero_int"].is_integral(), "0 an integer still");
  check::is_true(!(*back)["zero_float"].is_integral(), "0.0 a float still");
  check::is_true(!rflx::read_reflections(path).has_value(),
                 "and no reflections");
  std::remove(path.c_str());
}

TEST(a_table_of_every_column_comes_back_to_the_same_bytes) {
  // HANDOVER 3.3 and 3.4: each column's dtype, dials_type named, shoeboxes as
  // three flat arrays with a uint8 mask -- and the table back, written as
  // msgpack, the very bytes of the original written as msgpack.
  const Table t = every_column();
  const std::string path = scratch("table.rflx");
  rflx::write(path, nullptr, &t, "mxi_tests");
  const std::optional<Table> back = rflx::read_reflections(path);
  check::is_true(back.has_value(), "the reflections read back");
  check::is_true(!rflx::read_experiments(path).has_value(),
                 "and no experiments");
  write_reflections(scratch("a.refl"), t);
  write_reflections(scratch("b.refl"), *back);
  check::is_true(bytes_of(scratch("a.refl")) == bytes_of(scratch("b.refl")),
                 "the same msgpack bytes");
  for (const char *f : {"table.rflx", "a.refl", "b.refl"})
    std::remove(scratch(f).c_str());
}

TEST(a_column_the_format_has_no_place_for_is_refused_by_name) {
  Table t;
  t.nrows = 1;
  Table::Opaque odd;
  odd.type = "something<else>";
  odd.rows = 1;
  odd.bytes = "x";
  t.set_opaque("odd", odd);
  bool refused = false;
  try {
    rflx::write(scratch("odd.rflx"), nullptr, &t, "mxi_tests");
  } catch (const rflx::Error &e) {
    refused = std::string(e.what()).find("odd") != std::string::npos;
  }
  check::is_true(refused, "refused, naming the column");
  std::remove(scratch("odd.rflx").c_str());
  std::remove(scratch("odd.rflx.part").c_str());
}

} // namespace mxi

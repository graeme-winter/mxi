// .rflx: one HDF5 file holding an experiment list, a reflection table, or both,
// in dxtbx-h5's layout (github.com/graeme-winter/dxtbx-h5, docs/HANDOVER.md,
// Part 1), mxi's primary format (docs/rflx.md).
//
// The experiment list at /experiments is the JSON tree an .expt holds, mapped
// into HDF5 losslessly -- so it is read into the json::Value an .expt reads
// into, and experiments_from_json makes the models from it as it does from
// JSON. The reflections at /reflections/{n}, each a table, read into the Table
// a msgpack .refl reads into, every column's type its DIALS name. So a program
// given a .rflx works on exactly what it would have from the DIALS pair.

#ifndef MXI_RFLX_HH
#define MXI_RFLX_HH

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hh"
#include "refl.hh"

namespace mxi {
namespace rflx {

class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

//: Whether the file at `path` is HDF5, by its eight-byte signature -- which a
//: .rflx is and an .expt, JSON, and a .refl, msgpack, are not.
bool is_hdf5(const std::string &path);

//: The file's experiment list, as the JSON tree an .expt holds; none if the
//: file has no /experiments.
std::optional<json::Value> read_experiments(const std::string &path);

//: The file's reflections, every table group under /reflections joined in
//: order, each row's experiment its `id`; none if the file has none. Columns
//: of types mxi's tables have no place for -- float32, uint8, strings -- are
//: left out, and named in `skipped` if given.
std::optional<Table>
read_reflections(const std::string &path,
                 std::vector<std::string> *skipped = nullptr);

//: Whether the file is a .rflx holding an experiment list: HDF5 with
//: /experiments -- which an NXmx master, HDF5 with /entry, is not.
bool has_experiments(const std::string &path);

//: A program's inputs, the experiment list then the reflections: one .rflx
//: standing for both, as the same path twice; anything else as it is.
std::vector<std::string> as_pair(const std::vector<std::string> &inputs);

//: Writes a .rflx holding either or both, replacing any file at `path`.
//: `creator` names the program, for the root attribute of that name. Every
//: column of the table is written, or the write refused: a column of a type
//: the format has no place for is named in the error.
void write(const std::string &path, const json::Value *experiments,
           const Table *reflections, const std::string &creator);

} // namespace rflx
} // namespace mxi

#endif // MXI_RFLX_HH

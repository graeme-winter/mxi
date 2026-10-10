// Reading and writing DIALS experiment lists.
//
// The shapes accepted here are the ones real files use, which is not always
// the shape the documentation suggests:
//
//   * the goniometer is `axes` / `angles` / `scan_axis`, numbered from the
//     sample outwards, not `rotation_axis` with fixed and setting rotations;
//   * the scan's oscillation is a per-image array under `properties`, not a
//     `[start, width]` pair;
//   * the detector's `px_mm_strategy` decides whether the parallax correction
//     applies, and it is worth 1.6 pixels when it does.
//
// The older forms are accepted too where they cost nothing, because a reader
// that only understands the one file it was tested against is not much of a
// reader. What is NOT accepted is silence: a scan whose oscillation cannot be
// found is an error, not a zero, because two zeroes compare equal and a
// comparison that passes for want of data is worse than one that fails.

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "geometry.hh"
#include "json.hh"

namespace mxi {

class ExptError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

//: The Gaussian profile model, if the file carried one.
//:
//: Read rather than modelled: this package estimates its own and does not
//: write this block back changed, but an `integrated.expt` has one and a
//: caller that can use it should not have to be told the numbers on a command
//: line.
struct ProfileBlock {
  bool present = false;
  double sigma_b = 0.0; //: degrees
  double sigma_m = 0.0; //: degrees
  double n_sigma = 3.0;
};

struct ExperimentList {
  std::vector<Experiment> experiments;

  //: The document this was read from, kept whole.
  //:
  //: Writing an experiment list means writing back everything that was in it,
  //: not only the parts this package understands. An `imageset` block says
  //: where the images are and is the only link from the file to the data;
  //: dropping it makes the output unusable, and dropping it while still
  //: writing the experiment's reference to it makes dxtbx index an empty list
  //: and fail with
  //:
  //:     IndexError: list index out of range
  //:
  //: `profile`, `scaling_model` and `history` are in the same position: not
  //: modelled here, nobody's business to discard.
  //:
  //: So the whole document is kept and written back with the models this
  //: package does understand replaced. Empty for an experiment list built in
  //: memory rather than read from a file.
  json::Value source;

  //: The profile block of the first experiment, when there is one.
  ProfileBlock profile;

  //: Where the images are, from the `imageset` block: the template of the
  //: first imageset, verbatim. Empty if the file has none.
  //:
  //: A caller that has the experiment list should not have to be told the
  //: image file as well -- the .expt already says, and being given both is how
  //: they come to disagree.
  std::string image_template;

  std::size_t size() const { return experiments.size(); }
  bool empty() const { return experiments.empty(); }
  Experiment &operator[](std::size_t i) { return experiments[i]; }
  const Experiment &operator[](std::size_t i) const { return experiments[i]; }
  std::vector<Experiment>::iterator begin() { return experiments.begin(); }
  std::vector<Experiment>::iterator end() { return experiments.end(); }
  std::vector<Experiment>::const_iterator begin() const {
    return experiments.begin();
  }
  std::vector<Experiment>::const_iterator end() const {
    return experiments.end();
  }
};

//: The experiment list's JSON tree, from an .expt or a .rflx's /experiments
//: (docs/rflx.md), told by what the file is; a .rflx without one refused.
json::Value read_experiment_document(const std::string &path);
ExperimentList read_experiments(const std::string &path);
ExperimentList experiments_from_json(const json::Value &document);

// Serialise. Models are shared where they are identical, as DIALS does, so
// four sweeps on one crystal write one crystal and four goniometers.
json::Value experiments_to_json(const ExperimentList &list);

//: Several experiment lists as one, as dials.import writes several sweeps:
//: every model of each kept as its own, and each experiment's indices
//: renumbered past those of the lists before it. The history is the first's.
json::Value join_experiment_lists(const std::vector<json::Value> &lists);

//: Experiment `index` of a list alone: the experiment, and of each kind of
//: model only the one it names, renumbered to the first -- a list of one sweep,
//: as mxi_integrate integrates each of several.
json::Value slice_experiment_list(const json::Value &list, std::size_t index);

//: Models of one kind that are identical in content made one: every experiment
//: naming a copy names the first instead, and the copies are gone. As several
//: sweeps integrated apart carry copies of the one crystal they were refined
//: with, and DIALS writes it once, shared.
void share_identical(json::Value *list, const std::string &kind);
void write_experiments(const std::string &path, const ExperimentList &list);

} // namespace mxi

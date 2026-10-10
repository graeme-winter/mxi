// Reading an experiment list, over the JSON parser beside it.
//
// This file used to carry its own parser -- four hundred lines duplicating
// src/json.cc. Two parsers for one format is two places for a convention to be
// wrong, and this repository has spent enough on that.
//
// It does NOT use the shared experiment reader, and the reason is worth
// recording rather than rediscovering. That reader is deliberately strict: it
// refuses a scan with no oscillation, because assuming zero would make one
// compare equal to anything. The spot finder needs three facts and where the
// images are, and none of them is the oscillation; making it refuse an .expt
// it can perfectly well work from would be a regression dressed as tidying.
//
// So the parsing is shared and the interpretation is not. `expt::Info` is what
// the spot finder needs and deliberately is not a dxtbx experiment list.

#include "expt.hh"

#include <stdexcept>

#include "../expt.hh"
#include "../json.hh"

namespace expt {

namespace {

// The imageset block, which the shared reader keeps verbatim in `source` but
// does not interpret: nothing else in this repository needs to know where the
// images are, and the spot finder needs nothing else from it.
void read_imageset(const mxi::json::Value &document,
                   const mxi::json::Value &experiment, Info *info) {
  if (!document.is_object())
    return;
  const mxi::json::Value &imagesets = document["imageset"];
  if (!imagesets.is_array() || imagesets.as_array().empty())
    return;
  info->imagesets = imagesets.as_array().size();

  // The experiment's own image set, by its index; the first where it names
  // none, as a list of one sweep may not.
  std::size_t at = 0;
  if (experiment.is_object() && experiment["imageset"].is_number())
    at = static_cast<std::size_t>(experiment["imageset"].as_number());
  if (at >= imagesets.as_array().size())
    return;
  const mxi::json::Value &first = imagesets.as_array()[at];
  if (!first.is_object())
    return;
  const mxi::json::Value &tmpl = first["template"];
  if (tmpl.is_string() && !tmpl.as_string().empty()) {
    info->has_imageset = true;
    info->image_file = tmpl.as_string();
    info->templated = info->image_file.find('#') != std::string::npos;
  }

  const mxi::json::Value &indices = first["single_file_indices"];
  if (!indices.is_array() || indices.as_array().empty())
    return;
  const mxi::json::Array &items = indices.as_array();
  info->frames = items.size();
  info->first_index = static_cast<std::int64_t>(items.front().as_number());
  info->last_index = static_cast<std::int64_t>(items.back().as_number());
  // Checked rather than assumed: a sliced or filtered import can leave gaps,
  // and reading them as a range would quietly read the wrong frames.
  for (std::size_t i = 1; i < items.size(); ++i) {
    const auto previous = static_cast<std::int64_t>(items[i - 1].as_number());
    const auto current = static_cast<std::int64_t>(items[i].as_number());
    if (current != previous + 1) {
      info->contiguous_indices = false;
      break;
    }
  }
}

} // namespace

namespace {

// The model an experiment names, by its index into the document's list.
const mxi::json::Value *model_of(const mxi::json::Value &document,
                                 const mxi::json::Value &experiment,
                                 const char *name) {
  const mxi::json::Value &index = experiment[name];
  if (!index.is_number())
    return nullptr;
  const mxi::json::Value &list = document[name];
  if (!list.is_array())
    return nullptr;
  const auto at = static_cast<std::size_t>(index.as_number());
  if (at >= list.as_array().size())
    return nullptr;
  return &list.as_array()[at];
}

} // namespace

Info read(const std::string &path, std::size_t index) {
  Info info;
  mxi::json::Value document;
  try {
    document = mxi::read_experiment_document(path);
  } catch (const std::exception &error) {
    throw std::runtime_error(error.what());
  }
  if (!document.is_object() || !document["experiment"].is_array()) {
    throw std::runtime_error(path + " is not an experiment list");
  }
  const mxi::json::Array &experiments = document["experiment"].as_array();
  info.experiments = experiments.size();
  if (experiments.empty())
    return info;
  if (index >= experiments.size())
    throw std::runtime_error(
        path + " has " + std::to_string(experiments.size()) +
        " experiments, and no experiment " + std::to_string(index));
  info.index = index;
  const mxi::json::Value &first = experiments[index];

  if (first["identifier"].is_string()) {
    info.identifier = first["identifier"].as_string();
  }

  // A still has no scan, which is not an error.
  if (const mxi::json::Value *scan = model_of(document, first, "scan")) {
    const mxi::json::Value &range = (*scan)["image_range"];
    if (range.is_array() && range.as_array().size() == 2) {
      info.has_scan = true;
      info.first_image =
          static_cast<std::int64_t>(range.as_array()[0].as_number());
      info.last_image =
          static_cast<std::int64_t>(range.as_array()[1].as_number());
    }
  }

  if (const mxi::json::Value *detector =
          model_of(document, first, "detector")) {
    const mxi::json::Value &panels = (*detector)["panels"];
    if (panels.is_array() && !panels.as_array().empty()) {
      info.has_detector = true;
      info.panels = panels.as_array().size();
      const mxi::json::Value &size = panels.as_array()[0]["image_size"];
      if (size.is_array() && size.as_array().size() == 2) {
        info.image_fast =
            static_cast<std::size_t>(size.as_array()[0].as_number());
        info.image_slow =
            static_cast<std::size_t>(size.as_array()[1].as_number());
      }
    }
  }

  read_imageset(document, first, &info);
  return info;
}

std::string describe(const Info &info) {
  std::string result;
  if (info.experiments > 1) {
    result = "experiment " + std::to_string(info.index + 1) + " of " +
             std::to_string(info.experiments);
  } else {
    result = std::to_string(info.experiments) + " experiment";
    if (info.experiments != 1)
      result += "s";
  }
  if (info.has_scan) {
    result += ", images " + std::to_string(info.first_image) + " to " +
              std::to_string(info.last_image);
  }
  if (info.has_imageset) {
    result += ", " + info.image_file;
    if (info.frames != 0) {
      result += " (" + std::to_string(info.frames) + " frames)";
    }
  }
  if (info.has_detector) {
    result += ", " + std::to_string(info.panels) + " panel";
    if (info.panels != 1)
      result += "s";
    result += " of " + std::to_string(info.image_fast) + " x " +
              std::to_string(info.image_slow) + " pixels (fast x slow)";
  }
  if (!info.identifier.empty())
    result += ", identifier " + info.identifier;
  return result;
}

} // namespace expt

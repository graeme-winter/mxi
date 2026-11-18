// Regions of the detector a user has marked untrusted -- a backstop, its arm,
// a shadow -- drawn with mxi_view and kept in each image set's block of the
// experiment list, so that they travel with the data through every program:
//
//     "untrusted": [{"panel": 0, "circle": [x, y, r]},
//                   {"panel": 0, "rectangle": [x0, y0, x1, y1]},
//                   {"panel": 0, "polygon": [[x, y], [x, y], ...]},
//                   {"panel": 0, "pixels": [[x, y], [x, y], ...]}]
//
// in pixels, as DIALS counts them: pixel i spans i to i + 1, so a pixel is
// inside a shape when its centre, i + 0.5, is. "pixels" lists whole pixels by
// column and row -- hot pixels, signal on too many images (mxi find).
// Rasterised for a frame, they join the detector's own pixel_mask
// (Series::add_masked), so that spot finding and integration, on the CPU and on
// a device, leave them out as they leave out the detector's bad pixels.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hh"

namespace mxi {

//: The pixels of a width x height frame inside any of `shapes`, as row-major
//: indices y * width + x, sorted and unique. A shape on a panel other than 0,
//: or not one of the three, is refused with std::runtime_error: a mask that
//: does not apply must not pass for one that does.
std::vector<std::uint64_t> untrusted_pixels(const json::Value &shapes,
                                            std::uint64_t width,
                                            std::uint64_t height);

//: How many shapes, and of what, for a line of output: "2 circles, 1 polygon".
std::string describe_untrusted(const json::Value &shapes);

} // namespace mxi

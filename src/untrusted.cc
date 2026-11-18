#include "untrusted.hh"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mxi {

namespace {

double number(const json::Value &v, const char *what) {
  if (!v.is_number())
    throw std::runtime_error(std::string("an untrusted ") + what +
                             " has a coordinate that is not a number");
  return v.as_number();
}

// Each pixel in [x0, x1) x [y0, y1) whose centre `inside` accepts.
template <typename Inside>
void fill(double x0, double y0, double x1, double y1, std::uint64_t width,
          std::uint64_t height, Inside inside,
          std::vector<std::uint64_t> *out) {
  const auto clamp = [](double v, std::uint64_t n) {
    return static_cast<std::int64_t>(
        std::clamp(v, 0.0, static_cast<double>(n)));
  };
  const std::int64_t i0 = clamp(std::floor(x0), width),
                     i1 = clamp(std::ceil(x1), width);
  const std::int64_t j0 = clamp(std::floor(y0), height),
                     j1 = clamp(std::ceil(y1), height);
  for (std::int64_t j = j0; j < j1; ++j)
    for (std::int64_t i = i0; i < i1; ++i)
      if (inside(static_cast<double>(i) + 0.5, static_cast<double>(j) + 0.5))
        out->push_back(static_cast<std::uint64_t>(j) * width +
                       static_cast<std::uint64_t>(i));
}

} // namespace

std::vector<std::uint64_t> untrusted_pixels(const json::Value &shapes,
                                            std::uint64_t width,
                                            std::uint64_t height) {
  std::vector<std::uint64_t> out;
  if (!shapes.is_array())
    return out;
  for (const json::Value &s : shapes.as_array()) {
    if (!s.is_object())
      throw std::runtime_error("an untrusted region that is not an object");
    if (s["panel"].is_number() && s["panel"].as_number() != 0.0)
      throw std::runtime_error(
          "an untrusted region on a panel other than 0: frames are read as one "
          "panel here");
    if (s["circle"].is_array()) {
      const json::Array &c = s["circle"].as_array();
      if (c.size() != 3)
        throw std::runtime_error("an untrusted circle is x, y, r");
      const double x = number(c[0], "circle"), y = number(c[1], "circle"),
                   r = number(c[2], "circle");
      fill(
          x - r, y - r, x + r, y + r, width, height,
          [&](double px, double py) {
            return (px - x) * (px - x) + (py - y) * (py - y) <= r * r;
          },
          &out);
    } else if (s["rectangle"].is_array()) {
      const json::Array &c = s["rectangle"].as_array();
      if (c.size() != 4)
        throw std::runtime_error("an untrusted rectangle is x0, y0, x1, y1");
      double x0 = number(c[0], "rectangle"), y0 = number(c[1], "rectangle");
      double x1 = number(c[2], "rectangle"), y1 = number(c[3], "rectangle");
      if (x1 < x0)
        std::swap(x0, x1);
      if (y1 < y0)
        std::swap(y0, y1);
      fill(
          x0, y0, x1, y1, width, height,
          [&](double px, double py) {
            return px >= x0 && px <= x1 && py >= y0 && py <= y1;
          },
          &out);
    } else if (s["polygon"].is_array()) {
      std::vector<double> xs, ys;
      for (const json::Value &p : s["polygon"].as_array()) {
        if (!p.is_array() || p.as_array().size() != 2)
          throw std::runtime_error("an untrusted polygon's vertex is x, y");
        xs.push_back(number(p.as_array()[0], "polygon"));
        ys.push_back(number(p.as_array()[1], "polygon"));
      }
      if (xs.size() < 3)
        throw std::runtime_error("an untrusted polygon has fewer than three "
                                 "vertices");
      const std::size_t n = xs.size();
      // Even-odd: a centre is inside if a ray to its right crosses the edges
      // an odd number of times.
      fill(
          *std::min_element(xs.begin(), xs.end()),
          *std::min_element(ys.begin(), ys.end()),
          *std::max_element(xs.begin(), xs.end()),
          *std::max_element(ys.begin(), ys.end()), width, height,
          [&](double px, double py) {
            bool in = false;
            for (std::size_t a = 0, b = n - 1; a < n; b = a++)
              if ((ys[a] > py) != (ys[b] > py) &&
                  px < (xs[b] - xs[a]) * (py - ys[a]) / (ys[b] - ys[a]) + xs[a])
                in = !in;
            return in;
          },
          &out);
    } else if (s["pixels"].is_array()) {
      // Single pixels, by column and row: hot pixels, found by mxi find
      // --mask-hot or mxi untrusted --hot (16 November). A pixel off the
      // frame is refused, as a shape that cannot apply.
      for (const json::Value &p : s["pixels"].as_array()) {
        if (!p.is_array() || p.as_array().size() != 2)
          throw std::runtime_error("an untrusted pixel is x, y");
        const double x = number(p.as_array()[0], "pixel");
        const double y = number(p.as_array()[1], "pixel");
        if (x != std::floor(x) || y != std::floor(y) || x < 0.0 || y < 0.0 ||
            x >= static_cast<double>(width) || y >= static_cast<double>(height))
          throw std::runtime_error("an untrusted pixel " + std::to_string(x) +
                                   ", " + std::to_string(y) +
                                   " is not a whole pixel on the frame");
        out.push_back(static_cast<std::uint64_t>(y) * width +
                      static_cast<std::uint64_t>(x));
      }
    } else {
      throw std::runtime_error("an untrusted region is a circle, a rectangle, "
                               "a polygon or a list of pixels");
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string describe_untrusted(const json::Value &shapes) {
  if (!shapes.is_array())
    return "none";
  std::size_t circles = 0, rectangles = 0, polygons = 0, pixels = 0;
  for (const json::Value &s : shapes.as_array()) {
    circles += s["circle"].is_array();
    rectangles += s["rectangle"].is_array();
    polygons += s["polygon"].is_array();
    if (s["pixels"].is_array())
      pixels += s["pixels"].as_array().size();
  }
  std::string out;
  const auto add = [&](std::size_t k, const char *one, const char *many) {
    if (k == 0)
      return;
    if (!out.empty())
      out += ", ";
    out += std::to_string(k) + " " + (k == 1 ? one : many);
  };
  add(circles, "circle", "circles");
  add(rectangles, "rectangle", "rectangles");
  add(polygons, "polygon", "polygons");
  add(pixels, "single pixel", "single pixels");
  return out.empty() ? "none" : out;
}

} // namespace mxi

#include "laue.hh"

#include <gemmi/it92.hpp>

#include "parallel.hh"

#include "resolution.hh"
#include "scale.hh"
#include "timing.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <unordered_map>

namespace mxi {

namespace {

const Rotation kIdentityRotation{1, 0, 0, 0, 1, 0, 0, 0, 1};

Rotation times(const Rotation &a, const Rotation &b) {
  Rotation out{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      int v = 0;
      for (int k = 0; k < 3; ++k)
        v += a[static_cast<std::size_t>(i * 3 + k)] *
             b[static_cast<std::size_t>(k * 3 + j)];
      out[static_cast<std::size_t>(i * 3 + j)] = v;
    }
  return out;
}

double cauchy_cdf(double x, double loc, double scale) {
  return 0.5 + std::atan((x - loc) / scale) / std::acos(-1.0);
}

double trunc_cauchy_pdf(double x, double a, double b, double loc,
                        double scale) {
  const double pdf =
      scale / (std::acos(-1.0) * ((x - loc) * (x - loc) + scale * scale));
  return pdf / (cauchy_cdf(b, loc, scale) - cauchy_cdf(a, loc, scale));
}

struct Accumulator {
  double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
  std::size_t n = 0;
  void add(double x, double y) {
    sx += x;
    sy += y;
    sxx += x * x;
    syy += y * y;
    sxy += x * y;
    ++n;
  }
  double cc() const {
    if (n < 2)
      return 0.0;
    const double N = static_cast<double>(n);
    const double v = (sxx - sx * sx / N) * (syy - sy * sy / N);
    return v > 0.0 ? (sxy - sx * sy / N) / std::sqrt(v) : 0.0;
  }
};

//: Quartiles by linear interpolation.
double quantile_of(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  const double at = q * static_cast<double>(v.size() - 1);
  const auto lo = static_cast<std::size_t>(at);
  const std::size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (at - static_cast<double>(lo)) * (v[hi] - v[lo]);
}

} // namespace

int rotation_order(const Rotation &r) {
  Rotation p = r;
  for (int n = 1; n <= 6; ++n) {
    if (p == kIdentityRotation)
      return n;
    p = times(p, r);
  }
  return 0;
}

std::vector<Rotation> symmetry_elements(const std::vector<Rotation> &lattice) {
  std::vector<Rotation> out;
  for (const Rotation &r : lattice) {
    if (std::find(out.begin(), out.end(), r) != out.end())
      continue;
    const int n = rotation_order(r);
    // A rotation of order above two stands with its inverse, r^(n-1).
    bool inverse_listed = false;
    if (n > 2) {
      Rotation inv = r;
      for (int k = 1; k < n - 1; ++k)
        inv = times(inv, r);
      inverse_listed = std::find(out.begin(), out.end(), inv) != out.end();
    }
    if (!inverse_listed)
      out.push_back(r);
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Rotation &a, const Rotation &b) {
                     return rotation_order(a) == 1 && rotation_order(b) != 1;
                   });
  return out;
}

namespace {

// The mean of carbon's, nitrogen's and oxygen's squared scattering factors at
// d: a light atom's, as near right for a small molecule as for a protein.
double mean_f2(double d) {
  using It = gemmi::IT92<double>;
  const double stol2 = 1.0 / (4.0 * d * d);
  double sum = 0.0;
  for (gemmi::El el : {gemmi::El::C, gemmi::El::N, gemmi::El::O}) {
    const double f = It::get(el, 0).calculate_sf(stol2);
    sum += f * f;
  }
  return sum / 3.0;
}

std::array<double, 6> q_of(const Miller &h) {
  const double a = h[0], b = h[1], c = h[2];
  return {a * a, b * b, c * c, 2 * a * b, 2 * a * c, 2 * b * c};
}

// Wilson's maximum likelihood, acentric: minus the log likelihood is
// sum I/S + ln S, S = k f2 exp(-q.b), convex in (ln k, b), so Newton's method,
// halving a step that does not lower it. Negative intensities as zero.
WilsonFit fit_wilson(const P1Intensities &data) {
  WilsonFit fit;
  std::vector<std::size_t> rows;
  for (std::size_t k = 0; k < data.size(); ++k) {
    const double ds2 = 1.0 / (data.d[k] * data.d[k]);
    if (ds2 > 0.008 && ds2 < 0.690 && std::isfinite(data.i[k]))
      rows.push_back(k);
  }
  fit.used = rows.size();
  if (rows.size() < 100)
    return fit;
  std::vector<double> lf2(rows.size()), ipos(rows.size());
  std::vector<std::array<double, 6>> q(rows.size());
  double mean_i = 0.0, mean_f = 0.0;
  for (std::size_t r = 0; r < rows.size(); ++r) {
    const std::size_t k = rows[r];
    const double f2 = mean_f2(data.d[k]);
    lf2[r] = std::log(f2);
    ipos[r] = std::max(0.0, data.i[k]);
    q[r] = q_of(data.hkl[k]);
    mean_i += ipos[r];
    mean_f += f2;
  }
  if (!(mean_i > 0.0))
    return fit;
  std::array<double, 7> theta{std::log(mean_i / mean_f), 0, 0, 0, 0, 0, 0};
  const auto nll = [&](const std::array<double, 7> &t) {
    double sum = 0.0;
    for (std::size_t r = 0; r < rows.size(); ++r) {
      double x = t[0] + lf2[r];
      for (int j = 0; j < 6; ++j)
        x -= q[r][static_cast<std::size_t>(j)] *
             t[static_cast<std::size_t>(j + 1)];
      sum += ipos[r] * std::exp(-x) + x;
    }
    return sum;
  };
  double current = nll(theta);
  for (int it = 0; it < 100; ++it) {
    double g[7] = {0}, H[7][7] = {{0}};
    for (std::size_t r = 0; r < rows.size(); ++r) {
      double x = theta[0] + lf2[r];
      for (int j = 0; j < 6; ++j)
        x -= q[r][static_cast<std::size_t>(j)] *
             theta[static_cast<std::size_t>(j + 1)];
      const double ratio = ipos[r] * std::exp(-x); // I / S
      double a[7] = {1.0};
      for (int j = 0; j < 6; ++j)
        a[j + 1] = -q[r][static_cast<std::size_t>(j)];
      for (int u = 0; u < 7; ++u) {
        g[u] += (1.0 - ratio) * a[u];
        for (int v = 0; v < 7; ++v)
          H[u][v] += ratio * a[u] * a[v];
      }
    }
    // Solve H step = -g by Gaussian elimination with partial pivoting.
    double m[7][8];
    for (int u = 0; u < 7; ++u) {
      for (int v = 0; v < 7; ++v)
        m[u][v] = H[u][v] + (u == v ? 1e-12 * (1.0 + std::fabs(H[u][u])) : 0.0);
      m[u][7] = -g[u];
    }
    for (int col = 0; col < 7; ++col) {
      int pivot = col;
      for (int r = col + 1; r < 7; ++r)
        if (std::fabs(m[r][col]) > std::fabs(m[pivot][col]))
          pivot = r;
      for (int v = 0; v < 8; ++v)
        std::swap(m[col][v], m[pivot][v]);
      if (std::fabs(m[col][col]) < 1e-300)
        return fit;
      for (int r = 0; r < 7; ++r) {
        if (r == col)
          continue;
        const double f = m[r][col] / m[col][col];
        for (int v = col; v < 8; ++v)
          m[r][v] -= f * m[col][v];
      }
    }
    std::array<double, 7> step;
    for (int u = 0; u < 7; ++u)
      step[static_cast<std::size_t>(u)] = m[u][7] / m[u][u];
    double scale = 1.0, next = current;
    std::array<double, 7> trial = theta;
    for (int halving = 0; halving < 40; ++halving) {
      for (std::size_t u = 0; u < 7; ++u)
        trial[u] = theta[u] + scale * step[u];
      next = nll(trial);
      if (next <= current)
        break;
      scale *= 0.5;
    }
    if (!(next <= current))
      break;
    const double change = current - next;
    theta = trial;
    current = next;
    fit.iterations = it + 1;
    if (change <= 1e-10 * std::fabs(current))
      break;
  }
  fit.fitted = std::isfinite(current);
  fit.log_scale = theta[0];
  for (int j = 0; j < 6; ++j)
    fit.b[j] = theta[static_cast<std::size_t>(j + 1)];
  return fit;
}

} // namespace

std::size_t normalise(P1Intensities &data, std::size_t per_shell,
                      WilsonFit *fit_out) {
  // E^2 by shells of equal count, for the Wilson outliers alone.
  std::vector<double> e2(data.size(), 0.0);
  {
    std::vector<std::size_t> order(data.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      return data.d[a] > data.d[b];
    });
    for (std::size_t start = 0; start < order.size(); start += per_shell) {
      std::size_t end = std::min(order.size(), start + per_shell);
      if (order.size() - end < per_shell / 2)
        end = order.size(); // no shell of a handful at the end
      double mean = 0.0;
      for (std::size_t k = start; k < end; ++k)
        mean += data.i[order[k]] / static_cast<double>(end - start);
      for (std::size_t k = start; k < end; ++k)
        e2[order[k]] = mean > 0.0 ? data.i[order[k]] / mean : 0.0;
      if (end == order.size())
        break;
    }
  }
  // The intensities themselves by the fitted model's scale and anisotropic
  // fall-off, as ml_normalise_aniso; by the shells, as before, if it could not
  // be fitted.
  const WilsonFit fit = fit_wilson(data);
  if (fit_out)
    *fit_out = fit;
  for (std::size_t k = 0; k < data.size(); ++k) {
    double factor;
    if (fit.fitted) {
      const std::array<double, 6> q = q_of(data.hkl[k]);
      double qb = 0.0;
      for (std::size_t j = 0; j < 6; ++j)
        qb += q[j] * fit.b[j];
      factor = std::exp(qb - fit.log_scale);
    } else {
      factor = e2[k] != 0.0 && data.i[k] != 0.0 ? e2[k] / data.i[k] : 1.0;
    }
    data.i[k] *= factor;
    data.sigma[k] *= factor;
  }
  // Wilson outliers, E^2 of 16 or more, as dials.symmetry removes them.
  P1Intensities kept;
  std::size_t removed = 0;
  for (std::size_t k = 0; k < data.size(); ++k) {
    if (e2[k] >= 16.0) {
      ++removed;
      continue;
    }
    kept.hkl.push_back(data.hkl[k]);
    kept.i.push_back(data.i[k]);
    kept.sigma.push_back(data.sigma[k]);
    kept.d.push_back(data.d[k]);
  }
  data = std::move(kept);
  return removed;
}

double p_cc_given_present(double cc, double sigma_cc, double expected) {
  return trunc_cauchy_pdf(cc, -1.0, 1.0, expected, sigma_cc);
}

double p_cc_given_absent(double cc, double sigma_cc) {
  // integral over x in [0, 1] of pdf(cc; x) (1 - x^2)^(1/2), over that of
  // (1 - x^2)^(1/2), which is pi/4; by the midpoint rule on 2000 steps.
  const int steps = 2000;
  double sum = 0.0;
  for (int k = 0; k < steps; ++k) {
    const double x = (k + 0.5) / steps;
    sum +=
        trunc_cauchy_pdf(cc, -1.0, 1.0, x, sigma_cc) * std::sqrt(1.0 - x * x);
  }
  return (sum / steps) / (std::acos(-1.0) / 4.0);
}

namespace {

//: A Miller index as one integer, 21 bits an index: a hash table's key, where
//: an ordered map compared indices down a tree for every lookup.
std::uint64_t pack(const Miller &m) {
  const auto bits = [](int v) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(v) & 0x1FFFFF);
  };
  return (bits(m[0]) << 42) | (bits(m[1]) << 21) | bits(m[2]);
}

using Where = std::unordered_map<std::uint64_t, std::size_t>;

//: CC of I(h) against I(-h) over the Friedel pairs present: what equivalent
//: reflections achieve, as dials.symmetry estimates E(CC; S) from the identity.
double identity_cc(const P1Intensities &data, const Where &where) {
  Accumulator acc;
  for (std::size_t k = 0; k < data.size(); ++k) {
    const Miller &h = data.hkl[k];
    const auto it = where.find(pack(Miller{-h[0], -h[1], -h[2]}));
    if (it != where.end())
      acc.add(data.i[k], data.i[it->second]);
  }
  return acc.n > 10 ? acc.cc() : 1.0;
}

} // namespace

LaueScores score_laue_groups(const P1Intensities &data,
                             const std::vector<Rotation> &lattice,
                             unsigned seed) {
  LaueScores out;
  double mark = Timing::now();
  Where where;
  where.reserve(data.size());
  for (std::size_t k = 0; k < data.size(); ++k)
    where[pack(data.hkl[k])] = k; // exact: Friedel mates are apart

  // sigma(CC) as a function of sample size, from pairs of unrelated
  // reflections at similar resolution: rms CC against 1/sqrt(n), fitted.
  std::mt19937 rng(seed);
  {
    std::vector<std::size_t> by_d(data.size());
    std::iota(by_d.begin(), by_d.end(), std::size_t{0});
    std::sort(by_d.begin(), by_d.end(), [&](std::size_t a, std::size_t b) {
      return data.d[a] > data.d[b];
    });
    std::vector<double> a, b;
    const std::size_t bin = std::max<std::size_t>(200, data.size() / 500 + 1);
    for (std::size_t s = 0; s + 1 < by_d.size(); s += bin) {
      std::vector<std::size_t> in(
          by_d.begin() + static_cast<long>(s),
          by_d.begin() + static_cast<long>(std::min(by_d.size(), s + bin)));
      std::shuffle(in.begin(), in.end(), rng);
      for (std::size_t k = 0; k + 1 < in.size(); k += 2) {
        a.push_back(data.i[in[k]]);
        b.push_back(data.i[in[k + 1]]);
      }
    }
    const std::size_t pairs = a.size();
    const std::size_t max_n = std::min<std::size_t>(pairs / 10, 200),
                      min_n = std::min<std::size_t>(5, max_n);
    if (max_n >= min_n + 4) {
      double sx = 0, sy = 0, sxx = 0, sxy = 0, k = 0;
      std::uniform_int_distribution<std::size_t> pick(0, pairs - 1);
      for (std::size_t n = min_n; n <= max_n; ++n) {
        double ms = 0.0;
        for (int rep = 0; rep < 200; ++rep) {
          Accumulator acc;
          for (std::size_t j = 0; j < n; ++j) {
            const std::size_t p = pick(rng);
            acc.add(a[p], b[p]);
          }
          ms += acc.cc() * acc.cc() / 200.0;
        }
        const double x = 1.0 / std::sqrt(static_cast<double>(n)),
                     y = std::sqrt(ms);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        k += 1.0;
      }
      out.cc_sig_fac = (sxy - sx * sy / k) / (sxx - sx * sx / k);
    }
  }
  // E(CC; S): the intensities' variance over it plus the mean variance of the
  // errors, combined as dials.symmetry does with CC of the identity, which on
  // intensities with Friedel mates merged is one.
  {
    double mean = 0.0, s2 = 0.0;
    for (std::size_t k = 0; k < data.size(); ++k) {
      mean += data.i[k] / static_cast<double>(data.size());
      s2 += data.sigma[k] * data.sigma[k] / static_cast<double>(data.size());
    }
    double var = 0.0;
    for (std::size_t k = 0; k < data.size(); ++k)
      var += (data.i[k] - mean) * (data.i[k] - mean) /
             static_cast<double>(data.size() - 1);
    out.e_cc_true = var / (var + s2);
    // CC of the identity, I(h) against I(-h): measured.
    out.cc_identity = identity_cc(data, where);
    const double sigma_1 = std::fmax(0.05, out.cc_sig_fac / std::sqrt(200.0));
    const double sigma_2 = std::fmax(
        0.05, out.cc_sig_fac / std::sqrt(static_cast<double>(data.size())));
    const double w1 = sigma_1 > 1e-4 ? 1.0 / (sigma_1 * sigma_1) : 0.0;
    const double w2 = data.size() > 10 ? 1.0 / (sigma_2 * sigma_2) : 0.0;
    out.cc_true = (w1 * out.e_cc_true + w2 * out.cc_identity) / (w1 + w2);
  }
  out.t_estimates = Timing::now() - mark;
  mark = Timing::now();
  // Each element: CC over the reflections it relates, excluding those it
  // leaves in place (on the axis), with dials.symmetry's generous outlier cut.
  // Each element on its own thread: each reads only what is above and writes
  // only its own score, so the scores are the same, element by element, on
  // any number of threads. One after another they were most of the scoring.
  const std::vector<Rotation> element_rotations = symmetry_elements(lattice);
  out.elements.assign(element_rotations.size(), ElementScore{});
  for_each_index(element_rotations.size(), [&](std::size_t element) {
    const Rotation &r = element_rotations[element];
    ElementScore e;
    e.rotation = r;
    e.order = rotation_order(r);
    std::vector<Rotation> ops{r};
    if (e.order > 2) {
      Rotation inv = r;
      for (int k = 1; k < e.order - 1; ++k)
        inv = times(inv, r);
      ops.push_back(inv);
    }
    std::vector<double> xs, ys;
    for (const Rotation &op : ops) {
      for (std::size_t k = 0; k < data.size(); ++k) {
        const Miller h = data.hkl[k];
        // The identity pairs h with -h, its Friedel mate; the rest h with
        // its image, exactly.
        const Miller image =
            e.order == 1 ? Miller{-h[0], -h[1], -h[2]} : apply(op, h);
        if (e.order > 1 && image == h)
          continue; // on the axis: epsilon > 1
        const auto it = where.find(pack(image));
        if (it == where.end())
          continue;
        xs.push_back(data.i[k]);
        ys.push_back(data.i[it->second]);
      }
    }
    std::vector<bool> keep(xs.size(), true);
    for (const std::vector<double> *col : {&xs, &ys}) {
      if (col->empty())
        break;
      const double q1 = quantile_of(*col, 0.25), q3 = quantile_of(*col, 0.75);
      const double cut = 20.0 * (q3 - q1);
      for (std::size_t k = 0; k < col->size(); ++k)
        if ((*col)[k] > q3 + cut || (*col)[k] < q1 - cut)
          keep[k] = false;
    }
    Accumulator acc;
    for (std::size_t k = 0; k < xs.size(); ++k)
      if (keep[k])
        acc.add(xs[k], ys[k]);
    e.pairs = acc.n;
    e.sx = acc.sx;
    e.sy = acc.sy;
    e.sxx = acc.sxx;
    e.syy = acc.syy;
    e.sxy = acc.sxy;
    if (e.pairs > 0) {
      e.cc = acc.cc();
      e.sigma_cc = std::fmax(0.1, out.cc_sig_fac /
                                      std::sqrt(static_cast<double>(e.pairs)));
      e.z = e.cc / e.sigma_cc;
      e.p_given_present = p_cc_given_present(e.cc, e.sigma_cc, out.cc_true);
      e.p_given_absent = p_cc_given_absent(e.cc, e.sigma_cc);
      e.likelihood = e.p_given_present / (e.p_given_present + e.p_given_absent);
    }
    out.elements[element] = e;
  });
  out.t_elements = Timing::now() - mark;
  mark = Timing::now();
  // Each subgroup: Evans (2011) A2.
  double total = 0.0;
  for (const std::vector<Rotation> &g : subgroups(lattice)) {
    GroupScore s;
    s.rotations = g;
    Accumulator in, out_of;
    double pl = 0.0, zf = 0.0, za = 0.0;
    std::size_t nf = 0, na = 0;
    for (const ElementScore &e : out.elements) {
      const bool has = std::find(g.begin(), g.end(), e.rotation) != g.end();
      s.contains.push_back(has);
      Accumulator &acc = has ? in : out_of;
      acc.sx += e.sx;
      acc.sy += e.sy;
      acc.sxx += e.sxx;
      acc.syy += e.syy;
      acc.sxy += e.sxy;
      acc.n += e.pairs;
      if (e.pairs <= 2)
        continue;
      if (has) {
        zf += e.z * e.z;
        ++nf;
        pl += std::log(e.p_given_present);
      } else {
        za += e.z * e.z;
        ++na;
        pl += std::log(e.p_given_absent);
      }
    }
    s.likelihood = std::exp(pl);
    s.z_for = nf ? std::sqrt(zf / nf) : 0.0;
    s.z_against = na ? std::sqrt(za / na) : 0.0;
    s.z_net = s.z_for - s.z_against;
    s.cc_for = in.cc();
    s.cc_against = out_of.cc();
    total += s.likelihood;
    out.groups.push_back(std::move(s));
  }
  for (GroupScore &s : out.groups)
    s.likelihood = total > 0.0 ? s.likelihood / total : 0.0;
  std::stable_sort(out.groups.begin(), out.groups.end(),
                   [](const GroupScore &a, const GroupScore &b) {
                     return a.likelihood > b.likelihood;
                   });
  out.t_groups = Timing::now() - mark;
  return out;
}

SpaceGroupChoice choose_space_group(const std::vector<Miller> &hkl,
                                    const std::vector<double> &intensity,
                                    const std::vector<double> &sigma,
                                    const SpaceGroup &patterson) {
  const std::vector<SpaceGroup> groups = space_groups_with_patterson(patterson);
  SpaceGroupChoice out{groups.empty() ? patterson : groups.front(), {}, {}};
  for (const SpaceGroup &g : groups) {
    AbsenceTest t{g, 0, 0.0, true};
    double sum = 0.0;
    for (std::size_t k = 0; k < hkl.size(); ++k) {
      if (patterson.absent(hkl[k]) || !g.absent(hkl[k]) || !(sigma[k] > 0.0))
        continue; // forbidden by the centring every candidate shares, or
                  // allowed
      sum += intensity[k] / sigma[k];
      ++t.tested;
    }
    t.mean_i_over_sigma = t.tested ? sum / static_cast<double>(t.tested) : 0.0;
    t.consistent = t.tested == 0 || t.mean_i_over_sigma <= 3.0;
    out.candidates.push_back(t);
  }
  std::size_t best = 0;
  bool any = false;
  for (const AbsenceTest &t : out.candidates)
    if (t.consistent && (!any || t.tested > best)) {
      best = t.tested;
      out.chosen = t.group;
      any = true;
    }
  for (const AbsenceTest &t : out.candidates)
    if (t.consistent && t.tested == best && t.group.name() != out.chosen.name())
      out.indistinguishable.push_back(t.group.name());
  return out;
}

double laue_resolution_limit(const P1Intensities &data,
                             double min_i_over_sigma) {
  if (data.size() == 0)
    return 0.0;
  std::vector<std::size_t> order(data.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return data.d[a] > data.d[b];
  });
  const std::size_t shells = 20,
                    per = std::max<std::size_t>(1, order.size() / shells);
  double limit = data.d[order.back()];
  for (std::size_t s = 0; s < shells; ++s) {
    const std::size_t a = s * per, b = s + 1 == shells
                                           ? order.size()
                                           : std::min(order.size(), a + per);
    if (a >= b)
      break;
    double si = 0.0, ss = 0.0;
    for (std::size_t k = a; k < b; ++k) {
      si += data.i[order[k]];
      ss += data.sigma[order[k]];
    }
    if (!(ss > 0.0) || si / ss < min_i_over_sigma) {
      limit = s == 0 ? data.d[order.back()] : data.d[order[a - 1]];
      break;
    }
  }
  return limit;
}

void select_resolution(P1Intensities &data, double d_min) {
  P1Intensities kept;
  for (std::size_t k = 0; k < data.size(); ++k) {
    if (data.d[k] < d_min)
      continue;
    kept.hkl.push_back(data.hkl[k]);
    kept.i.push_back(data.i[k]);
    kept.sigma.push_back(data.sigma[k]);
    kept.d.push_back(data.d[k]);
  }
  data = std::move(kept);
}

P1Intensities merge_in_p1(const ExperimentList &experiments,
                          const Table &reflections, P1Selection *report) {
  ScaleData data =
      build_scale_data(experiments, reflections, SpaceGroup::from_name("P 1"),
                       ScaleModelShape{1, 0, 0});
  P1Selection sel;
  sel.observations = data.size();
  for (std::size_t i = 0; i < data.size(); ++i)
    if (data.intensity[i] / std::sqrt(data.variance[i]) < -5.0) {
      data.outlier[i] = true;
      ++sel.negative;
    }
  // CC half above 0.6, as dials.symmetry finds it: the tanh fit of
  // dials.estimate_resolution through 50 bins of equal count, where it crosses
  // 0.6 -- no longer the last of 20 shells above 0.6, which took whole shells'
  // steps and differed from dials.symmetry's limits.
  {
    const std::vector<double> ones(data.size(), 1.0);
    const std::vector<ResolutionBin> bins =
        cc_half_bins(data, ones, *experiments[0].crystal, 50, 10, 0.1);
    sel.d_min_cc_half = estimate_resolution(bins, 0.6).d_min_cc_half;
  }
  // Merged by inverse variance, Friedel mates apart: I+ at the unique index,
  // I- at its negative.
  // Summed by a hash table, then put in the order the ordered map these were
  // in gave -- Miller order -- so that everything after is the same; each
  // index's sums gather in the order of its observations either way, and its
  // d is its last observation's.
  struct Sums {
    Miller h;
    double w = 0.0, wi = 0.0, d = 0.0;
  };
  std::vector<Sums> sums;
  std::unordered_map<std::uint64_t, std::size_t> slot;
  slot.reserve(data.size() / 2 + 16);
  for (std::size_t i = 0; i < data.size(); ++i) {
    if (data.outlier[i])
      continue;
    const Miller &u = data.unique[data.group[i]];
    const Miller h = (i < data.plus.size() && !data.plus[i])
                         ? Miller{-u[0], -u[1], -u[2]}
                         : u;
    const double w = 1.0 / data.variance[i];
    const auto [it, fresh] = slot.emplace(pack(h), sums.size());
    if (fresh)
      sums.push_back(Sums{h});
    Sums &s = sums[it->second];
    s.w += w;
    s.wi += w * data.intensity[i];
    s.d = data.d[i];
  }
  std::sort(sums.begin(), sums.end(),
            [](const Sums &a, const Sums &b) { return a.h < b.h; });
  P1Intensities all;
  for (const Sums &s : sums) {
    if (!(s.w > 0.0))
      continue;
    all.hkl.push_back(s.h);
    all.i.push_back(s.wi / s.w);
    all.sigma.push_back(1.0 / std::sqrt(s.w));
    all.d.push_back(s.d);
  }
  sel.d_min_i_over_sigma = laue_resolution_limit(all);
  // The finer of the two, as dials.symmetry takes it.
  sel.d_min = std::fmin(sel.d_min_cc_half, sel.d_min_i_over_sigma);
  select_resolution(all, sel.d_min);
  sel.kept = all.size();
  if (report)
    *report = sel;
  return all;
}

} // namespace mxi

#ifndef ESPRESSO_P3M_INTERPOLATION_HPP
#define ESPRESSO_P3M_INTERPOLATION_HPP

#include <utils/Span.hpp>
#include <utils/index.hpp>
#include <utils/math/bspline.hpp>

#include <boost/range/algorithm/copy.hpp>

#include <cassert>
#include <tuple>
#include <vector>

template <int cao> struct InterpolationWeights {
  /** Linear index of the corner of the interpolation cube. */
  int ind;
  /** Weights for the directions */
  Utils::Array<double, cao> w_x, w_y, w_z;
};

struct p3m_interpolation_weights {
  size_t m_cao = 0;
  /** Charge fractions for mesh assignment. */
  std::vector<double> ca_frac;
  /** index of first mesh point for charge assignment. */
  std::vector<int> ca_fmp;

  /**
   * @brief Number of points in the cache.
   * @return Number of points currently in the cache.
   */
  auto size() const { return ca_fmp.size(); }

  /**
   * @brief Charge assignment order the weights are for.
   * @return The charge assigment order.
   */
  auto cao() const { return m_cao; }

  /**
   * @brief Push back weights for one point.
   *
   * @param q_ind Mesh index.
   * @param w_x Weights in first direction.
   * @param w_y Weights in second direction.
   * @param w_z Weights in third direction.
   */
  template <int cao> void store(const InterpolationWeights<cao> &w) {
    assert(cao == m_cao);

    ca_fmp.push_back(w.ind);
    auto it = std::back_inserter(ca_frac);
    boost::copy(w.w_x, it);
    boost::copy(w.w_y, it);
    boost::copy(w.w_z, it);
  }

  template <int cao> InterpolationWeights<cao> load(size_t i) const {
    assert(cao == m_cao);

    using Utils::make_const_span;
    assert(i < size());

    InterpolationWeights<cao> ret;
    ret.ind = ca_fmp[i];

    auto const offset = ca_frac.data() + 3 * i * m_cao;
    boost::copy(make_const_span(offset + 0 * m_cao, m_cao), ret.w_x.begin());
    boost::copy(make_const_span(offset + 1 * m_cao, m_cao), ret.w_y.begin());
    boost::copy(make_const_span(offset + 2 * m_cao, m_cao), ret.w_z.begin());

    return ret;
  }

  /**
   * @brief Reset the cache.
   *
   * @param cao Interpolation order.
   */
  void reset(int cao) {
    m_cao = cao;
    ca_frac.clear();
    ca_fmp.clear();
  }
};

template <int cao>
InterpolationWeights<cao>
p3m_calculate_interpolation_weights(const Utils::Vector3d &real_pos,
                                    const Utils::Vector3d &ai,
                                    p3m_local_mesh const &local_mesh) {
  /** position shift for calc. of first assignment mesh point. */
  static auto const pos_shift = std::floor((cao - 1) / 2.0) - (cao % 2) / 2.0;

  /* distance to nearest mesh point */
  double dist[3];

  /* nearest mesh point */
  Utils::Vector3i nmp;

  for (int d = 0; d < 3; d++) {
    /* particle position in mesh coordinates */
    auto const pos = ((real_pos[d] - local_mesh.ld_pos[d]) * ai[d]) - pos_shift;

    nmp[d] = (int)pos;

    /* distance to nearest mesh point */
    dist[d] = (pos - nmp[d]) - 0.5;
  }

  InterpolationWeights<cao> ret;

  /* 3d-array index of nearest mesh point */
  ret.ind = Utils::get_linear_index(nmp, local_mesh.dim,
                                    Utils::MemoryOrder::ROW_MAJOR);
  for (int i = 0; i < cao; i++) {
    using Utils::bspline;

    ret.w_x[i] = bspline<cao>(i, dist[0]);
    ret.w_y[i] = bspline<cao>(i, dist[1]);
    ret.w_z[i] = bspline<cao>(i, dist[2]);
  }

  return ret;
}

template <int cao, class Kernel>
void p3m_interpolate(p3m_local_mesh const &local_mesh,
                     InterpolationWeights<cao> const &w, Kernel kernel) {
  auto q_ind = w.ind;
  for (int i0 = 0; i0 < cao; i0++) {
    auto const tmp0 = w.w_x[i0];
    for (int i1 = 0; i1 < cao; i1++) {
      auto const tmp1 = tmp0 * w.w_y[i1];
      for (int i2 = 0; i2 < cao; i2++) {
        kernel(q_ind, tmp1 * w.w_z[i2]);

        q_ind++;
      }
      q_ind += local_mesh.q_2_off;
    }
    q_ind += local_mesh.q_21_off;
  }
}

#endif // ESPRESSO_P3M_INTERPOLATION_HPP

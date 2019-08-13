/*
Copyright (C) 2010-2018 The ESPResSo project

This file is part of ESPResSo.

ESPResSo is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ESPResSo is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef CORE_SHORT_RANGE_HPP
#define CORE_SHORT_RANGE_HPP

#include "algorithm/for_each_pair.hpp"
#include "cells.hpp"
#include "grid.hpp"

#include <boost/iterator/indirect_iterator.hpp>
#include <profiler/profiler.hpp>

#include <utility>
#include <utils/NoOp.hpp>

/**
 * @brief Distance vector and length handed to pair kernels.
 */
struct Distance {
  explicit Distance(Utils::Vector3d const &vec21)
      : vec21(vec21), dist2(vec21.norm2()) {}

  Utils::Vector3d vec21;
  const double dist2;
};

namespace detail {
struct MinimalImageDistance {
  const BoxGeometry box;

  Distance operator()(Particle const &p1, Particle const &p2) const {
    return Distance(get_mi_vector(p1.r.p, p2.r.p, box));
  }
};

struct LayeredMinimalImageDistance {
  const BoxGeometry box;

  Distance operator()(Particle const &p1, Particle const &p2) const {
    auto mi_dist = get_mi_vector(p1.r.p, p2.r.p, box);
    mi_dist[2] = p1.r.p[2] - p2.r.p[2];

    return Distance(mi_dist);
  }
};

struct EuclidianDistance {
  Distance operator()(Particle const &p1, Particle const &p2) const {
    return Distance(p1.r.p - p2.r.p);
  }
};

/**
 * @brief Decided which distance function to use depending on the
          cell system, and call the pair code.
*/
template <typename CellIterator, typename ParticleKernel, typename PairKernel,
          typename VerletCriterion>
void decide_distance(CellIterator first, CellIterator last,
                     ParticleKernel &&particle_kernel, PairKernel &&pair_kernel,
                     VerletCriterion &&verlet_criterion) {
  switch (cell_structure.type) {
  case CELL_STRUCTURE_DOMDEC:
    Algorithm::for_each_pair(
        first, last, std::forward<PairKernel>(pair_kernel), EuclidianDistance{},
        std::forward<VerletCriterion>(verlet_criterion),
        cell_structure.use_verlet_list, rebuild_verletlist);
    break;
  case CELL_STRUCTURE_NSQUARE:
    Algorithm::for_each_pair(first, last, std::forward<PairKernel>(pair_kernel),
                             MinimalImageDistance{box_geo},
                             std::forward<VerletCriterion>(verlet_criterion),
                             cell_structure.use_verlet_list,
                             rebuild_verletlist);
    break;
  case CELL_STRUCTURE_LAYERED:
    Algorithm::for_each_pair(first, last, std::forward<PairKernel>(pair_kernel),
                             LayeredMinimalImageDistance{box_geo},
                             std::forward<VerletCriterion>(verlet_criterion),
                             cell_structure.use_verlet_list,
                             rebuild_verletlist);
    break;
  }
}

/**
 * @brief Functor that returns true for
 *        any arguments.
 */
struct True {
  template <class... T> bool operator()(T...) const { return true; }
};

template <class T>
auto constexpr is_noop =
    std::is_same<Utils::NoOp, std::remove_reference_t<T>>::value;
} // namespace detail

template <class ParticleKernel, class PairKernel,
          class VerletCriterion = detail::True, class LongRange = Utils::NoOp>
void short_range_loop(ParticleKernel particle_kernel, PairKernel &&pair_kernel,
                      const VerletCriterion &verlet_criterion = {},
                      LongRange long_range = {}) {
  ESPRESSO_PROFILER_CXX_MARK_FUNCTION;
  using detail::is_noop;

  assert(get_resort_particles() == Cells::RESORT_NONE);

  if (not is_noop<ParticleKernel>) {
    for (auto &p : cell_structure.local_cells().particles()) {
      particle_kernel(p);
    }
  }

  if (not is_noop<PairKernel> && cell_structure.min_range != INACTIVE_CUTOFF) {
    auto first = boost::make_indirect_iterator(local_cells.begin());
    auto last = boost::make_indirect_iterator(local_cells.end());

    detail::decide_distance(first, last, Utils::NoOp{},
                            std::forward<PairKernel>(pair_kernel),
                            verlet_criterion);

    rebuild_verletlist = 0;
  }

  if (not is_noop<LongRange>) {
    long_range(cell_structure.local_cells());
  }
}

#endif

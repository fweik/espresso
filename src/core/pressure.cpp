/*
 * Copyright (C) 2010-2019 The ESPResSo project
 * Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
 *   Max-Planck-Institute for Polymer Research, Theory Group
 *
 * This file is part of ESPResSo.
 *
 * ESPResSo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ESPResSo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/** \file
 *  Implementation of pressure.hpp.
 */

#include "Observable_stat.hpp"
#include "cells.hpp"
#include "communication.hpp"
#include "event.hpp"
#include "pressure_inline.hpp"
#include "reduce_observable_stat.hpp"
#include "virtual_sites.hpp"

#include <boost/range/algorithm/copy.hpp>

#include "short_range_loop.hpp"

#include "electrostatics_magnetostatics/coulomb.hpp"
#include "electrostatics_magnetostatics/dipole.hpp"

nptiso_struct nptiso = {0.0,
                        0.0,
                        0.0,
                        0.0,
                        0.0,
                        0.0,
                        {0.0, 0.0, 0.0},
                        {0.0, 0.0, 0.0},
                        0,
                        {NPTGEOM_XDIR, NPTGEOM_YDIR, NPTGEOM_ZDIR},
                        0,
                        false,
                        0};

/** Pressure tensor of the system */
Observable_stat obs_pressure{9};

Observable_stat const &get_obs_pressure() { return obs_pressure; }

/** Calculate long-range virials (P3M, ...). */
void calc_long_range_virials(const ParticleRange &particles) {
#ifdef ELECTROSTATICS
  /* calculate k-space part of electrostatic interaction. */
  auto const coulomb_pressure = Coulomb::calc_pressure_long_range(particles);
  boost::copy(coulomb_pressure, obs_pressure.coulomb.begin() + 9);
#endif
#ifdef DIPOLES
  /* calculate k-space part of magnetostatic interaction. */
  Dipole::calc_pressure_long_range();
#endif
}

void pressure_calc() {
  auto const volume = box_geo.volume();

  if (!interactions_sanity_checks())
    return;

  obs_pressure = Observable_stat{9};

  on_observable_calc();

  for (auto const &p : cell_structure.local_particles()) {
    add_kinetic_virials(p, obs_pressure);
  }

  short_range_loop([](Particle &p) { add_bonded_virials(p, obs_pressure); },
                   [](Particle &p1, Particle &p2, Distance const &d) {
                     add_non_bonded_pair_virials(p1, p2, d.vec21, sqrt(d.dist2),
                                                 obs_pressure);
                   });

  calc_long_range_virials(cell_structure.local_particles());

#ifdef VIRTUAL_SITES
  if (!obs_pressure.virtual_sites.empty()) {
    auto const vs_pressure = virtual_sites()->pressure_tensor();
    boost::copy(flatten(vs_pressure), obs_pressure.virtual_sites.begin());
  }
#endif

  obs_pressure.rescale(volume);

  /* gather data */
  auto obs_pressure_res = reduce(comm_cart, obs_pressure);
  if (obs_pressure_res) {
    std::swap(obs_pressure, *obs_pressure_res);
  }
}

void update_pressure() { mpi_gather_stats(GatherStats::pressure); }

Utils::Vector9d observable_compute_pressure_tensor() {
  update_pressure();
  Utils::Vector9d pressure_tensor{};
  for (size_t j = 0; j < 9; j++) {
    pressure_tensor[j] = obs_pressure.accumulate(0, j);
  }
  return pressure_tensor;
}

/*
  Copyright (C) 2010-2018 The ESPResSo project
  Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
    Max-Planck-Institute for Polymer Research, Theory Group

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
#ifndef HERTZIAN_H
#define HERTZIAN_H

/** \file
 *  Routines to calculate the Hertzian energy and/or force
 *  for a particle pair.
 *  \ref forces.cpp
 */

#include "nonbonded_interaction_data.hpp"
#include "particle_data.hpp"

#ifdef HERTZIAN

///
int hertzian_set_params(int part_type_a, int part_type_b, double eps,
                        double sig);

/** Calculate Hertzian force between particle p1 and p2 */
inline void add_hertzian_pair_force(const Particle *const p1,
                                    Particle const *const p2,
                                    IA_parameters *ia_params, double const d[3],
                                    double dist, double dist2,
                                    double force[3]) {
  if ((dist < ia_params->Hertzian_sig)) {
    double fac = 5. / 2. * ia_params->Hertzian_eps / ia_params->Hertzian_sig *
                 pow(1 - dist / ia_params->Hertzian_sig, 3. / 2.) / dist;

    for (int j = 0; j < 3; j++)
      force[j] += fac * d[j];
  }
}

/** calculate Lennard-Jones energy between particle p1 and p2. */
inline double hertzian_pair_energy(const Particle *p1, const Particle *p2,
                                   const IA_parameters *ia_params,
                                   const double d[3], double dist,
                                   double dist2) {
  if ((dist < ia_params->Hertzian_sig)) {
    return ia_params->Hertzian_eps *
           pow(1 - dist / ia_params->Hertzian_sig, 5. / 2.);
  }
  return 0.0;
}

#endif /* ifdef HERTZIAN */
#endif

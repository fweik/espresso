/*
  Copyright (C) 2010,2012,2013,2014,2015,2016 The ESPResSo project
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
#ifndef CORE_TOPOLOGY_HPP
#define CORE_TOPOLOGY_HPP

/** \file topology.hpp
 *
 *  This file contains functions for handling the system topology.
 */

#include "ParticleRange.hpp"
#include "Vector.hpp"

#include <boost/mpi/communicator.hpp>

#include <vector>

/************************************************/
/** \name Data Types */
/************************************************/
/*@{*/

/** Structure holding information about a molecule */
struct Molecule {
  int mol_id = -1;
  int n_part = 0;
  Vector3d com;
  double mass;
};

/*@}*/

/************************************************************/
/** \name Exported Functions */
/************************************************************/
/*@{*/

void update_topology_info(const boost::mpi::communicator &comm,
                          const ParticleRange &particles);
const std::vector<Molecule> &topology_info();

/*@}*/

#endif

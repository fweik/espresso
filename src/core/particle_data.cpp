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
 *  Particles and particle lists.
 *
 *  The corresponding header file is particle_data.hpp.
 */
#include "particle_data.hpp"

#include "PartCfg.hpp"
#include "Particle.hpp"
#include "bonded_interactions/bonded_interaction_data.hpp"
#include "cells.hpp"
#include "communication.hpp"
#include "debug.hpp"
#include "event.hpp"
#include "global.hpp"
#include "grid.hpp"
#include "integrate.hpp"
#include "nonbonded_interactions/nonbonded_interaction_data.hpp"
#include "partCfg_global.hpp"
#include "random.hpp"
#include "rotation.hpp"
#include "serialization/ParticleList.hpp"
#include "virtual_sites.hpp"

#include <utils/Cache.hpp>
#include <utils/constants.hpp>
#include <utils/mpi/gatherv.hpp>

#include <boost/range/algorithm.hpp>
#include <boost/serialization/variant.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/variant.hpp>

#include <boost/range/numeric.hpp>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utils/keys.hpp>
/************************************************
 * defines
 ************************************************/

/** my magic MPI code for send/recv_particles */
#define REQ_SNDRCV_PART 0xaa

namespace {
/**
 * @brief A generic particle update.
 *
 * Here the sub-struct structure of Particle is
 * used: the specification of the data member to update
 * consists of two parts, the pointer to the substruct @p s
 * and a pointer to a member of that substruct @p m.
 *
 * @tparam S Substruct type of Particle
 * @tparam s Pointer to a member of Particle
 * @tparam T Type of the member to update, must be serializable
 * @tparam m Pointer to the member.
 */
template <typename S, S Particle::*s, typename T, T S::*m>
struct UpdateParticle {
  T value;

  void operator()(Particle &p) const { (p.*s).*m = value; }

  template <class Archive> void serialize(Archive &ar, long int) { ar &value; }
};

template <typename T, T ParticleProperties::*m>
using UpdateProperty = UpdateParticle<ParticleProperties, &Particle::p, T, m>;
template <typename T, T ParticlePosition ::*m>
using UpdatePosition = UpdateParticle<ParticlePosition, &Particle::r, T, m>;
template <typename T, T ParticleMomentum ::*m>
using UpdateMomentum = UpdateParticle<ParticleMomentum, &Particle::m, T, m>;
template <typename T, T ParticleForce ::*m>
using UpdateForce = UpdateParticle<ParticleForce, &Particle::f, T, m>;

using Prop = ParticleProperties;

// clang-format off
using UpdatePropertyMessage = boost::variant
        < UpdateProperty<int, &Prop::type>
        , UpdateProperty<int, &Prop::mol_id>
#ifdef MASS
        , UpdateProperty<double, &Prop::mass>
#endif
#ifdef SHANCHEN
        , UpdateProperty<std::array<double, 2 * LB_COMPONENTS>, &Prop::solvation>
#endif
#ifdef ROTATIONAL_INERTIA
        , UpdateProperty<Utils::Vector3d, &Prop::rinertia>
#endif
#ifdef ROTATION
        , UpdateProperty<uint8_t, &Prop::rotation>
#endif
#ifdef ELECTROSTATICS
        , UpdateProperty<double, &Prop::q>
#endif
#ifdef LB_ELECTROHYDRODYNAMICS
        , UpdateProperty<Utils::Vector3d, &Prop::mu_E>
#endif
#ifdef ENGINE
        , UpdateProperty<ParticleParametersSwimming, &Prop::swim>
#endif
#ifdef DIPOLES
        , UpdateProperty<double, &Prop::dipm>
#endif
#ifdef VIRTUAL_SITES
        , UpdateProperty<bool, &Prop::is_virtual>
#ifdef VIRTUAL_SITES_RELATIVE
        , UpdateProperty<ParticleProperties::VirtualSitesRelativeParameters,
                         &Prop::vs_relative>
#endif
#endif
#ifdef LANGEVIN_PER_PARTICLE
        , UpdateProperty<double, &Prop::T>
#ifndef PARTICLE_ANISOTROPY
        , UpdateProperty<double, &Prop::gamma>
#else
        , UpdateProperty<Utils::Vector3d, &Prop::gamma>
#endif // PARTICLE_ANISOTROPY
#ifdef ROTATION
#ifndef PARTICLE_ANISOTROPY
        , UpdateProperty<double, &Prop::gamma_rot>
#else
        , UpdateProperty<Utils::Vector3d, &Prop::gamma_rot>
#endif // PARTICLE_ANISOTROPY
#endif // ROTATION
#endif // LANGEVIN_PER_PARTICLE
#ifdef EXTERNAL_FORCES
        , UpdateProperty<uint8_t, &Prop::ext_flag>
        , UpdateProperty<Utils::Vector3d, &Prop::ext_force>
#ifdef ROTATION
        , UpdateProperty<Utils::Vector3d, &Prop::ext_torque>
#endif
#endif
        >;

using UpdatePositionMessage = boost::variant
        < UpdatePosition<Utils::Vector3d, &ParticlePosition::p>
#ifdef ROTATION
        , UpdatePosition<Utils::Vector4d, &ParticlePosition::quat>
#endif
        >;

using UpdateMomentumMessage = boost::variant
      < UpdateMomentum<Utils::Vector3d, &ParticleMomentum::v>
#ifdef ROTATION
      , UpdateMomentum<Utils::Vector3d, &ParticleMomentum::omega>
#endif
      >;

using UpdateForceMessage = boost::variant
      < UpdateForce<Utils::Vector3d, &ParticleForce::f>
#ifdef ROTATION
      , UpdateForce<Utils::Vector3d, &ParticleForce::torque>
#endif
      >;

/**
 * @brief Delete specific bond.
 */
struct RemoveBond {
    std::vector<int> bond;

    void operator()(Particle &p) const {
      try_delete_bond(&p, bond.data());
    }

    template<class Archive>
            void serialize(Archive &ar, long int) {
        ar & bond;
    }
};


/**
 * @brief Delete all bonds.
 */
struct RemoveBonds {
    void operator()(Particle &p) const {
      p.bl.clear();
    }

    template<class Archive>
    void serialize(Archive &ar, long int) {
    }
};

struct AddBond {
    std::vector<int> bond;

    void operator()(Particle &p) const {
        local_add_particle_bond(p, bond);
    }

    template<class Archive>
    void serialize(Archive &ar, long int) {
        ar & bond;
    }
};

using UpdateBondMessage = boost::variant
        < RemoveBond
        , RemoveBonds
        , AddBond
        >;

#ifdef ROTATION
struct UpdateOrientation {
    Utils::Vector3d axis;
    double angle;

    void operator()(Particle &p) const {
        local_rotate_particle(p, axis, angle);
    }

    template<class Archive>
    void serialize(Archive &ar, long int) {
        ar & axis & angle;
    }
};
#endif

/**
 * @brief Top-level message.
 *
 * A message is either updates a property,
 * or a position, or ...
 * New messages can be added here, if they
 * fulfill the type requirements, namely:
 * They either have an integer member id indicating
 * the particle that should be updated an operator()(const Particle&)
 * that is called with the particle, or a tree of
 * variants with leafs that have such an operator() and member.
 */
using UpdateMessage = boost::variant
        < UpdatePropertyMessage
        , UpdatePositionMessage
        , UpdateMomentumMessage
        , UpdateForceMessage
        , UpdateBondMessage
#ifdef ROTATION
        , UpdateOrientation
#endif
        >;
// clang-format on

/**
 * @brief Meta-function to detect the message type from
 *        the particle substruct.
 */
template <typename S, S Particle::*s> struct message_type;

template <> struct message_type<ParticleProperties, &Particle::p> {
  using type = UpdatePropertyMessage;
};

template <> struct message_type<ParticlePosition, &Particle::r> {
  using type = UpdatePositionMessage;
};

template <> struct message_type<ParticleMomentum, &Particle::m> {
  using type = UpdateMomentumMessage;
};

template <> struct message_type<ParticleForce, &Particle::f> {
  using type = UpdateForceMessage;
};

template <typename S, S Particle::*s>
using message_type_t = typename message_type<S, s>::type;

/**
 * @brief Visitor for message evaluation.
 *
 * This visitor either recurses into the active type
 * if it is a variant type, or otherwise calls
 * operator()(Particle&) on the active type with
 * the particle that ought to be updated. This construction
 * allows to nest message variants to allow for sub-
 * categories. Those are mostly used here to differentiate
 * the updates for the substructs of Particle.
 */
struct UpdateVisitor : public boost::static_visitor<void> {
  explicit UpdateVisitor(int id) : id(id) {}

  const int id;

  /* Recurse into sub-variants */
  template <class... Message>
  void operator()(const boost::variant<Message...> &msg) const {
    boost::apply_visitor(*this, msg);
  }
  /* Plain messages are just called. */
  template <typename Message> void operator()(const Message &msg) const {
    assert(get_local_particle_data(id));
    msg(*get_local_particle_data(id));
  }
};
} // namespace

/**
 * @brief Callback for \ref mpi_send_update_message.
 */
void mpi_update_particle_slave(int node, int id) {
  if (node == comm_cart.rank()) {
    UpdateMessage msg{};
    comm_cart.recv(0, SOME_TAG, msg);
    boost::apply_visitor(UpdateVisitor{id}, msg);
  }

  on_particle_change();
}

/**
 * @brief Send a particle update message.
 *
 * This sends the message to the node that is responsible for the particle,
 * where
 * @p msg is called with the particle as argument. The message then performs the
 * change to the particle that is encoded in it. The mechanism to call a functor
 * based on the active type of a variant is called visitation. Here we can use
 * @c UpdateVisitor with a single templated call operator on the visitor because
 * all message (eventually) provide the same interface. Overall this is
 * logically equivalent to nested switch statements over the message types,
 * where the case statements play the role of the call of the messages in our
 * case. A general introduction can be found in the documentation of
 * boost::variant.
 *
 * @param id Id of the particle to update
 * @param msg The message
 */
void mpi_send_update_message(int id, const UpdateMessage &msg) {
  auto const pnode = get_particle_node(id);

  mpi_call(mpi_update_particle_slave, pnode, id);

  /* If the particle is remote, send the
   * message to the target, otherwise we
   * can just apply the update directly. */
  if (pnode != comm_cart.rank()) {
    comm_cart.send(pnode, SOME_TAG, msg);
  } else {
    boost::apply_visitor(UpdateVisitor{id}, msg);
  }

  on_particle_change();
}

template <typename S, S Particle::*s, typename T, T S::*m>
void mpi_update_particle(int id, const T &value) {
  using MessageType = message_type_t<S, s>;
  MessageType msg = UpdateParticle<S, s, T, m>{value};
  mpi_send_update_message(id, msg);
}

template <typename T, T ParticleProperties::*m>
void mpi_update_particle_property(int id, const T &value) {
  mpi_update_particle<ParticleProperties, &Particle::p, T, m>(id, value);
}

/************************************************
 * variables
 ************************************************/
bool type_list_enable;
std::unordered_map<int, std::unordered_set<int>> particle_type_map{};
void remove_id_from_map(int part_id, int type);
void add_id_to_type_map(int part_id, int type);

int max_seen_particle = -1;
int n_part = 0;
/**
 * @brief id -> rank
 */
std::unordered_map<int, int> particle_node;

std::vector<Particle *> local_particles;

/************************************************
 * local functions
 ************************************************/

int try_delete_bond(Particle *part, const int *bond);

void try_delete_exclusion(Particle *part, int part2);

void try_add_exclusion(Particle *part, int part2);

void auto_exclusion(int distance);

/************************************************
 * particle initialization functions
 ************************************************/

void free_particle(Particle *part) { part->~Particle(); }

void mpi_who_has_slave(int, int) {
  static std::vector<int> sendbuf;
  int n_part;

  n_part = cells_get_n_particles();
  MPI_Gather(&n_part, 1, MPI_INT, nullptr, 0, MPI_INT, 0, comm_cart);
  if (n_part == 0)
    return;

  sendbuf.resize(n_part);

  auto end = std::transform(cell_structure.local_cells().particles().begin(),
                            cell_structure.local_cells().particles().end(),
                            sendbuf.data(),
                            [](Particle const &p) { return p.p.identity; });

  auto npart = std::distance(sendbuf.data(), end);
  MPI_Send(sendbuf.data(), npart, MPI_INT, 0, SOME_TAG, comm_cart);
}

void mpi_who_has(const ParticleRange &particles) {
  static auto *sizes = new int[n_nodes];
  std::vector<int> pdata;

  mpi_call(mpi_who_has_slave, -1, 0);

  int n_part = cells_get_n_particles();
  /* first collect number of particles on each node */
  MPI_Gather(&n_part, 1, MPI_INT, sizes, 1, MPI_INT, 0, comm_cart);

  /* then fetch particle locations */
  for (int pnode = 0; pnode < n_nodes; pnode++) {
    if (pnode == this_node) {
      for (auto const &p : particles)
        particle_node[p.p.identity] = this_node;

    } else if (sizes[pnode] > 0) {
      if (pdata.size() < sizes[pnode]) {
        pdata.resize(sizes[pnode]);
      }
      MPI_Recv(pdata.data(), sizes[pnode], MPI_INT, pnode, SOME_TAG, comm_cart,
               MPI_STATUS_IGNORE);
      for (int i = 0; i < sizes[pnode]; i++)
        particle_node[pdata[i]] = pnode;
    }
  }
}

/**
 * @brief Rebuild the particle index.
 */
void build_particle_node() {
  mpi_who_has(cell_structure.local_cells().particles());
}

/**
 *  @brief Get the mpi rank which owns the particle with id.
 */
int get_particle_node(int id) {
  if ((id < 0) or (id > get_maximal_particle_id()))
    throw std::runtime_error("Invalid particle id!");

  if (particle_node.empty())
    build_particle_node();

  auto const needle = particle_node.find(id);

  // Check if particle has a node, if not, we assume it does not exist.
  if (needle == particle_node.end()) {
    throw std::runtime_error("Particle node for id " + std::to_string(id) +
                             " not found!");
  }
  return needle->second;
}

void clear_particle_node() { particle_node.clear(); }

/************************************************
 * organizational functions
 ************************************************/

void update_local_particles(ParticleList *pl) {
  Particle *p = pl->part;
  int n = pl->n, i;
  for (i = 0; i < n; i++)
    set_local_particle_data(p[i].p.identity, &p[i]);
}

void append_unindexed_particle(ParticleList *l, Particle &&part) {
  l->resize(l->n + 1);
  new (&(l->part[l->n - 1])) Particle(std::move(part));
}

Particle *append_indexed_particle(ParticleList *l, Particle &&part) {
  auto const re = l->resize(l->n + 1);
  auto p = new (&(l->part[l->n - 1])) Particle(std::move(part));

  if (re)
    update_local_particles(l);
  else
    set_local_particle_data(p->p.identity, p);
  return p;
}

Particle *move_unindexed_particle(ParticleList *dl, ParticleList *sl, int i) {
  assert(sl->n > 0);
  assert(i < sl->n);

  dl->resize(dl->n + 1);
  auto dst = &dl->part[dl->n - 1];
  auto src = &sl->part[i];
  auto end = &sl->part[sl->n - 1];

  new (dst) Particle(std::move(*src));
  if (src != end) {
    new (src) Particle(std::move(*end));
  }

  sl->resize(sl->n - 1);
  return dst;
}

Particle *move_indexed_particle(ParticleList *dl, ParticleList *sl, int i) {
  assert(sl->n > 0);
  assert(i < sl->n);
  int re = dl->resize(dl->n + 1);
  Particle *dst = &dl->part[dl->n - 1];
  Particle *src = &sl->part[i];
  Particle *end = &sl->part[sl->n - 1];

  new (dst) Particle(std::move(*src));

  if (re) {
    update_local_particles(dl);
  } else {
    set_local_particle_data(dst->p.identity, dst);
  }
  if (src != end) {
    new (src) Particle(std::move(*end));
  }

  if (sl->resize(sl->n - 1)) {
    update_local_particles(sl);
  } else if (src != end) {
    set_local_particle_data(src->p.identity, src);
  }
  return dst;
}

/**
 * @brief Extract an indexed particle from a list.
 *
 * Removes a particle from a particle list and
 * from the particle index.
 *
 * @param i Index of particle to remove,
 *          needs to be valid.
 * @param sl List to remove the particle from,
 *           needs to be non-empty.
 * @return The extracted particle.
 */
Particle extract_indexed_particle(ParticleList *sl, int i) {
  assert(sl->n > 0);
  assert(i < sl->n);
  Particle *src = &sl->part[i];
  Particle *end = &sl->part[sl->n - 1];

  Particle p = std::move(*src);

  set_local_particle_data(p.p.identity, nullptr);

  if (src != end) {
    new (src) Particle(std::move(*end));
  }

  if (sl->resize(sl->n - 1)) {
    update_local_particles(sl);
  } else if (src != end) {
    set_local_particle_data(src->p.identity, src);
  }
  return p;
}

namespace {
/* Limit cache to 100 MiB */
std::size_t const max_cache_size = (100ul * 1048576ul) / sizeof(Particle);
Utils::Cache<int, Particle> particle_fetch_cache(max_cache_size);
} // namespace

void invalidate_fetch_cache() { particle_fetch_cache.invalidate(); }

boost::optional<const Particle &> get_particle_data_local(int id) {
  auto p = get_local_particle_data(id);

  if (p and (not p->l.ghost)) {
    return *p;
  }

  return {};
}

REGISTER_CALLBACK_ONE_RANK(get_particle_data_local)

const Particle &get_particle_data(int part) {
  auto const pnode = get_particle_node(part);

  if (pnode == this_node) {
    assert(get_local_particle_data(part));
    return *get_local_particle_data(part);
  }

  /* Query the cache */
  auto const p_ptr = particle_fetch_cache.get(part);
  if (p_ptr) {
    return *p_ptr;
  }

  /* Cache miss, fetch the particle,
   * put it into the cache and return a pointer into the cache. */
  auto const cache_ptr = particle_fetch_cache.put(
      part, Communication::mpiCallbacks().call(Communication::Result::one_rank,
                                               get_particle_data_local, part));
  return *cache_ptr;
}

void mpi_get_particles_slave(int, int) {
  std::vector<int> ids;
  boost::mpi::scatter(comm_cart, ids, 0);

  std::vector<Particle> parts(ids.size());
  std::transform(ids.begin(), ids.end(), parts.begin(), [](int id) {
    assert(get_local_particle_data(id));
    return *get_local_particle_data(id);
  });

  Utils::Mpi::gatherv(comm_cart, parts.data(), parts.size(), 0);
}

/**
 * @brief Get multiple particles at once.
 *
 * *WARNING* Particles are returned in an arbitrary order.
 *
 * @param ids The ids of the particles that should be returned.
 *
 * @returns The particle data.
 */
std::vector<Particle> mpi_get_particles(std::vector<int> const &ids) {
  mpi_call(mpi_get_particles_slave, 0, 0);
  /* Return value */
  std::vector<Particle> parts(ids.size());

  /* Group ids per node */
  std::vector<std::vector<int>> node_ids(comm_cart.size());
  for (auto const &id : ids) {
    auto const pnode = get_particle_node(id);

    node_ids[pnode].push_back(id);
  }

  /* Distributed ids to the nodes */
  {
    std::vector<int> ignore;
    boost::mpi::scatter(comm_cart, node_ids, ignore, 0);
  }

  /* Copy local particles */
  std::transform(node_ids[this_node].cbegin(), node_ids[this_node].cend(),
                 parts.begin(), [](int id) {
                   assert(get_local_particle_data(id));
                   return *get_local_particle_data(id);
                 });

  std::vector<int> node_sizes(comm_cart.size());
  std::transform(
      node_ids.cbegin(), node_ids.cend(), node_sizes.begin(),
      [](std::vector<int> const &ids) { return static_cast<int>(ids.size()); });

  Utils::Mpi::gatherv(comm_cart, parts.data(), parts.size(), parts.data(),
                      node_sizes.data(), 0);

  return parts;
}

void prefetch_particle_data(std::vector<int> ids) {
  /* Nothing to do on a single node. */
  if (comm_cart.size() == 1)
    return;

  /* Remove local, already cached and non-existent particles from the list. */
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [](int id) {
                             if (not particle_exists(id)) {
                               return true;
                             }
                             auto const pnode = get_particle_node(id);
                             return (pnode == this_node) ||
                                    particle_fetch_cache.has(id);
                           }),
            ids.end());

  /* Don't prefetch more particles than fit the cache. */
  if (ids.size() > particle_fetch_cache.max_size())
    ids.resize(particle_fetch_cache.max_size());

  /* Fetch the particles... */
  auto parts = mpi_get_particles(ids);

  /* mpi_get_particles does not return the parts in the correct
     order, so the ids need to be updated. */
  std::transform(parts.cbegin(), parts.cend(), ids.begin(),
                 [](Particle const &p) { return p.identity(); });

  /* ... and put them into the cache. */
  particle_fetch_cache.put(ids.cbegin(), ids.cend(),
                           std::make_move_iterator(parts.begin()));
}

int place_particle(int part, const double *pos) {
  Utils::Vector3d p{pos[0], pos[1], pos[2]};

  if (particle_exists(part)) {
    mpi_place_particle(get_particle_node(part), part, p);

    return ES_PART_OK;
  }
  particle_node[part] = mpi_place_new_particle(part, p);

  return ES_PART_CREATED;
}

void set_particle_v(int part, double *v) {
  mpi_update_particle<ParticleMomentum, &Particle::m, Utils::Vector3d,
                      &ParticleMomentum::v>(part, Utils::Vector3d(v, v + 3));
}

#ifdef ENGINE
void set_particle_swimming(int part, ParticleParametersSwimming swim) {
  mpi_update_particle_property<ParticleParametersSwimming,
                               &ParticleProperties::swim>(part, swim);
}
#endif

void set_particle_f(int part, const Utils::Vector3d &F) {
  mpi_update_particle<ParticleForce, &Particle::f, Utils::Vector3d,
                      &ParticleForce::f>(part, F);
}

#if defined(MASS)
void set_particle_mass(int part, double mass) {
  mpi_update_particle_property<double, &ParticleProperties::mass>(part, mass);
}
#else
const constexpr double ParticleProperties::mass;
#endif

#ifdef ROTATIONAL_INERTIA
void set_particle_rotational_inertia(int part, double *rinertia) {
  mpi_update_particle_property<Utils::Vector3d, &ParticleProperties::rinertia>(
      part, Utils::Vector3d(rinertia, rinertia + 3));
}
#else
constexpr Utils::Vector3d ParticleProperties::rinertia;
#endif
#ifdef ROTATION
void set_particle_rotation(int part, int rot) {
  mpi_update_particle_property<uint8_t, &ParticleProperties::rotation>(part,
                                                                       rot);
}
#endif
#ifdef ROTATION
void rotate_particle(int part, const Utils::Vector3d &axis, double angle) {
  mpi_send_update_message(part, UpdateOrientation{axis, angle});
}
#endif

#ifdef DIPOLES
void set_particle_dipm(int part, double dipm) {
  mpi_update_particle_property<double, &ParticleProperties::dipm>(part, dipm);
}

void set_particle_dip(int part, double const *const dip) {
  Utils::Vector4d quat;
  double dipm;
  std::tie(quat, dipm) =
      convert_dip_to_quat(Utils::Vector3d({dip[0], dip[1], dip[2]}));

  set_particle_dipm(part, dipm);
  set_particle_quat(part, quat.data());
}

#endif

#ifdef VIRTUAL_SITES
void set_particle_virtual(int part, bool is_virtual) {
  mpi_update_particle_property<bool, &ParticleProperties::is_virtual>(
      part, is_virtual);
}
#endif

#ifdef VIRTUAL_SITES_RELATIVE
void set_particle_vs_quat(int part, Utils::Vector4d const &vs_relative_quat) {
  auto vs_relative = get_particle_data(part).p.vs_relative;
  vs_relative.quat = vs_relative_quat;

  mpi_update_particle_property<
      ParticleProperties::VirtualSitesRelativeParameters,
      &ParticleProperties::vs_relative>(part, vs_relative);
}

void set_particle_vs_relative(int part, int vs_relative_to, double vs_distance,
                              Utils::Vector4d const &rel_ori) {
  ParticleProperties::VirtualSitesRelativeParameters vs_relative;
  vs_relative.distance = vs_distance;
  vs_relative.to_particle_id = vs_relative_to;
  vs_relative.rel_orientation = rel_ori;

  mpi_update_particle_property<
      ParticleProperties::VirtualSitesRelativeParameters,
      &ParticleProperties::vs_relative>(part, vs_relative);
}
#endif

void set_particle_q(int part, double q) {
#ifdef ELECTROSTATICS
  mpi_update_particle_property<double, &ParticleProperties::q>(part, q);
#endif
}

#ifndef ELECTROSTATICS
const constexpr double ParticleProperties::q;
#endif

#ifdef LB_ELECTROHYDRODYNAMICS
void set_particle_mu_E(int part, double *mu_E) {
  mpi_update_particle_property<Utils::Vector3d, &ParticleProperties::mu_E>(
      part, Utils::Vector3d(mu_E, mu_E + 3));
}

void get_particle_mu_E(int part, double (&mu_E)[3]) {
  auto const &p = get_particle_data(part);

  for (int i = 0; i < 3; i++) {
    mu_E[i] = p.p.mu_E[i];
  }
}
#endif

void set_particle_type(int p_id, int type) {
  make_particle_type_exist(type);

  if (type_list_enable) {
    // check if the particle exists already and the type is changed, then remove
    // it from the list which contains it
    auto const &cur_par = get_particle_data(p_id);
    int prev_type = cur_par.p.type;
    if (prev_type != type) {
      // particle existed before so delete it from the list
      remove_id_from_map(p_id, prev_type);
    }
    add_id_to_type_map(p_id, type);
  }

  mpi_update_particle_property<int, &ParticleProperties::type>(p_id, type);
}

void set_particle_mol_id(int part, int mid) {
  mpi_update_particle_property<int, &ParticleProperties::mol_id>(part, mid);
}

#ifdef ROTATION
void set_particle_quat(int part, double *quat) {
  mpi_update_particle<ParticlePosition, &Particle::r, Utils::Vector4d,
                      &ParticlePosition::quat>(part,
                                               Utils::Vector4d(quat, quat + 4));
}

void set_particle_omega_lab(int part, const Utils::Vector3d &omega_lab) {
  auto const &particle = get_particle_data(part);

  mpi_update_particle<ParticleMomentum, &Particle::m, Utils::Vector3d,
                      &ParticleMomentum::omega>(
      part, convert_vector_space_to_body(particle, omega_lab));
}

void set_particle_omega_body(int part, const Utils::Vector3d &omega) {
  mpi_update_particle<ParticleMomentum, &Particle::m, Utils::Vector3d,
                      &ParticleMomentum::omega>(part, omega);
}

void set_particle_torque_lab(int part, const Utils::Vector3d &torque_lab) {
  auto const &particle = get_particle_data(part);

  mpi_update_particle<ParticleForce, &Particle::f, Utils::Vector3d,
                      &ParticleForce::torque>(
      part, convert_vector_space_to_body(particle, torque_lab));
}
#endif

#ifdef LANGEVIN_PER_PARTICLE
void set_particle_temperature(int part, double T) {
  mpi_update_particle_property<double, &ParticleProperties::T>(part, T);
}

#ifndef PARTICLE_ANISOTROPY
void set_particle_gamma(int part, double gamma) {
  mpi_update_particle_property<double, &ParticleProperties::gamma>(part, gamma);
}
#else
void set_particle_gamma(int part, Utils::Vector3d gamma) {
  mpi_update_particle_property<Utils::Vector3d, &ParticleProperties::gamma>(
      part, gamma);
}
#endif // PARTICLE_ANISOTROPY

#ifdef ROTATION
#ifndef PARTICLE_ANISOTROPY
void set_particle_gamma_rot(int part, double gamma_rot) {
  mpi_update_particle_property<double, &ParticleProperties::gamma_rot>(
      part, gamma_rot);
}
#else
void set_particle_gamma_rot(int part, Utils::Vector3d gamma_rot) {
  mpi_update_particle_property<Utils::Vector3d, &ParticleProperties::gamma_rot>(
      part, gamma_rot);
}
#endif // PARTICLE_ANISOTROPY
#endif // ROTATION
#endif // LANGEVIN_PER_PARTICLE

#ifdef EXTERNAL_FORCES
#ifdef ROTATION
void set_particle_ext_torque(int part, const Utils::Vector3d &torque) {
  mpi_update_particle_property<Utils::Vector3d,
                               &ParticleProperties::ext_torque>(part, torque);
}
#endif

void set_particle_ext_force(int part, const Utils::Vector3d &force) {
  mpi_update_particle_property<Utils::Vector3d, &ParticleProperties::ext_force>(
      part, force);
}

void set_particle_fix(int part, uint8_t flag) {
  mpi_update_particle_property<uint8_t, &ParticleProperties::ext_flag>(part,
                                                                       flag);
}
#endif

void delete_particle_bond(int part, Utils::Span<const int> bond) {
  mpi_send_update_message(
      part, UpdateBondMessage{RemoveBond{{bond.begin(), bond.end()}}});
}

void delete_particle_bonds(int part) {
  mpi_send_update_message(part, UpdateBondMessage{RemoveBonds{}});
}

void add_particle_bond(int part, Utils::Span<const int> bond) {
  mpi_send_update_message(
      part, UpdateBondMessage{AddBond{{bond.begin(), bond.end()}}});
}

void remove_all_particles() {
  mpi_remove_particle(-1, -1);
  clear_particle_node();
}

int remove_particle(int p_id) {
  auto const &cur_par = get_particle_data(p_id);
  if (type_list_enable) {
    // remove particle from its current type_list
    int type = cur_par.p.type;
    remove_id_from_map(p_id, type);
  }

  auto const pnode = get_particle_node(p_id);

  particle_node[p_id] = -1;
  mpi_remove_particle(pnode, p_id);

  particle_node.erase(p_id);

  if (p_id == get_maximal_particle_id()) {
    max_seen_particle--;
    mpi_bcast_parameter(FIELD_MAXPART);
  }
  return ES_OK;
}

/**
 * @brief Remove all bonds on particle involing other particle.
 *
 * @param p Particle whose bond list is modified.
 * @param id Bonds involving this id are removed.
 */
static void remove_all_bonds_to(Particle &p, int id) {
  IntList *bl = &p.bl;
  int i, j, partners;

  for (i = 0; i < bl->n;) {
    partners = bonded_ia_params[bl->e[i]].num;
    for (j = 1; j <= partners; j++)
      if (bl->e[i + j] == id)
        break;
    if (j <= partners) {
      bl->erase(bl->begin() + i, bl->begin() + i + 1 + partners);
    } else
      i += 1 + partners;
  }
  assert(i == bl->n);
}

void remove_all_bonds_to(int identity) {
  for (auto &p : cell_structure.local_cells().particles()) {
    remove_all_bonds_to(p, identity);
  }
}

void local_remove_particle(int part) {
  Cell *cell = nullptr;
  int position = -1;
  for (auto c : cell_structure.local_cells()) {
    for (int i = 0; i < c->n; i++) {
      auto &p = c->part[i];

      if (p.identity() == part) {
        cell = c;
        position = i;
      } else {
        remove_all_bonds_to(p, part);
      }
    }
  }

  assert(cell && (position >= 0));

  extract_indexed_particle(cell, position);
}

Particle *local_place_particle(int id, const Utils::Vector3d &pos, int _new) {
  auto pp = Utils::Vector3d{pos[0], pos[1], pos[2]};
  auto i = Utils::Vector3i{};
  fold_position(pp, i, box_geo);

  if (_new) {
    Particle new_part;
    new_part.p.identity = id;
    new_part.r.p = pp;
    new_part.l.i = i;

    /* allocate particle anew */
    auto cell = cell_structure.particle_to_cell(new_part);
    if (!cell) {
      return nullptr;
    }

    return append_indexed_particle(cell, std::move(new_part));
  }

  auto pt = get_local_particle_data(id);
  pt->r.p = pp;
  pt->l.i = i;

  return pt;
}

void local_remove_all_particles() {
  Cell *cell;
  int c;
  n_part = 0;
  max_seen_particle = -1;
  std::fill(local_particles.begin(), local_particles.end(), nullptr);

  for (c = 0; c < cell_structure.local_cells().n; c++) {
    Particle *p;
    int i, np;
    cell = cell_structure.local_cells().cell[c];
    p = cell->part;
    np = cell->n;
    for (i = 0; i < np; i++)
      free_particle(&p[i]);
    cell->n = 0;
  }
}

void local_rescale_particles(int dir, double scale) {
  for (auto &p : cell_structure.local_cells().particles()) {
    if (dir < 3)
      p.r.p[dir] *= scale;
    else {
      p.r.p *= scale;
    }
  }
}

void added_particle(int part) {
  n_part++;

  if (part > max_seen_particle) {
    max_seen_particle = part;
  }
}

void local_add_particle_bond(Particle &p, Utils::Span<const int> bond) {
  boost::copy(bond, std::back_inserter(p.bl));
}

int try_delete_bond(Particle *part, const int *bond) {
  IntList *bl = &part->bl;
  int i, j, type, partners;

  // Empty bond means: delete all bonds
  if (!bond) {
    bl->clear();

    return ES_OK;
  }

  // Go over the bond list to find the bond to delete
  for (i = 0; i < bl->n;) {
    type = bl->e[i];
    partners = bonded_ia_params[type].num;

    // If the bond type does not match the one, we want to delete, skip
    if (type != bond[0])
      i += 1 + partners;
    else {
      // Go over the bond partners
      for (j = 1; j <= partners; j++) {
        // Leave the loop early, if the bond to delete and the bond with in the
        // particle don't match
        if (bond[j] != bl->e[i + j])
          break;
      }
      // If we did not exit from the loop early, all parameters matched
      // and we go on with deleting
      if (j > partners) {
        // New length of bond list
        bl->erase(bl->begin() + i, bl->begin() + i + 1 + partners);

        return ES_OK;
      }
      i += 1 + partners;
    }
  }
  return ES_ERROR;
}

#ifdef EXCLUSIONS
void local_change_exclusion(int part1, int part2, int _delete) {
  if (part1 == -1 && part2 == -1) {
    for (auto &p : cell_structure.local_cells().particles()) {
      p.el.clear();
    }

    return;
  }

  /* part1, if here */
  auto part = get_local_particle_data(part1);
  if (part) {
    if (_delete)
      try_delete_exclusion(part, part2);
    else
      try_add_exclusion(part, part2);
  }

  /* part2, if here */
  part = get_local_particle_data(part2);
  if (part) {
    if (_delete)
      try_delete_exclusion(part, part1);
    else
      try_add_exclusion(part, part1);
  }
}

void try_add_exclusion(Particle *part, int part2) {
  for (int i = 0; i < part->el.n; i++)
    if (part->el.e[i] == part2)
      return;

  part->el.push_back(part2);
}

void try_delete_exclusion(Particle *part, int part2) {
  IntList &el = part->el;

  if (!el.empty()) {
    el.erase(std::remove(el.begin(), el.end(), part2), el.end());
  };
}
#endif

#ifdef EXCLUSIONS
namespace {
/* keep a unique list for particle i. Particle j is only added if it is not i
   and not already in the list. */
void add_partner(IntList *il, int i, int j, int distance) {
  int k;
  if (j == i)
    return;
  for (k = 0; k < il->n; k += 2)
    if (il->e[k] == j)
      return;

  il->push_back(j);
  il->push_back(distance);
}
} // namespace

int change_exclusion(int part1, int part2, int _delete) {
  if (particle_exists(part1) && particle_exists(part2)) {
    mpi_send_exclusion(part1, part2, _delete);
    return ES_OK;
  }
  return ES_ERROR;
}

void remove_all_exclusions() { mpi_send_exclusion(-1, -1, 1); }

void auto_exclusions(int distance) {
  /* partners is a list containing the currently found excluded particles for
     each particle, and their distance, as an interleaved list */
  std::unordered_map<int, IntList> partners;

  /* We need bond information */
  partCfg().update_bonds();

  /* determine initial connectivity */
  for (auto const &part1 : partCfg()) {
    auto const p1 = part1.p.identity;
    for (int i = 0; i < part1.bl.n;) {
      Bonded_ia_parameters const &ia_params = bonded_ia_params[part1.bl.e[i++]];
      if (ia_params.num == 1) {
        auto const p2 = part1.bl.e[i++];
        /* you never know what the user does, may bond a particle to itself...?
         */
        if (p2 != p1) {
          add_partner(&partners[p1], p1, p2, 1);
          add_partner(&partners[p2], p2, p1, 1);
        }
      } else
        i += ia_params.num;
    }
  }

  /* calculate transient connectivity. For each of the current neighbors,
     also exclude their close enough neighbors.
  */
  for (int count = 1; count < distance; count++) {
    for (auto const &p : partCfg()) {
      auto const p1 = p.identity();
      for (int i = 0; i < partners[p1].n; i += 2) {
        auto const p2 = partners[p1].e[i];
        auto const dist1 = partners[p1].e[i + 1];
        if (dist1 > distance)
          continue;
        /* loop over all partners of the partner */
        for (int j = 0; j < partners[p2].n; j += 2) {
          auto const p3 = partners[p2].e[j];
          auto const dist2 = dist1 + partners[p2].e[j + 1];
          if (dist2 > distance)
            continue;
          add_partner(&partners[p1], p1, p3, dist2);
          add_partner(&partners[p3], p3, p1, dist2);
        }
      }
    }
  }

  /* setup the exclusions and clear the arrays. We do not setup the exclusions
     up there, since on_part_change clears the partCfg, so that we would have to
     restore it continuously. Of course this could be optimized by bundling the
     exclusions, but this is only done once and the overhead is as much as for
     setting the bonds, which the user apparently accepted.
  */
  for (auto &p : partCfg()) {
    auto const id = p.identity();
    for (int j = 0; j < partners[id].n; j++)
      if (id < partners[id].e[j])
        change_exclusion(id, partners[id].e[j], 0);
  }
}
#endif

void init_type_map(int type) {
  type_list_enable = true;
  if (type < 0)
    throw std::runtime_error("Types may not be negative");

  // fill particle map
  if (particle_type_map.count(type) == 0)
    particle_type_map[type] = std::unordered_set<int>();

  for (auto const &p : partCfg()) {
    if (p.p.type == type)
      particle_type_map.at(type).insert(p.p.identity);
  }
}

void remove_id_from_map(int part_id, int type) {
  if (particle_type_map.find(type) != particle_type_map.end())
    particle_type_map.at(type).erase(part_id);
}

int get_random_p_id(int type, int random_index_in_type_map) {
  if (random_index_in_type_map + 1 > particle_type_map.at(type).size())
    throw std::runtime_error("The provided index exceeds the number of "
                             "particle types listed in the particle_type_map");
  return *std::next(particle_type_map[type].begin(), random_index_in_type_map);
}

void add_id_to_type_map(int part_id, int type) {
  if (particle_type_map.find(type) != particle_type_map.end())
    particle_type_map.at(type).insert(part_id);
}

int number_of_particles_with_type(int type) {
  if (particle_type_map.count(type) == 0)
    throw std::runtime_error("The provided particle type does not exist in "
                             "the particle_type_map");
  return static_cast<int>(particle_type_map.at(type).size());
}

// The following functions are used by the python interface to obtain
// properties of a particle, which are only compiled in in some configurations
// This is needed, because cython does not support conditional compilation
// within a ctypedef definition

#ifdef ROTATION
void pointer_to_omega_body(Particle const *p, double const *&res) {
  res = p->m.omega.data();
}

void pointer_to_quat(Particle const *p, double const *&res) {
  res = p->r.quat.data();
}

#endif

void pointer_to_q(Particle const *p, double const *&res) { res = &(p->p.q); }

#ifdef VIRTUAL_SITES
void pointer_to_virtual(Particle const *p, bool const *&res) {
  res = &(p->p.is_virtual);
}
#endif

#ifdef VIRTUAL_SITES_RELATIVE
void pointer_to_vs_quat(Particle const *p, double const *&res) {
  res = (p->p.vs_relative.quat.data());
}

void pointer_to_vs_relative(Particle const *p, int const *&res1,
                            double const *&res2, double const *&res3) {
  res1 = &(p->p.vs_relative.to_particle_id);
  res2 = &(p->p.vs_relative.distance);
  res3 = p->p.vs_relative.rel_orientation.data();
}
#endif

#ifdef DIPOLES

void pointer_to_dipm(Particle const *p, double const *&res) {
  res = &(p->p.dipm);
}
#endif

#ifdef EXTERNAL_FORCES
void pointer_to_ext_force(Particle const *p, double const *&res2) {
  res2 = p->p.ext_force.data();
}
#ifdef ROTATION
void pointer_to_ext_torque(Particle const *p, double const *&res2) {
  res2 = p->p.ext_torque.data();
}
#endif
void pointer_to_fix(Particle const *p, const uint8_t *&res) {
  res = &(p->p.ext_flag);
}
#endif

#ifdef LANGEVIN_PER_PARTICLE
void pointer_to_gamma(Particle const *p, double const *&res) {
#ifndef PARTICLE_ANISOTROPY
  res = &(p->p.gamma);
#else
  res = p->p.gamma.data(); // array [3]
#endif // PARTICLE_ANISTROPY
}

#ifdef ROTATION
void pointer_to_gamma_rot(Particle const *p, double const *&res) {
#ifndef PARTICLE_ANISOTROPY
  res = &(p->p.gamma_rot);
#else
  res = p->p.gamma_rot.data(); // array [3]
#endif // ROTATIONAL_INERTIA
}
#endif // ROTATION

void pointer_to_temperature(Particle const *p, double const *&res) {
  res = &(p->p.T);
}
#endif // LANGEVIN_PER_PARTICLE

#ifdef ENGINE
void pointer_to_swimming(Particle const *p,
                         ParticleParametersSwimming const *&swim) {
  swim = &(p->p.swim);
}
#endif

#ifdef ROTATIONAL_INERTIA
void pointer_to_rotational_inertia(Particle const *p, double const *&res) {
  res = p->p.rinertia.data();
}
#endif

bool particle_exists(int part_id) {
  if (particle_node.empty())
    build_particle_node();
  return particle_node.count(part_id);
}

std::vector<int> get_particle_ids() {
  if (particle_node.empty())
    build_particle_node();

  auto ids = Utils::keys(particle_node);
  boost::sort(ids);

  return ids;
}

int get_maximal_particle_id() {
  if (particle_node.empty())
    build_particle_node();

  return boost::accumulate(particle_node, -1,
                           [](int max, const std::pair<int, int> &kv) {
                             return std::max(max, kv.first);
                           });
}
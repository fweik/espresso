
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

#ifndef __UTILS_FACTORY_HPP
#define __UTILS_FACTORY_HPP

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace Utils {

/**
 * @brief Factory template.
 *
 * Can be used to construct registered instances of classes derived
 * from the base type (`T`) by name.
 * One registry per base type (`T`). To get a new one,
 * use new type ( `struct NewT : public T {};` ).
 * To add a new type it has to be given a name an a function of type
 * `%Factory<T>::%builder_type` to create an instance has to be provided.
 * The class contains a default implementation for the creation
 * function (`%Factory<T>::%builder<Derived>`) which just calls
 * new to create an instance. A user provided function could
 * be used to use a non-default constructor, or to allocate memory
 * for the instance in a specific way, e.g. by placing all new instances
 * in a vector.
 *
 * Example usage:
 * @code{.cpp}
 *     struct A {};
 *     struct B : public A {};
 *     struct C : public A {
 *       C(int c) : m_c(c) {}
 *       int m_c;
 *     };
 *
 *     // Register B as 'b' with default builder:
 *     Factory<A>::register_new<B>("b");
 *     // Register C as 'c' with user_defined builder:
 *     Factory<A>::register_new("c", []() -> typename Factory<A>::pointer_type {
 *         return new C(5); });
 *
 *     // Create a B
 *     auto b = Factory<A>::make("b");
 *     assert(dynamic_cast<B *>(b.get()));
 *
 *     // Create a C
 *     auto c = Factory<A>::make("c");
 *     assert(dynamic_cast<C *>(c.get())->m_c == 5);
 * @endcode
 */
template <
    /** The base type of the instances created by this factory */
    class T>
class Factory {
public:
  /** The returned pointer type */
  using pointer_type = std::unique_ptr<T>;
  /** Type of the constructor functions */
  using builder_type = std::function<T *()>;

public:
  /**
   * @brief Construct an instance by name.
   */
  pointer_type make(const std::string &name) const {
    try {
      auto builder = m_map.at(name);
      return assert(builder), pointer_type(builder());
    } catch (std::out_of_range const &) {
      throw std::domain_error("Class '" + name + "' not found.");
    }
  }

  /**
   * @brief Check if the factory knows how to make `name`.
   *
   * @param name Given name to check.
   * @return Whether we know how to make a `name`.
   */
  bool has_builder(const std::string &name) const {
    return not(m_map.find(name) == m_map.end());
  }

  /**
   * @brief Register a new type.
   *
   * @param name Given name for the type, has to be unique in this Factory<T>.
   * @param b Function to create an instance.
   */
  void register_new(const std::string &name, const builder_type &b) {
    m_map[name] = b;
  }

  /**
   * @brief Register a new type with the default construction function.
   *
   * @param name Given name for the type, has to be unique in this Factory<T>.
   */
  template <typename Derived> void register_new(const std::string &name) {
    register_new(name, []() -> T * { return new Derived(); });
  }

private:
  /** Maps names to construction functions. */
  std::unordered_map<std::string, builder_type> m_map;
};

} /* namespace Utils */

#endif

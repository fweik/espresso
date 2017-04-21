/*
  Copyright (C) 2017 The ESPResSo project

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

/** \file int_pow_test.cpp Unit tests for the
 * Utils::int_pow function.
*/

#include <iostream>
#include <limits>
#include <type_traits>

#include <cmath>

#define BOOST_TEST_MODULE int_pow test
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "utils/math/int_pow.hpp"
using Utils::int_pow;

BOOST_AUTO_TEST_CASE(even) {
  const double x = 3.14159;

  /* We check here for bitwise identity on
     purpose: The function should produce
     exactly the same expressions. */
  BOOST_CHECK(1 == int_pow<0>(x));
  BOOST_CHECK(x * x == int_pow<2>(x));
  /* Brackets are important to get the same
   * order of operations as in the squaring tree. */
  BOOST_CHECK((x * x) * (x * x) == int_pow<4>(x));
}

BOOST_AUTO_TEST_CASE(odd) {
  const double x = 3.14159;

  BOOST_CHECK(x == int_pow<1>(x));
  BOOST_CHECK((x * x) * x == int_pow<3>(x));
  BOOST_CHECK((x * x) * (x * x) * x == int_pow<5>(x));
}

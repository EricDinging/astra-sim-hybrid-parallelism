/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_HILBERTCURVE_HH__
#define __ASTRASIM_SCHEDULING_HILBERTCURVE_HH__

#include <cstdint>

namespace AstraSim {
namespace Scheduling {

// 3-D Hilbert curve in a 2^p x 2^p x 2^p cube using Skilling's 2004
// algorithm. Pack convention matches the Python `hilbertcurve` package
// (galtay/hilbertcurve, v2.0.5) so parity tests against the Python
// reference produce identical orderings.
//
// Requires 0 <= x, y, z < 2^p and 0 <= p <= 21 (so the distance fits in
// uint64_t: 3 * 21 = 63 bits).

uint64_t hilbert_d_from_xyz(int x, int y, int z, int p);

}  // namespace Scheduling
}  // namespace AstraSim

#endif

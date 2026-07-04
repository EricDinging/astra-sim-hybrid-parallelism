/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/
#include "astra-sim/system/astraccl/native_collectives/logical_topology/RingTopology.hh"

#include <gtest/gtest.h>
#include <vector>

using AstraSim::RingTopology;
using Dir = RingTopology::Direction;

// Identity ring [0..8): ctor 2 with offset==1 and index_in_ring==id triggers
// the fast-path. Neighbors and index must match a plain consecutive ring.
TEST(RingTopologyIdentity, IdentityRingNeighborsAndIndex) {
    for (int id = 0; id < 8; ++id) {
        RingTopology r(RingTopology::Dimension::NA, id, /*total=*/8,
                       /*index_in_ring=*/id, /*offset=*/1);
        EXPECT_EQ(r.get_index_in_ring(), id);
        EXPECT_EQ(r.get_nodes_in_ring(), 8);
        for (int n = 0; n < 8; ++n) {
            EXPECT_EQ(r.get_receiver(n, Dir::Clockwise), (n + 1) % 8);
            EXPECT_EQ(r.get_receiver(n, Dir::Anticlockwise), (n + 7) % 8);
        }
    }
}

// Non-contiguous ring via ctor 1 must NOT take the fast-path; the maps drive
// the (permuted) neighbor relation.
TEST(RingTopologyIdentity, NonContiguousUsesMaps) {
    // ring order: 5 -> 6 -> 7 -> 4 -> (wrap) 5
    RingTopology r(RingTopology::Dimension::NA, /*id=*/7,
                   std::vector<int>{5, 6, 7, 4});
    EXPECT_EQ(r.get_nodes_in_ring(), 4);
    EXPECT_EQ(r.get_index_in_ring(), 2);  // id 7 sits at index 2
    EXPECT_EQ(r.get_receiver(5, Dir::Clockwise), 6);
    EXPECT_EQ(r.get_receiver(7, Dir::Clockwise), 4);
    EXPECT_EQ(r.get_receiver(4, Dir::Clockwise), 5);
}

// Contiguous NPUs via ctor 1 take the fast-path and stay correct.
TEST(RingTopologyIdentity, Ctor1ContiguousIsIdentity) {
    RingTopology r(RingTopology::Dimension::NA, /*id=*/3,
                   std::vector<int>{0, 1, 2, 3, 4, 5});
    EXPECT_EQ(r.get_index_in_ring(), 3);
    EXPECT_EQ(r.get_receiver(3, Dir::Clockwise), 4);
    EXPECT_EQ(r.get_receiver(5, Dir::Clockwise), 0);
}

// Ctor 2 SLOW path: offset==1 but index_in_ring != id -- a shifted sub-ring
// {4,5,6,7} (base=id-index=4), as a torus dimension produces. The identity
// fast-path must NOT fire; the maps drive the wrap. This pins the load-bearing
// `index_in_ring == id` conjunct of the guard: if the fast-path wrongly fired,
// get_receiver(7, CW) would be 8 instead of 4.
TEST(RingTopologyIdentity, Ctor2ShiftedRingUsesMaps) {
    RingTopology r(RingTopology::Dimension::NA, /*id=*/5, /*total=*/4,
                   /*index_in_ring=*/1, /*offset=*/1);
    EXPECT_EQ(r.get_nodes_in_ring(), 4);
    EXPECT_EQ(r.get_index_in_ring(), 1);
    EXPECT_EQ(r.get_receiver(5, Dir::Clockwise), 6);
    EXPECT_EQ(r.get_receiver(7, Dir::Clockwise), 4);
    EXPECT_EQ(r.get_receiver(4, Dir::Clockwise), 5);
}

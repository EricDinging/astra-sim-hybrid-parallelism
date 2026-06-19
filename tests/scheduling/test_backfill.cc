/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/Backfill.hh"

#include <gtest/gtest.h>

#include <vector>

namespace {

using AstraSim::Tick;
using AstraSim::Scheduling::backfill_safe;
using AstraSim::Scheduling::compute_reservation;
using AstraSim::Scheduling::Reservation;
using AstraSim::Scheduling::RunningJob;

// --- compute_reservation ---------------------------------------------------

TEST(Backfill, ReservationWorkedExample) {
    // pivot needs 8, free_now=2; jobs end t=10 (4 ranks), t=25 (4 ranks).
    // After t=10 avail=6 (<8); after t=25 avail=10 (>=8) -> shadow=25, extra=2.
    std::vector<RunningJob> running = {{10, 4}, {25, 4}};
    Reservation r = compute_reservation(/*now=*/0, /*k_head=*/8,
                                        /*free_now=*/2, running);
    EXPECT_TRUE(r.exists);
    EXPECT_EQ(r.shadow_time, 25ull);
    EXPECT_EQ(r.extra, 2);
}

TEST(Backfill, ReservationFreeNowAlreadyEnough) {
    // Head DEFERed on shape, not count: free_now >= k_head -> shadow=now.
    std::vector<RunningJob> running = {{100, 4}};
    Reservation r = compute_reservation(/*now=*/7, /*k_head=*/2,
                                        /*free_now=*/5, running);
    EXPECT_TRUE(r.exists);
    EXPECT_EQ(r.shadow_time, 7ull);
    EXPECT_EQ(r.extra, 3);
}

TEST(Backfill, ReservationUnsortedInputSameResult) {
    std::vector<RunningJob> running = {{25, 4}, {10, 4}};  // reversed
    Reservation r = compute_reservation(0, 8, 2, running);
    EXPECT_TRUE(r.exists);
    EXPECT_EQ(r.shadow_time, 25ull);
    EXPECT_EQ(r.extra, 2);
}

TEST(Backfill, ReservationTiedEndTimesResultInvariant) {
    // Three jobs end at t=10 ranks 2,2,2; k_head=3, free_now=0. Crossing at the
    // second job: avail=4 -> shadow=10, extra=1, independent of tie order.
    std::vector<RunningJob> running = {{10, 2}, {10, 2}, {10, 2}};
    Reservation r = compute_reservation(0, 3, 0, running);
    EXPECT_TRUE(r.exists);
    EXPECT_EQ(r.shadow_time, 10ull);
    EXPECT_EQ(r.extra, 1);
}

TEST(Backfill, ReservationNeverSatisfiable) {
    // free_now=0 + 2 (job@10) + 2 (job@20) = 4 < k_head=8 -> exists=false.
    std::vector<RunningJob> running = {{10, 2}, {20, 2}};
    Reservation r = compute_reservation(0, 8, 0, running);
    EXPECT_FALSE(r.exists);
}

TEST(Backfill, ReservationNoRunningJobsAndShort) {
    // free_now satisfies on its own with no running jobs.
    Reservation r = compute_reservation(0, 4, 4, {});
    EXPECT_TRUE(r.exists);
    EXPECT_EQ(r.shadow_time, 0ull);
    EXPECT_EQ(r.extra, 0);
}

TEST(Backfill, ReservationEmptyRunningInsufficientFreeNow) {
    // No running jobs to drain and free_now < k_head -> unsatisfiable.
    Reservation r = compute_reservation(0, /*k_head=*/8, /*free_now=*/2, {});
    EXPECT_FALSE(r.exists);
}

// --- backfill_safe (pure predicate) ----------------------------------------

TEST(Backfill, SafeConditionAShortJob) {
    Reservation res{true, /*shadow=*/25, /*extra=*/0};
    EXPECT_TRUE(backfill_safe(res, /*cand_end=*/20, /*ranks=*/3, /*extra=*/0));
}

TEST(Backfill, SafeConditionBExtraNodes) {
    Reservation res{true, /*shadow=*/25, /*extra=*/2};
    EXPECT_TRUE(backfill_safe(res, /*cand_end=*/40, /*ranks=*/2, /*extra=*/2));
}

TEST(Backfill, UnsafeWhenLongAndNoExtra) {
    Reservation res{true, /*shadow=*/25, /*extra=*/0};
    EXPECT_FALSE(backfill_safe(res, /*cand_end=*/40, /*ranks=*/3, /*extra=*/0));
}

TEST(Backfill, BoundaryConditionAEqual) {
    // cand_end == shadow_time is safe via (a), regardless of rank count.
    Reservation res{true, /*shadow=*/25, /*extra=*/0};
    EXPECT_TRUE(backfill_safe(res, /*cand_end=*/25, /*ranks=*/99, /*extra=*/0));
}

TEST(Backfill, BoundaryConditionBEqual) {
    // ranks == extra is safe via (b); ranks > extra is not.
    Reservation res{true, /*shadow=*/25, /*extra=*/2};
    EXPECT_TRUE(backfill_safe(res, /*cand_end=*/40, /*ranks=*/2, /*extra=*/2));
    EXPECT_FALSE(backfill_safe(res, /*cand_end=*/40, /*ranks=*/3, /*extra=*/2));
}

TEST(Backfill, SafeUsesCallerExtraNotResExtra) {
    // res.extra starts at 5, but the caller has already consumed slack and
    // passes the live remaining extra = 3. backfill_safe must use 3, not 5.
    Reservation res{true, /*shadow=*/25, /*extra=*/5};
    EXPECT_TRUE(backfill_safe(res, /*cand_end=*/40, /*ranks=*/3, /*extra=*/3));
    EXPECT_FALSE(backfill_safe(res, /*cand_end=*/40, /*ranks=*/4, /*extra=*/3));
}

TEST(Backfill, NotExistsRejectsAll) {
    Reservation res{false, 0, 0};
    EXPECT_FALSE(
        backfill_safe(res, /*cand_end=*/5, /*ranks=*/1, /*extra=*/100));
}

}  // namespace

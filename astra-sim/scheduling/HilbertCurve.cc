/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/scheduling/HilbertCurve.hh"

#include <cassert>

// Skilling, "Programming the Hilbert curve", AIP Conference Proceedings, 2004.
// Pack convention: bit b of X[i] <-> bit (b*N + (N-1-i)) of the scalar
// distance d. Matches galtay/hilbertcurve v2.0.5 (which encodes d as a
// p*n-bit big-endian string sliced as x[i] = bits[i::n]).

namespace AstraSim {
namespace Scheduling {

namespace {

constexpr int kDim = 3;

// In-place forward transform from axes to the transposed (pre-pack)
// representation. After this call, bit b of X[i] is the bit at position
// b*N + (N-1-i) of the scalar Hilbert distance.
void axes_to_transpose(uint32_t* X, int B) {
    const uint32_t M = 1u << (B - 1);
    // Inverse undo.
    for (uint32_t Q = M; Q > 1; Q >>= 1) {
        const uint32_t P = Q - 1;
        for (int i = 0; i < kDim; ++i) {
            if (X[i] & Q) {
                X[0] ^= P;
            } else {
                const uint32_t t = (X[0] ^ X[i]) & P;
                X[0] ^= t;
                X[i] ^= t;
            }
        }
    }
    // Gray encode.
    for (int i = 1; i < kDim; ++i) {
        X[i] ^= X[i - 1];
    }
    uint32_t t = 0;
    for (uint32_t Q = M; Q > 1; Q >>= 1) {
        if (X[kDim - 1] & Q) {
            t ^= Q - 1;
        }
    }
    for (int i = 0; i < kDim; ++i) {
        X[i] ^= t;
    }
}

// In-place reverse transform from transposed representation back to axes.
void transpose_to_axes(uint32_t* X, int B) {
    const uint32_t N2 = 2u << (B - 1);  // 2^B
    // Gray decode by H ^ (H/2).
    const uint32_t t = X[kDim - 1] >> 1;
    for (int i = kDim - 1; i > 0; --i) {
        X[i] ^= X[i - 1];
    }
    X[0] ^= t;
    // Undo excess work.
    for (uint32_t Q = 2; Q != N2; Q <<= 1) {
        const uint32_t P = Q - 1;
        for (int i = kDim - 1; i >= 0; --i) {
            if (X[i] & Q) {
                X[0] ^= P;
            } else {
                const uint32_t u = (X[0] ^ X[i]) & P;
                X[0] ^= u;
                X[i] ^= u;
            }
        }
    }
}

}  // namespace

uint64_t hilbert_d_from_xyz(int x, int y, int z, int p) {
    assert(p >= 0 && p <= 21);
    if (p == 0) {
        return 0;
    }
    assert(x >= 0 && y >= 0 && z >= 0);
    assert(x < (1 << p) && y < (1 << p) && z < (1 << p));
    uint32_t X[kDim] = {static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                        static_cast<uint32_t>(z)};
    axes_to_transpose(X, p);
    uint64_t d = 0;
    for (int b = 0; b < p; ++b) {
        for (int i = 0; i < kDim; ++i) {
            const uint64_t bit = (X[i] >> b) & 1u;
            d |= bit << (b * kDim + (kDim - 1 - i));
        }
    }
    return d;
}

std::array<int, 3> hilbert_xyz_from_d(uint64_t d, int p) {
    assert(p >= 0 && p <= 21);
    if (p == 0) {
        return {0, 0, 0};
    }
    uint32_t X[kDim] = {0, 0, 0};
    for (int b = 0; b < p; ++b) {
        for (int i = 0; i < kDim; ++i) {
            const auto bit =
                static_cast<uint32_t>((d >> (b * kDim + (kDim - 1 - i))) & 1u);
            X[i] |= bit << b;
        }
    }
    transpose_to_axes(X, p);
    return {static_cast<int>(X[0]), static_cast<int>(X[1]),
            static_cast<int>(X[2])};
}

}  // namespace Scheduling
}  // namespace AstraSim

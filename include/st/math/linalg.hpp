// SPDX-License-Identifier: Apache-2.0
#pragma once

// 3x3 decompositions and attitude fits. All double precision, no allocation, bounded work.

#include "st/math/vec.hpp"

namespace st {

// Eigen-decomposition of a symmetric 3x3 matrix by cyclic Jacobi rotations.
// Eigenvalues sorted descending; columns of `vecs` are the matching unit eigenvectors.
// Worst case: kMaxJacobiSweeps sweeps of 3 rotations (converges in ~6 sweeps for doubles).
inline constexpr int kMaxJacobiSweeps = 32;
void symmetric_eigen3(const Mat3& a, Vec3& values, Mat3& vecs) noexcept;

// Singular value decomposition a = U * diag(s) * V^T with s descending and non-negative.
// U and V are orthogonal (either may have determinant -1, as with LAPACK). O(1).
void svd3(const Mat3& a, Mat3& u, Vec3& s, Mat3& v) noexcept;

// Orthogonal polar factor U*V^T of h (what numpy's `U @ Vh` gives tetra3). May be a
// reflection when det(h) < 0. O(1).
Mat3 orthogonal_polar(const Mat3& h) noexcept;

// Closest proper rotation U*diag(1,1,det(UV^T))*V^T (Kabsch with reflection correction). O(1).
Mat3 proper_rotation(const Mat3& h) noexcept;

// Binomial CDF P[X <= k], X ~ Binomial(n, p). Matches scipy.stats.binom.cdf semantics:
// k < 0 -> 0, k >= n -> 1, p outside [0, 1] or NaN -> NaN.
// Worst case: O(min(k, n)) (summed in log space, no underflow for any n).
double binomial_cdf(long k, long n, double p) noexcept;

}  // namespace st

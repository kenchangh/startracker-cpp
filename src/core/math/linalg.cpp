// SPDX-License-Identifier: Apache-2.0
#include "st/math/linalg.hpp"

#include <cmath>
#include <limits>

namespace st {

void symmetric_eigen3(const Mat3& in, Vec3& values, Mat3& vecs) noexcept {
  Mat3 a = in;
  Mat3 v = Mat3::identity();
  for (int sweep = 0; sweep < kMaxJacobiSweeps; ++sweep) {
    const double off = a.m[0][1] * a.m[0][1] + a.m[0][2] * a.m[0][2] + a.m[1][2] * a.m[1][2];
    const double diag = a.m[0][0] * a.m[0][0] + a.m[1][1] * a.m[1][1] + a.m[2][2] * a.m[2][2];
    if (off <= 1e-30 * diag || off == 0.0) break;
    for (unsigned p = 0; p < 2; ++p) {
      for (unsigned q = p + 1; q < 3; ++q) {
        const double apq = a.m[p][q];
        if (apq == 0.0) continue;
        // Rotation that zeroes a[p][q] (Numerical Recipes formulation).
        const double theta = (a.m[q][q] - a.m[p][p]) / (2.0 * apq);
        const double t =
            (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (unsigned k = 0; k < 3; ++k) {  // A <- A * J
          const double akp = a.m[k][p], akq = a.m[k][q];
          a.m[k][p] = c * akp - s * akq;
          a.m[k][q] = s * akp + c * akq;
        }
        for (unsigned k = 0; k < 3; ++k) {  // A <- J^T * A
          const double apk = a.m[p][k], aqk = a.m[q][k];
          a.m[p][k] = c * apk - s * aqk;
          a.m[q][k] = s * apk + c * aqk;
        }
        for (unsigned k = 0; k < 3; ++k) {  // V <- V * J
          const double vkp = v.m[k][p], vkq = v.m[k][q];
          v.m[k][p] = c * vkp - s * vkq;
          v.m[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
  // Sort descending (3 elements, stable selection).
  unsigned idx[3] = {0, 1, 2};
  for (unsigned i = 0; i < 2; ++i) {
    for (unsigned j = i + 1; j < 3; ++j) {
      if (a.m[idx[j]][idx[j]] > a.m[idx[i]][idx[i]]) {
        const unsigned t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
      }
    }
  }
  for (unsigned i = 0; i < 3; ++i) {
    values[static_cast<int>(i)] = a.m[idx[i]][idx[i]];
    for (unsigned k = 0; k < 3; ++k) vecs.m[k][i] = v.m[k][idx[i]];
  }
}

namespace {
void set_col(Mat3& m, unsigned c, const Vec3& v) noexcept {
  m.m[0][c] = v.x;
  m.m[1][c] = v.y;
  m.m[2][c] = v.z;
}
// Any unit vector orthogonal to a (a unit).
Vec3 any_orthogonal(const Vec3& a) noexcept {
  const Vec3 t = std::fabs(a.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  return normalized(cross(a, t));
}
}  // namespace

void svd3(const Mat3& a, Mat3& u, Vec3& s, Mat3& v) noexcept {
  // One-sided (Hestenes) Jacobi: orthogonalize the columns of A by plane rotations applied on
  // the right. Works on A directly (not A^T A), so small singular values keep full relative
  // accuracy. Bounded by kMaxJacobiSweeps sweeps of 3 rotations.
  Vec3 c[3] = {a.col(0), a.col(1), a.col(2)};
  Vec3 vc[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const double eps = std::numeric_limits<double>::epsilon();
  for (int sweep = 0; sweep < kMaxJacobiSweeps; ++sweep) {
    bool rotated = false;
    for (int p = 0; p < 2; ++p) {
      for (int q = p + 1; q < 3; ++q) {
        const double alpha = dot(c[p], c[p]);
        const double beta = dot(c[q], c[q]);
        const double gamma = dot(c[p], c[q]);
        if (gamma == 0.0 || std::fabs(gamma) <= eps * std::sqrt(alpha * beta)) continue;
        rotated = true;
        const double zeta = (beta - alpha) / (2.0 * gamma);
        const double t =
            (zeta >= 0 ? 1.0 : -1.0) / (std::fabs(zeta) + std::sqrt(1.0 + zeta * zeta));
        const double cs = 1.0 / std::sqrt(1.0 + t * t);
        const double sn = cs * t;
        const Vec3 cp = c[p], cq = c[q];
        c[p] = cs * cp - sn * cq;
        c[q] = sn * cp + cs * cq;
        const Vec3 vp = vc[p], vq = vc[q];
        vc[p] = cs * vp - sn * vq;
        vc[q] = sn * vp + cs * vq;
      }
    }
    if (!rotated) break;
  }
  double sv[3] = {norm(c[0]), norm(c[1]), norm(c[2])};
  int idx[3] = {0, 1, 2};
  for (int i = 0; i < 2; ++i) {
    for (int j = i + 1; j < 3; ++j) {
      if (sv[idx[j]] > sv[idx[i]]) {
        const int t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
      }
    }
  }
  const double smax = sv[idx[0]];
  const double tiny = smax > 0 ? smax * 8 * eps : 0.0;
  Vec3 ucols[3];
  int rank = 0;
  for (int i = 0; i < 3; ++i) {
    const int k = idx[i];
    s[i] = sv[k];
    set_col(v, static_cast<unsigned>(i), vc[k]);
    if (sv[k] > tiny && sv[k] > 0) {
      ucols[i] = c[k] / sv[k];
      ++rank;
    }
  }
  // Complete U to an orthonormal basis for zero singular values.
  if (rank == 0) ucols[0] = {1, 0, 0};
  if (rank <= 1) {
    ucols[1] = any_orthogonal(ucols[0]);
    if (rank == 0) s[0] = 0;
    s[1] = 0;
  } else {
    ucols[1] = normalized(ucols[1] - dot(ucols[1], ucols[0]) * ucols[0]);
  }
  const Vec3 u2 = cross(ucols[0], ucols[1]);
  if (rank == 3) {
    ucols[2] = dot(u2, ucols[2]) < 0 ? -u2 : u2;
  } else {
    ucols[2] = u2;
    s[2] = 0;
  }
  for (unsigned i = 0; i < 3; ++i) set_col(u, i, ucols[i]);
}

Mat3 orthogonal_polar(const Mat3& h) noexcept {
  Mat3 u, v;
  Vec3 s;
  svd3(h, u, s, v);
  return u * transpose(v);
}

Mat3 proper_rotation(const Mat3& h) noexcept {
  Mat3 u, v;
  Vec3 s;
  svd3(h, u, s, v);
  Mat3 r = u * transpose(v);
  if (determinant(r) < 0) {
    for (unsigned k = 0; k < 3; ++k) u.m[k][2] = -u.m[k][2];
    r = u * transpose(v);
  }
  return r;
}

double binomial_cdf(long k, long n, double p) noexcept {
  if (std::isnan(p) || p < 0.0 || p > 1.0 || n < 0) return std::numeric_limits<double>::quiet_NaN();
  if (k < 0) return 0.0;
  if (k >= n) return 1.0;
  if (p == 0.0) return 1.0;  // X == 0 always, k >= 0
  if (p == 1.0) return 0.0;  // X == n always, k < n
  const double lp = std::log(p);
  const double lq = std::log1p(-p);
  // log pmf(0) = n*log(1-p); log pmf(i+1) = log pmf(i) + log((n-i)/(i+1)) + log(p/(1-p))
  double lpmf = static_cast<double>(n) * lq;
  double sum = 0.0;
  // Sum relative to the largest term to avoid underflow.
  double lmax = lpmf;
  double l = lpmf;
  for (long i = 0; i < k; ++i) {
    l += std::log(static_cast<double>(n - i) / static_cast<double>(i + 1)) + lp - lq;
    if (l > lmax) lmax = l;
  }
  for (long i = 0; i <= k; ++i) {
    sum += std::exp(lpmf - lmax);
    lpmf += std::log(static_cast<double>(n - i) / static_cast<double>(i + 1)) + lp - lq;
  }
  const double r = sum * std::exp(lmax);
  return r > 1.0 ? 1.0 : r;
}

}  // namespace st

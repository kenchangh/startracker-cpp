// SPDX-License-Identifier: Apache-2.0
#include <cmath>
#include <random>

#include "st/math/linalg.hpp"
#include "unit/no_alloc_fixture.hpp"

namespace {
using namespace st;
using MathTest = st::test::NoAllocTest;

Mat3 random_matrix(std::mt19937_64& g) {
  std::uniform_real_distribution<double> u(-1, 1);
  Mat3 m;
  for (auto& r : m.m)
    for (auto& v : r) v = u(g);
  return m;
}
double max_abs_diff(const Mat3& a, const Mat3& b) {
  double d = 0;
  for (unsigned i = 0; i < 3; ++i)
    for (unsigned j = 0; j < 3; ++j) d = std::max(d, std::fabs(a.m[i][j] - b.m[i][j]));
  return d;
}

TEST_F(MathTest, QuaternionMatrixRoundTrip) {
  std::mt19937_64 g(1);
  std::uniform_real_distribution<double> u(-3, 3);
  for (int i = 0; i < 200; ++i) {
    const Quat q = canonical(from_rotation_vector({u(g), u(g), u(g)}));
    const Quat q2 = from_matrix(to_matrix(q));
    EXPECT_LT(angle_between(q, q2), 1e-12);
    EXPECT_NEAR(determinant(to_matrix(q)), 1.0, 1e-12);
    const Vec3 rv = to_rotation_vector(q);
    EXPECT_LT(angle_between(from_rotation_vector(rv), q), 1e-12);
  }
  const Quat a = from_rotation_vector({0, 0, kPi / 2});
  const Vec3 v = rotate(a, {1, 0, 0});
  EXPECT_NEAR(v.x, 0, 1e-15);
  EXPECT_NEAR(v.y, 1, 1e-15);
  // Composition: rotate(a*b, v) == rotate(a, rotate(b, v)).
  const Quat b = from_rotation_vector({0.3, -0.2, 0.1});
  const Vec3 w1 = rotate(a * b, {0.2, 0.5, 0.8});
  const Vec3 w2 = rotate(a, rotate(b, {0.2, 0.5, 0.8}));
  EXPECT_NEAR(norm(w1 - w2), 0, 1e-14);
}

TEST_F(MathTest, SvdReconstructs) {
  std::mt19937_64 g(2);
  for (int i = 0; i < 500; ++i) {
    const Mat3 a = random_matrix(g);
    Mat3 u, v;
    Vec3 s;
    svd3(a, u, s, v);
    Mat3 sd;
    sd.m[0][0] = s.x;
    sd.m[1][1] = s.y;
    sd.m[2][2] = s.z;
    EXPECT_LT(max_abs_diff(u * sd * transpose(v), a), 1e-12);
    EXPECT_LT(max_abs_diff(transpose(u) * u, Mat3::identity()), 1e-12);
    EXPECT_LT(max_abs_diff(transpose(v) * v, Mat3::identity()), 1e-12);
    EXPECT_GE(s.x, s.y);
    EXPECT_GE(s.y, s.z);
    EXPECT_GE(s.z, 0);
  }
}

TEST_F(MathTest, PolarFactorKeepsReflectionLikeNumpy) {
  // H = R * diag(1, 1, -1) has polar factor with det -1; proper_rotation must give det +1.
  const Mat3 r = to_matrix(from_rotation_vector({0.1, 0.2, 0.3}));
  Mat3 f = Mat3::identity();
  f.m[2][2] = -1;
  const Mat3 h = r * f;
  EXPECT_NEAR(determinant(orthogonal_polar(h)), -1.0, 1e-12);
  EXPECT_LT(max_abs_diff(orthogonal_polar(h), h), 1e-12);
  EXPECT_NEAR(determinant(proper_rotation(h)), 1.0, 1e-12);
  // Kabsch on exact data recovers the rotation.
  Mat3 cov;
  const Vec3 pts[4] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.5, 0.5, 0.7}};
  for (const Vec3& p : pts) cov = cov + outer(r * p, p);
  EXPECT_LT(max_abs_diff(orthogonal_polar(cov), r), 1e-12);
  EXPECT_LT(max_abs_diff(proper_rotation(cov), r), 1e-12);
}

TEST_F(MathTest, RankDeficientSvd) {
  Mat3 a;  // rank 1
  a.m[0] = {1, 2, 3};
  a.m[1] = {2, 4, 6};
  a.m[2] = {3, 6, 9};
  Mat3 u, v;
  Vec3 s;
  svd3(a, u, s, v);
  EXPECT_NEAR(s.y, 0, 1e-12);
  EXPECT_NEAR(s.z, 0, 1e-12);
  EXPECT_LT(max_abs_diff(transpose(u) * u, Mat3::identity()), 1e-12);
  Mat3 z;
  svd3(z, u, s, v);  // all zero must not produce NaN
  EXPECT_FALSE(std::isnan(u.m[0][0]));
}

struct BinomCase {
  long k, n;
  double p, want;
};
TEST_F(MathTest, BinomialMatchesScipy) {
  // Reference values from scipy.stats.binom.cdf (scipy 1.13.1).
  const BinomCase cases[] = {
      {3, 10, 0.5, 0.171875},
      {0, 30, 0.98999999999999999, 1.0000000000000266e-60},
      {5, 30, 0.99280000000000002, 3.733020058071904e-49},
      {12, 30, 0.98999999999999999, 7.7158115601436296e-29},
      {28, 30, 0.90000000000000002, 0.81630498080739633},
      {1, 50, 0.999, 4.9951000000002178e-146},
      {20, 40, 0.29999999999999999, 0.9975806401028664},
      {0, 1, 0.5, 0.5},
  };
  for (const auto& c : cases) {
    EXPECT_NEAR(binomial_cdf(c.k, c.n, c.p) / c.want, 1.0, 1e-9) << c.k << " " << c.n << " " << c.p;
  }
  EXPECT_EQ(binomial_cdf(-1, 10, 0.5), 0.0);
  EXPECT_EQ(binomial_cdf(10, 10, 0.5), 1.0);
  EXPECT_EQ(binomial_cdf(12, 10, 0.5), 1.0);
  EXPECT_TRUE(std::isnan(binomial_cdf(3, 10, -0.1)));
  EXPECT_TRUE(std::isnan(binomial_cdf(3, 10, 1.5)));
  EXPECT_EQ(binomial_cdf(3, 10, 1.0), 0.0);
  EXPECT_EQ(binomial_cdf(3, 10, 0.0), 1.0);
}
}  // namespace

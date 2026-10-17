// SPDX-License-Identifier: Apache-2.0
// Port of ESA tetra3 get_centroids_from_image (l.1796-2135 @ f9fa2eb). "E:nnn" refers to
// tetra3/tetra3.py. Float32 steps mirror numpy/scipy float32 arithmetic.
#include "st/centroid/centroider.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace st {

struct Centroider::Blob {
  std::uint32_t area = 0;
  std::uint32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
  std::uint32_t first = 0;  // raster index of the first pixel (label order)
};

namespace {

constexpr std::uint32_t kNoLabel = 0xFFFFFFFFu;

// numpy pairwise_sum (loops_utils.h), float32 accumulators.
float pairwise_f32(const float* a, std::size_t n) noexcept {
  if (n < 8) {
    float r = 0.0f;
    for (std::size_t i = 0; i < n; ++i) r += a[i];
    return r;
  }
  if (n <= 128) {
    float r[8];
    for (int j = 0; j < 8; ++j) r[j] = a[j];
    std::size_t i = 8;
    for (; i < n - (n % 8); i += 8) {
      for (std::size_t j = 0; j < 8; ++j) r[j] += a[i + j];
    }
    float res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += a[i];
    return res;
  }
  std::size_t n2 = n / 2;
  n2 -= n2 % 8;
  return pairwise_f32(a, n2) + pairwise_f32(a + n2, n - n2);
}

// Same algorithm in double (numpy float64 sums).
double pairwise_f64(const double* a, std::size_t n) noexcept {
  if (n < 8) {
    double r = 0.0;
    for (std::size_t i = 0; i < n; ++i) r += a[i];
    return r;
  }
  if (n <= 128) {
    double r[8];
    for (int j = 0; j < 8; ++j) r[j] = a[j];
    std::size_t i = 8;
    for (; i < n - (n % 8); i += 8) {
      for (std::size_t j = 0; j < 8; ++j) r[j] += a[i + j];
    }
    double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) res += a[i];
    return res;
  }
  std::size_t n2 = n / 2;
  n2 -= n2 % 8;
  return pairwise_f64(a, n2) + pairwise_f64(a + n2, n - n2);
}

// scipy 'reflect' extension (d c b a | a b c d | d c b a) for any offset.
std::size_t reflect(long i, long n) noexcept {
  const long period = 2 * n;
  long m = i % period;
  if (m < 0) m += period;
  return static_cast<std::size_t>(m < n ? m : period - 1 - m);
}

// scipy NI_UniformFilter1D on one line: running mean in double, updated with
// (in - out) / size per step.
void uniform_line(const double* ext, std::size_t length, std::size_t size, double* out) noexcept {
  double tmp = 0.0;
  for (std::size_t i = 0; i < size; ++i) tmp += ext[i];
  tmp /= static_cast<double>(size);
  out[0] = tmp;
  for (std::size_t i = 1; i < length; ++i) {
    tmp += (ext[i + size - 1] - ext[i - 1]) / static_cast<double>(size);
    out[i] = tmp;
  }
}

// scipy.ndimage.uniform_filter(src, size, output=float32): axis 0 then axis 1, float32
// intermediate. `dst` may alias neither `src` nor itself. `col` holds max(W,H) + size doubles,
// `outl` max(W,H) doubles.
void uniform_filter(const float* src, float* dst, std::uint32_t w, std::uint32_t h,
                    std::uint32_t size, double* col, double* outl) noexcept {
  const long s1 = static_cast<long>(size / 2);
  // Axis 0 (down the columns), all columns at once so memory is read row by row: per column
  // the arithmetic is exactly the 1-D running mean.
  double* tmp = outl;  // per-column running sums (w entries)
  for (std::uint32_t x = 0; x < w; ++x) tmp[x] = 0.0;
  for (std::uint32_t k = 0; k < size; ++k) {
    const float* row = src + reflect(static_cast<long>(k) - s1, h) * w;
    for (std::uint32_t x = 0; x < w; ++x) tmp[x] += static_cast<double>(row[x]);
  }
  for (std::uint32_t x = 0; x < w; ++x) {
    tmp[x] /= static_cast<double>(size);
    dst[x] = static_cast<float>(tmp[x]);
  }
  for (std::uint32_t y = 1; y < h; ++y) {
    const float* add =
        src + reflect(static_cast<long>(y) + static_cast<long>(size) - 1 - s1, h) * w;
    const float* sub = src + reflect(static_cast<long>(y) - 1 - s1, h) * w;
    float* d = dst + static_cast<std::size_t>(y) * w;
    for (std::uint32_t x = 0; x < w; ++x) {
      tmp[x] +=
          (static_cast<double>(add[x]) - static_cast<double>(sub[x])) / static_cast<double>(size);
      d[x] = static_cast<float>(tmp[x]);
    }
  }
  // Axis 1 (along rows), in place on dst row by row.
  for (std::uint32_t y = 0; y < h; ++y) {
    float* r = dst + static_cast<std::size_t>(y) * w;
    for (std::uint32_t k = 0; k < w + size - 1; ++k) {
      col[k] = static_cast<double>(r[reflect(static_cast<long>(k) - s1, w)]);
    }
    uniform_line(col, w, size, outl);
    for (std::uint32_t x = 0; x < w; ++x) r[x] = static_cast<float>(outl[x]);
  }
}

// np.median of float32 values (in place partial sort of `v`).
float median_f32(float* v, std::size_t n) noexcept {
  if (n == 0) return 0.0f;
  const std::size_t mid = n / 2;
  std::nth_element(v, v + mid, v + n);
  const float hi = v[mid];
  if (n % 2 == 1) return hi;
  const float lo = *std::max_element(v, v + mid);
  // np.mean of two float32 values: float32 sum, then divided in double and rounded.
  return static_cast<float>(static_cast<double>(lo + hi) / 2.0);
}

std::uint32_t find_root(std::span<std::uint32_t> parent, std::uint32_t a) noexcept {
  std::uint32_t r = a;
  while (parent[r] != r) r = parent[r];
  while (parent[a] != r) {  // path compression
    const std::uint32_t next = parent[a];
    parent[a] = r;
    a = next;
  }
  return r;
}

}  // namespace

float numpy_sum_f32(std::span<const float> v) noexcept {
  float acc = 0.0f;
  for (std::size_t i = 0; i < v.size(); i += 8192) {
    acc += pairwise_f32(v.data() + i, std::min<std::size_t>(8192, v.size() - i));
  }
  return acc;
}

std::size_t Centroider::workspace_bytes(const CentroiderConfig& c) noexcept {
  const std::size_t px = static_cast<std::size_t>(c.max_width) * c.max_height;
  const std::size_t line = std::max(c.max_width, c.max_height) + c.filter_size;
  const std::size_t pix =
      std::max<std::size_t>({c.max_area, std::size_t{c.centroid_window} * c.centroid_window, 4096});
  const std::size_t local = c.sigma_mode == SigmaMode::kLocalRootSquare ? px * 4 : 0;
  return px * (4 + 4 + 1 + 1 + 4) + local + (px + 63) / 64 * 8 +
         (4 * std::size_t{c.max_blobs} + 1) * 8 + (std::size_t{c.max_blobs} + 1) * sizeof(Blob) +
         2 * line * 8 + 3 * pix * 8 + 1024;
}

Result<Centroider> Centroider::create(const CentroiderConfig& c, Arena& arena) noexcept {
  if (c.max_width == 0 || c.max_height == 0 || c.max_blobs == 0 ||
      ((c.background == BackgroundMode::kLocalMean ||
        c.sigma_mode == SigmaMode::kLocalRootSquare) &&
       (c.filter_size == 0 || c.filter_size % 2 == 0)) ||
      !(c.sigma > 0) || c.max_blobs > (1u << 28)) {
    return Status::kInvalidArgument;
  }
  Centroider z;
  z.cfg_ = c;
  const std::size_t px = static_cast<std::size_t>(c.max_width) * c.max_height;
  const std::size_t line = std::max(c.max_width, c.max_height) + c.filter_size;
  const std::size_t pix =
      std::max<std::size_t>({c.max_area, std::size_t{c.centroid_window} * c.centroid_window, 4096});
  ST_TRY_ASSIGN(z.img_, arena.allocate_uninit<float>(px, 64));
  ST_TRY_ASSIGN(z.work_, arena.allocate_uninit<float>(px, 64));
  ST_TRY_ASSIGN(z.mask_a_, arena.allocate_uninit<std::uint8_t>(px, 64));
  ST_TRY_ASSIGN(z.mask_b_, arena.allocate_uninit<std::uint8_t>(px, 64));
  ST_TRY_ASSIGN(z.labels_, arena.allocate_uninit<std::uint32_t>(px, 64));
  ST_TRY_ASSIGN(z.parent_, arena.allocate_uninit<std::uint32_t>(4 * std::size_t{c.max_blobs} + 1));
  ST_TRY_ASSIGN(z.remap_, arena.allocate_uninit<std::uint32_t>(4 * std::size_t{c.max_blobs} + 1));
  ST_TRY_ASSIGN(z.blobs_, arena.allocate<Blob>(c.max_blobs));
  if (c.sigma_mode == SigmaMode::kLocalRootSquare) {
    ST_TRY_ASSIGN(z.work2_, arena.allocate_uninit<float>(px, 64));
  }
  ST_TRY_ASSIGN(z.line_, arena.allocate_uninit<double>(line));
  ST_TRY_ASSIGN(z.line_out_, arena.allocate_uninit<double>(line));
  ST_TRY_ASSIGN(z.pix_, arena.allocate_uninit<double>(3 * pix));
  if (c.use_hot_pixel_map) {
    ST_TRY_ASSIGN(z.hot_words_, arena.allocate<std::uint64_t>(bitset_words(px)));
    z.hot_ = BitSpan(z.hot_words_, px);
  }
  return z;
}

namespace {

// Converts the input to float32 (tetra3: np.asarray(image, dtype=np.float32)), replacing hot
// pixels by the mean of their non-hot 4-neighbours.
void convert(const ImageView& im, float* dst, const BitSpan* hot,
             std::uint32_t stride_px) noexcept {
  const std::size_t stride =
      im.stride_bytes != 0 ? im.stride_bytes
                           : std::size_t{im.width} * (im.format == PixelFormat::kMono8 ? 1u : 2u);
  const auto* base = static_cast<const unsigned char*>(im.data);
  auto px = [&](std::uint32_t x, std::uint32_t y) -> float {
    const unsigned char* row = base + static_cast<std::size_t>(y) * stride;
    if (im.format == PixelFormat::kMono8) return static_cast<float>(row[x]);
    std::uint16_t v = 0;
    std::memcpy(&v, row + 2 * static_cast<std::size_t>(x), 2);
    return static_cast<float>(v);
  };
  for (std::uint32_t y = 0; y < im.height; ++y) {
    for (std::uint32_t x = 0; x < im.width; ++x)
      dst[static_cast<std::size_t>(y) * im.width + x] = px(x, y);
  }
  if (hot == nullptr) return;
  auto is_hot = [&](std::uint32_t x, std::uint32_t y) {
    return hot->test(static_cast<std::size_t>(y) * stride_px + x);
  };
  for (std::size_t i = hot->find_first(); i != BitSpan::npos; i = hot->find_next(i + 1)) {
    const auto y = static_cast<std::uint32_t>(i / stride_px);
    const auto x = static_cast<std::uint32_t>(i % stride_px);
    if (y >= im.height || x >= im.width) continue;
    double sum = 0;
    int n = 0;
    if (x > 0 && !is_hot(x - 1, y)) {
      sum += static_cast<double>(px(x - 1, y));
      ++n;
    }
    if (x + 1 < im.width && !is_hot(x + 1, y)) {
      sum += static_cast<double>(px(x + 1, y));
      ++n;
    }
    if (y > 0 && !is_hot(x, y - 1)) {
      sum += static_cast<double>(px(x, y - 1));
      ++n;
    }
    if (y + 1 < im.height && !is_hot(x, y + 1)) {
      sum += static_cast<double>(px(x, y + 1));
      ++n;
    }
    dst[static_cast<std::size_t>(y) * im.width + x] = n > 0 ? static_cast<float>(sum / n) : 0.0f;
  }
}

}  // namespace

Result<std::uint32_t> Centroider::extract(const ImageView& im, std::span<Spot> out,
                                          CentroidStats* stats_out) noexcept {
  CentroidStats stats;
  if (im.data == nullptr || im.width == 0 || im.height == 0 || im.width > cfg_.max_width ||
      im.height > cfg_.max_height) {
    return Status::kInvalidArgument;
  }
  const std::uint32_t w = im.width, h = im.height;
  const std::size_t n = static_cast<std::size_t>(w) * h;
  float* img = img_.data();
  float* work = work_.data();

  // 1. Convert (E:1912-1923).
  convert(im, img, cfg_.use_hot_pixel_map ? &hot_ : nullptr, cfg_.max_width);

  // 3. Background (E:1934-1953).
  switch (cfg_.background) {
    case BackgroundMode::kNone: break;
    case BackgroundMode::kLocalMean:
      uniform_filter(img, work, w, h, cfg_.filter_size, line_.data(), line_out_.data());
      for (std::size_t i = 0; i < n; ++i) img[i] = img[i] - work[i];
      break;
    case BackgroundMode::kGlobalMean: {
      const auto mean =
          static_cast<float>(static_cast<double>(numpy_sum_f32({img, n})) / static_cast<double>(n));
      for (std::size_t i = 0; i < n; ++i) img[i] = img[i] - mean;
      break;
    }
    case BackgroundMode::kGlobalMedian: {
      std::memcpy(work, img, n * sizeof(float));
      const float med = median_f32(work, n);
      for (std::size_t i = 0; i < n; ++i) img[i] = img[i] - med;
      break;
    }
  }

  // 4-5. Threshold (E:1957-1983).
  std::uint8_t* mask = mask_a_.data();
  if (cfg_.threshold > 0 || cfg_.sigma_mode != SigmaMode::kLocalRootSquare) {
    float th = cfg_.threshold;
    if (!(th > 0)) {
      if (cfg_.sigma_mode == SigmaMode::kGlobalRootSquare) {
        for (std::size_t i = 0; i < n; ++i) work[i] = img[i] * img[i];
        const auto mean = static_cast<float>(static_cast<double>(numpy_sum_f32({work, n})) /
                                             static_cast<double>(n));
        const float std_f = std::sqrt(mean);
        th = static_cast<float>(static_cast<double>(std_f) * static_cast<double>(cfg_.sigma));
      } else {  // kGlobalMedianAbs
        for (std::size_t i = 0; i < n; ++i) work[i] = std::fabs(img[i]);
        const float med = median_f32(work, n);
        th = static_cast<float>(static_cast<double>(med) * 1.48 * static_cast<double>(cfg_.sigma));
      }
    }
    stats.threshold = th;
    for (std::size_t i = 0; i < n; ++i) mask[i] = img[i] > th ? 1 : 0;
  } else {
    // Local root square: sqrt(uniform_filter(residual^2)) * sigma per pixel.
    for (std::size_t i = 0; i < n; ++i) work[i] = img[i] * img[i];
    float* filtered = work2_.data();
    uniform_filter(work, filtered, w, h, cfg_.filter_size, line_.data(), line_out_.data());
    for (std::size_t i = 0; i < n; ++i) {
      mask[i] = img[i] > std::sqrt(filtered[i]) * cfg_.sigma ? 1 : 0;
    }
  }

  // 5. Binary opening with a 3x3 cross, border value 0 (E:1984-1985).
  if (cfg_.binary_open) {
    std::uint8_t* e = mask_b_.data();
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        e[i] = static_cast<std::uint8_t>(mask[i] != 0 && x > 0 && mask[i - 1] != 0 && x + 1 < w &&
                                         mask[i + 1] != 0 && y > 0 && mask[i - w] != 0 &&
                                         y + 1 < h && mask[i + w] != 0);
      }
    }
    for (std::uint32_t y = 0; y < h; ++y) {
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        mask[i] = static_cast<std::uint8_t>(
            e[i] != 0 || (x > 0 && e[i - 1] != 0) || (x + 1 < w && e[i + 1] != 0) ||
            (y > 0 && e[i - w] != 0) || (y + 1 < h && e[i + w] != 0));
      }
    }
  }

  // 6. Label 4-connected regions (E:1989), two passes with union-find. Final labels are numbered
  // in raster order of each region's first pixel, as scipy.ndimage.label numbers them.
  std::uint32_t* lab = labels_.data();
  const auto max_prov = static_cast<std::uint32_t>(parent_.size() - 1);
  std::uint32_t nprov = 0;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      if (mask[i] == 0) {
        lab[i] = kNoLabel;
        continue;
      }
      const std::uint32_t left = x > 0 ? lab[i - 1] : kNoLabel;
      const std::uint32_t up = y > 0 ? lab[i - w] : kNoLabel;
      if (left == kNoLabel && up == kNoLabel) {
        if (nprov >= max_prov) {
          stats.blob_overflow = true;
          lab[i] = kNoLabel;
          continue;
        }
        parent_[nprov] = nprov;
        lab[i] = nprov++;
      } else if (left == kNoLabel || up == kNoLabel) {
        lab[i] = left == kNoLabel ? up : left;
      } else {
        const std::uint32_t a = find_root(parent_, left), b = find_root(parent_, up);
        if (a != b) parent_[std::max(a, b)] = std::min(a, b);
        lab[i] = std::min(a, b);
      }
    }
  }
  // Flatten, then number regions in raster order of their first pixel (scipy's numbering).
  for (std::uint32_t p = 0; p < nprov; ++p) {
    parent_[p] = find_root(parent_, p);
    remap_[p] = kNoLabel;
  }
  constexpr std::uint32_t kDropped = 0xFFFFFFFEu;
  std::uint32_t nblobs = 0;
  const auto max_blobs = static_cast<std::uint32_t>(blobs_.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (lab[i] == kNoLabel) continue;
    std::uint32_t& id = remap_[parent_[lab[i]]];
    const auto x = static_cast<std::uint32_t>(i % w), y = static_cast<std::uint32_t>(i / w);
    if (id == kNoLabel) {
      if (nblobs >= max_blobs) {
        stats.blob_overflow = true;
        id = kDropped;
      } else {
        id = nblobs++;
        Blob& nb = blobs_[id];
        nb.area = 0;
        nb.xmin = nb.xmax = x;
        nb.ymin = nb.ymax = y;
        nb.first = static_cast<std::uint32_t>(i);
      }
    }
    if (id == kDropped) {
      lab[i] = kNoLabel;
      continue;
    }
    lab[i] = id;
    Blob& b = blobs_[id];
    ++b.area;
    b.xmin = std::min(b.xmin, x);
    b.xmax = std::max(b.xmax, x);
    b.ymax = std::max(b.ymax, y);
  }
  stats.components = nblobs;

  // 7. Statistics and filters (E:2007-2048). Pixels of a region are visited in raster order.
  std::uint32_t nout = 0;
  double* av = pix_.data();
  const std::size_t pcap = pix_.size() / 3;
  double* xa = av + pcap;
  double* ya = xa + pcap;
  for (std::uint32_t id = 0; id < nblobs; ++id) {
    const Blob& b = blobs_[id];
    if ((cfg_.min_area != 0 && b.area < cfg_.min_area) ||
        (cfg_.max_area != 0 && b.area > cfg_.max_area)) {
      ++stats.rejected;
      continue;
    }
    double m0 = 0, sx = 0, sy = 0;
    std::size_t k = 0;
    const bool buffered = b.area <= pcap;
    for (std::uint32_t y = b.ymin; y <= b.ymax; ++y) {
      for (std::uint32_t x = b.xmin; x <= b.xmax; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (lab[i] != id) continue;
        const auto a = static_cast<double>(img[i]);
        if (buffered) {
          av[k] = a;
          xa[k] = static_cast<double>(x) * a;
          ya[k] = static_cast<double>(y) * a;
          ++k;
        } else {
          m0 += a;
          sx += static_cast<double>(x) * a;
          sy += static_cast<double>(y) * a;
        }
      }
    }
    if (buffered) {
      m0 = pairwise_f64(av, k);
      sx = pairwise_f64(xa, k);
      sy = pairwise_f64(ya, k);
    }
    const double m1x = sx / m0, m1y = sy / m0;
    if ((cfg_.min_sum > 0 && m0 < cfg_.min_sum) || (cfg_.max_sum > 0 && m0 > cfg_.max_sum)) {
      ++stats.rejected;
      continue;
    }
    // Second moments (always computed; used for trail rejection when configured).
    double cxx = 0, cyy = 0, cxy = 0;
    for (std::uint32_t y = b.ymin; y <= b.ymax; ++y) {
      for (std::uint32_t x = b.xmin; x <= b.xmax; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (lab[i] != id) continue;
        const auto a = static_cast<double>(img[i]);
        const double dx = static_cast<double>(x) - m1x, dy = static_cast<double>(y) - m1y;
        cxx += dx * dx * a;
        cyy += dy * dy * a;
        cxy += dx * dy * a;
      }
    }
    const double m2xx = std::max(0.0, cxx / m0), m2yy = std::max(0.0, cyy / m0), m2xy = cxy / m0;
    const double root = std::sqrt((m2xx - m2yy) * (m2xx - m2yy) + 4 * m2xy * m2xy);
    const double major = std::sqrt(2 * (m2xx + m2yy + root));
    const double minor = std::sqrt(2 * std::max(0.0, m2xx + m2yy - root));
    if (cfg_.max_axis_ratio > 0 && minor <= 0) {
      ++stats.rejected;
      continue;
    }
    const double ratio = major / std::max(minor, 1e-9);
    if (cfg_.max_axis_ratio > 0 && ratio > cfg_.max_axis_ratio) {
      ++stats.rejected;
      continue;
    }
    // Keep the best `out.size()` spots by sum, ties by label order (insertion into a sorted
    // list; out.size() is small).
    Spot s;
    s.x = m1x + 0.5;
    s.y = m1y + 0.5;
    s.sum = m0;
    s.area = b.area;
    s.m2xx = m2xx;
    s.m2yy = m2yy;
    s.m2xy = m2xy;
    s.axis_ratio = ratio;
    ++stats.accepted;
    const std::size_t cap =
        cfg_.max_returned != 0 ? std::min<std::size_t>(cfg_.max_returned, out.size()) : out.size();
    if (cap == 0) continue;
    std::size_t pos = nout;
    if (nout == cap) {
      if (!(m0 > out[nout - 1].sum)) continue;
      pos = nout - 1;
    } else {
      ++nout;
    }
    while (pos > 0 && m0 > out[pos - 1].sum) {
      out[pos] = out[pos - 1];
      --pos;
    }
    out[pos] = s;
  }

  // 9. Optional window re-centroiding (E:2097-2119).
  if (cfg_.centroid_window > 0) {
    const std::uint32_t win = std::min({cfg_.centroid_window, w, h});
    for (std::uint32_t k = 0; k < nout; ++k) {
      long ox = static_cast<long>(std::floor(out[k].x)) - win / 2;
      long oy = static_cast<long>(std::floor(out[k].y)) - win / 2;
      ox = std::clamp<long>(ox, 0, static_cast<long>(w - win));
      oy = std::clamp<long>(oy, 0, static_cast<long>(h - win));
      double s = 0, sx = 0, sy = 0;
      for (std::uint32_t j = 0; j < win; ++j) {
        for (std::uint32_t i = 0; i < win; ++i) {
          const auto v = static_cast<double>(
              img[static_cast<std::size_t>(oy + j) * w + static_cast<std::size_t>(ox + i)]);
          s += v;
          sx += v * (i + 0.5);
          sy += v * (j + 0.5);
        }
      }
      out[k].x = sx / s + static_cast<double>(ox);
      out[k].y = sy / s + static_cast<double>(oy);
    }
  }
  if (stats_out != nullptr) *stats_out = stats;
  return nout;
}

Status Centroider::extract_rois(const ImageView& im, std::span<const Roi> rois, const RoiConfig& rc,
                                std::span<Spot> out, std::span<bool> found) noexcept {
  if (im.data == nullptr || out.size() < rois.size() || found.size() < rois.size() ||
      im.width > cfg_.max_width || im.height > cfg_.max_height) {
    return Status::kInvalidArgument;
  }
  const std::size_t stride =
      im.stride_bytes != 0 ? im.stride_bytes
                           : std::size_t{im.width} * (im.format == PixelFormat::kMono8 ? 1u : 2u);
  const auto* base = static_cast<const unsigned char*>(im.data);
  auto px = [&](std::uint32_t x, std::uint32_t y) -> float {
    const unsigned char* row = base + static_cast<std::size_t>(y) * stride;
    if (im.format == PixelFormat::kMono8) return static_cast<float>(row[x]);
    std::uint16_t v = 0;
    std::memcpy(&v, row + 2 * static_cast<std::size_t>(x), 2);
    return static_cast<float>(v);
  };
  auto value = [&](std::uint32_t x, std::uint32_t y) -> float {
    if (cfg_.use_hot_pixel_map && hot_.test(static_cast<std::size_t>(y) * cfg_.max_width + x)) {
      double s = 0;
      int n = 0;
      if (x > 0) {
        s += static_cast<double>(px(x - 1, y));
        ++n;
      }
      if (x + 1 < im.width) {
        s += static_cast<double>(px(x + 1, y));
        ++n;
      }
      if (y > 0) {
        s += static_cast<double>(px(x, y - 1));
        ++n;
      }
      if (y + 1 < im.height) {
        s += static_cast<double>(px(x, y + 1));
        ++n;
      }
      return n > 0 ? static_cast<float>(s / n) : 0.0f;
    }
    return px(x, y);
  };
  for (std::size_t r = 0; r < rois.size(); ++r) {
    found[r] = false;
    const std::uint32_t x0 = std::min(rois[r].x0, im.width), y0 = std::min(rois[r].y0, im.height);
    const std::uint32_t x1 = std::min(im.width, x0 + rois[r].width);
    const std::uint32_t y1 = std::min(im.height, y0 + rois[r].height);
    if (x1 - x0 < 3 || y1 - y0 < 3) continue;
    const std::uint32_t rw = x1 - x0, rh = y1 - y0;
    if (static_cast<std::size_t>(rw) * rh > img_.size()) return Status::kCapacityExceeded;
    // Border statistics.
    double s = 0, s2 = 0;
    std::size_t nb = 0;
    for (std::uint32_t x = x0; x < x1; ++x) {
      for (std::uint32_t y : {y0, y1 - 1}) {
        const auto v = static_cast<double>(value(x, y));
        s += v;
        s2 += v * v;
        ++nb;
      }
    }
    for (std::uint32_t y = y0 + 1; y + 1 < y1; ++y) {
      for (std::uint32_t x : {x0, x1 - 1}) {
        const auto v = static_cast<double>(value(x, y));
        s += v;
        s2 += v * v;
        ++nb;
      }
    }
    const double mean = s / static_cast<double>(nb);
    const double sd = std::sqrt(std::max(0.0, s2 / static_cast<double>(nb) - mean * mean));
    const double th = mean + static_cast<double>(rc.sigma) * std::max(sd, 0.5);
    // Residual and labels in ROI-local coordinates.
    float* res = img_.data();
    std::uint32_t* lab = labels_.data();
    for (std::uint32_t y = 0; y < rh; ++y) {
      for (std::uint32_t x = 0; x < rw; ++x) {
        res[static_cast<std::size_t>(y) * rw + x] =
            static_cast<float>(static_cast<double>(value(x0 + x, y0 + y)) - mean);
      }
    }
    const auto max_prov = static_cast<std::uint32_t>(parent_.size() - 1);
    std::uint32_t nprov = 0;
    for (std::uint32_t y = 0; y < rh; ++y) {
      for (std::uint32_t x = 0; x < rw; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * rw + x;
        if (!(static_cast<double>(res[i]) + mean > th)) {
          lab[i] = kNoLabel;
          continue;
        }
        const std::uint32_t left = x > 0 ? lab[i - 1] : kNoLabel;
        const std::uint32_t up = y > 0 ? lab[i - rw] : kNoLabel;
        if (left == kNoLabel && up == kNoLabel) {
          if (nprov >= max_prov) {
            lab[i] = kNoLabel;
            continue;
          }
          parent_[nprov] = nprov;
          lab[i] = nprov++;
        } else if (left == kNoLabel || up == kNoLabel) {
          lab[i] = left == kNoLabel ? up : left;
        } else {
          const std::uint32_t a = find_root(parent_, left), b = find_root(parent_, up);
          if (a != b) parent_[std::max(a, b)] = std::min(a, b);
          lab[i] = std::min(a, b);
        }
      }
    }
    if (nprov == 0) continue;
    // Accumulate per root (roots < nprov; use blobs_ for area and line_ for sums, capped).
    const std::uint32_t nacc = std::min<std::uint32_t>(
        nprov, static_cast<std::uint32_t>(std::min(blobs_.size(), line_.size() / 3)));
    double* sum = line_.data();
    double* sx = sum + nacc;
    double* sy = sx + nacc;
    for (std::uint32_t p = 0; p < nacc; ++p) {
      sum[p] = sx[p] = sy[p] = 0;
      blobs_[p].area = 0;
    }
    for (std::uint32_t y = 0; y < rh; ++y) {
      for (std::uint32_t x = 0; x < rw; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * rw + x;
        if (lab[i] == kNoLabel) continue;
        const std::uint32_t root = find_root(parent_, lab[i]);
        if (root >= nacc) continue;
        const auto a = static_cast<double>(res[i]);
        sum[root] += a;
        sx[root] += a * x;
        sy[root] += a * y;
        ++blobs_[root].area;
      }
    }
    std::int64_t best = -1;
    for (std::uint32_t p = 0; p < nacc; ++p) {
      const std::uint32_t area = blobs_[p].area;
      if (area == 0 || area < rc.min_area || (rc.max_area != 0 && area > rc.max_area)) continue;
      if (best < 0 || sum[p] > sum[static_cast<std::size_t>(best)]) best = p;
    }
    if (best < 0) continue;
    const auto b = static_cast<std::size_t>(best);
    Spot sp;
    sp.sum = sum[b];
    sp.area = blobs_[b].area;
    sp.x = sx[b] / sum[b] + 0.5 + x0;
    sp.y = sy[b] / sum[b] + 0.5 + y0;
    if (rc.max_axis_ratio > 0) {
      double cxx = 0, cyy = 0, cxy = 0;
      const double mx = sp.x - 0.5 - x0, my = sp.y - 0.5 - y0;
      for (std::uint32_t y = 0; y < rh; ++y) {
        for (std::uint32_t x = 0; x < rw; ++x) {
          const std::size_t i = static_cast<std::size_t>(y) * rw + x;
          if (lab[i] == kNoLabel || find_root(parent_, lab[i]) != b) continue;
          const auto a = static_cast<double>(res[i]);
          cxx += (x - mx) * (x - mx) * a;
          cyy += (y - my) * (y - my) * a;
          cxy += (x - mx) * (y - my) * a;
        }
      }
      cxx = std::max(0.0, cxx / sp.sum);
      cyy = std::max(0.0, cyy / sp.sum);
      cxy /= sp.sum;
      const double root = std::sqrt((cxx - cyy) * (cxx - cyy) + 4 * cxy * cxy);
      const double major = std::sqrt(2 * (cxx + cyy + root));
      const double minor = std::sqrt(2 * std::max(0.0, cxx + cyy - root));
      sp.axis_ratio = major / std::max(minor, 1e-9);
      if (minor <= 0 || sp.axis_ratio > rc.max_axis_ratio) continue;
    }
    out[r] = sp;
    found[r] = true;
  }
  return Status::kOk;
}

}  // namespace st

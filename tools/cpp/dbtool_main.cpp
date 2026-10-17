// SPDX-License-Identifier: Apache-2.0
// st_dbtool: build, convert and inspect database blobs.
//   st_dbtool pack <dir> <out.stdb>        arrays (.npy) + props.txt -> blob (used by the
//                                         tetra3 .npz converter tools/convert_tetra3_npz.py)
//   st_dbtool generate --catalog hip_main.dat --max-fov DEG [options] -o out.stdb
//   st_dbtool info <file.stdb>
//   st_dbtool verify <file.stdb>           full validation, including the probe bound
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "db_writer.hpp"
#include "generator.hpp"
#include "npy.hpp"
#include "st/db/database.hpp"

using namespace st;
using namespace st::tools;

namespace {

std::map<std::string, std::string> read_props(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  std::map<std::string, std::string> m;
  std::string line;
  while (std::getline(f, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    m[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return m;
}

double num(const std::map<std::string, std::string>& m, const std::string& k, double def) {
  auto it = m.find(k);
  if (it == m.end() || it->second.empty() || it->second == "None") return def;
  if (it->second == "nan") return std::nan("");
  return std::stod(it->second);
}
std::pair<double, double> pair_of(const std::map<std::string, std::string>& m,
                                  const std::string& k) {
  auto it = m.find(k);
  if (it == m.end()) return {std::nan(""), std::nan("")};
  std::stringstream ss(it->second);
  std::string a, b;
  std::getline(ss, a, ',');
  std::getline(ss, b, ',');
  auto p = [](const std::string& s) {
    return (s.empty() || s == "nan" || s == "None") ? std::nan("") : std::stod(s);
  };
  return {p(a), p(b)};
}
db::CatalogKind catalog_kind(const std::string& s) {
  if (s == "hip_main") return db::CatalogKind::kHipMain;
  if (s == "bsc5") return db::CatalogKind::kBsc5;
  if (s == "tyc_main") return db::CatalogKind::kTycMain;
  return db::CatalogKind::kUnknown;
}
bool exists(const std::string& p) { return std::ifstream(p).good(); }

int cmd_pack(const std::string& dir, const std::string& out) {
  const auto props = read_props(dir + "/props.txt");
  DbContents c;
  db::Header& h = c.header;
  const NpyArray st = read_npy(dir + "/star_table.npy");
  if (st.shape.size() != 2 || st.shape[1] != 6) throw std::runtime_error("star_table must be Nx6");
  c.star_table.resize(st.count());
  for (std::size_t i = 0; i < st.count(); ++i)
    c.star_table[i] = static_cast<float>(st.as_double(i));
  const NpyArray pc = read_npy(dir + "/pattern_catalog.npy");
  if (pc.shape.size() != 2 || pc.shape[1] != 4)
    throw std::runtime_error("pattern_catalog must be Mx4");
  c.slots.resize(pc.shape[0]);
  for (std::size_t i = 0; i < pc.shape[0]; ++i)
    for (std::size_t k = 0; k < 4; ++k)
      c.slots[i][k] = static_cast<std::uint32_t>(pc.as_u64(i * 4 + k));
  if (exists(dir + "/star_catalog_IDs.npy")) {
    const NpyArray ids = read_npy(dir + "/star_catalog_IDs.npy");
    if (ids.shape.size() == 1) {
      for (std::size_t i = 0; i < ids.count(); ++i)
        c.catalog_ids.push_back(static_cast<std::uint32_t>(ids.as_u64(i)));
    } else {
      std::cerr << "note: multi-column catalog IDs (tyc_main) are not stored\n";
    }
  }
  if (exists(dir + "/pattern_largest_edge.npy")) {
    const NpyArray le = read_npy(dir + "/pattern_largest_edge.npy");
    if (le.dtype != "<f2") throw std::runtime_error("pattern_largest_edge must be float16");
    c.largest_edge_half.resize(le.count());
    std::memcpy(c.largest_edge_half.data(), le.data.data(), le.data.size());
  }
  if (exists(dir + "/pattern_key_hashes.npy")) {
    const NpyArray kh = read_npy(dir + "/pattern_key_hashes.npy");
    if (kh.dtype != "<u2") throw std::runtime_error("pattern_key_hashes must be uint16");
    c.key_hash.resize(kh.count());
    std::memcpy(c.key_hash.data(), kh.data.data(), kh.data.size());
  }
  constexpr double d2r = 3.14159265358979323846 / 180.0;
  h.pattern_bins = static_cast<std::uint16_t>(num(props, "pattern_bins", 0));
  h.pattern_max_error = num(props, "pattern_max_error", 0);
  h.max_fov = num(props, "max_fov", 0) * d2r;
  h.min_fov = num(props, "min_fov", num(props, "max_fov", 0)) * d2r;
  h.star_max_magnitude = num(props, "star_max_magnitude", std::nan(""));
  h.epoch_proper_motion = num(props, "epoch_proper_motion", std::nan(""));
  h.epoch_equinox = num(props, "epoch_equinox", 2000);
  const auto ra = pair_of(props, "range_ra"), dec = pair_of(props, "range_dec");
  h.range_ra[0] = ra.first;
  h.range_ra[1] = ra.second;  // tetra3 stores radians (quirk e)
  h.range_dec[0] = dec.first;
  h.range_dec[1] = dec.second;
  h.pattern_stars_per_fov = static_cast<std::uint16_t>(num(props, "pattern_stars_per_fov", 0));
  h.verification_stars_per_fov =
      static_cast<std::uint16_t>(num(props, "verification_stars_per_fov", 0));
  h.patterns_per_lattice_field =
      static_cast<std::uint32_t>(num(props, "patterns_per_lattice_field", 0));
  h.catalog = catalog_kind(props.count("star_catalog") ? props.at("star_catalog") : "");
  const bool presorted = props.count("presort_patterns") && props.at("presort_patterns") == "True";
  const bool simplify = props.count("simplify_pattern") && props.at("simplify_pattern") == "True";
  h.flags = (presorted ? db::kFlagPresorted : 0u) | (simplify ? db::kFlagSimplifyPattern : 0u);
  const std::string probe = props.count("probe") ? props.at("probe") : "quadratic";
  h.probe_mode = probe == "linear" ? db::ProbeMode::kLinear : db::ProbeMode::kQuadraticMagic;
  // tetra3 divides the match threshold by slots // 2; cedar-solve stores num_patterns.
  h.threshold_patterns = static_cast<std::uint32_t>(
      num(props, "num_patterns", static_cast<double>(c.slots.size() / 2)));
  const auto t0 = std::chrono::steady_clock::now();
  auto blob = build_blob(std::move(c));
  write_file(out, blob);
  std::cout << "wrote " << out << " (" << blob.size() << " bytes) in "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
            << " s\n";
  return 0;
}

int cmd_info(const std::string& path, bool deep) {
  auto bytes = read_file(path);
  Database::OpenOptions o;
  o.validate_probe_bound = deep;
  auto d = Database::open(bytes, o);
  if (!d.ok()) {
    std::cerr << "open failed: " << to_string(d.status()) << "\n";
    return 1;
  }
  const auto& h = d.value().header();
  std::printf("file_size            %llu\n", static_cast<unsigned long long>(h.file_size));
  std::printf("num_stars            %u\nnum_slots            %u\nnum_patterns         %u\n",
              h.num_stars, h.num_slots, h.num_patterns);
  std::printf("threshold_patterns   %u\nmax_probe_chain      %u\n", h.threshold_patterns,
              h.max_probe_chain);
  std::printf("pattern_bins         %u\npattern_max_error    %.9g\n", h.pattern_bins,
              h.pattern_max_error);
  std::printf("fov (deg)            %.6g .. %.6g\n", h.min_fov * 180 / 3.14159265358979323846,
              h.max_fov * 180 / 3.14159265358979323846);
  std::printf("star_max_magnitude   %.4g\nepoch_proper_motion  %.6g\n", h.star_max_magnitude,
              h.epoch_proper_motion);
  std::printf("star_index_bytes     %u\nprobe_mode           %s\nflags                0x%x\n",
              h.star_index_bytes, h.probe_mode == db::ProbeMode::kLinear ? "linear" : "quadratic",
              h.flags);
  std::printf("catalog_ids          %s\nkey_hash             %s\nlargest_edge         %s\n",
              d.value().has_catalog_ids() ? "yes" : "no", d.value().has_key_hash() ? "yes" : "no",
              d.value().has_largest_edge() ? "yes" : "no");
  if (deep) std::printf("verify               ok\n");
  return 0;
}

int cmd_generate(int argc, char** argv) {
  GeneratorConfig cfg;
  std::string catalog, out, sets_out;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
      return argv[++i];
    };
    auto range = [&]() {
      auto s = next();
      auto c = s.find(',');
      return std::make_pair(std::stod(s.substr(0, c)), std::stod(s.substr(c + 1)));
    };
    if (a == "--catalog")
      catalog = next();
    else if (a == "--max-fov")
      cfg.max_fov_deg = std::stod(next());
    else if (a == "--min-fov")
      cfg.min_fov_deg = std::stod(next());
    else if (a == "--mag")
      cfg.star_max_magnitude = std::stod(next());
    else if (a == "--epoch") {
      auto e = next();
      if (e != "none") cfg.epoch_proper_motion = std::stod(e);
    } else if (a == "--pattern-stars")
      cfg.pattern_stars_per_fov = std::stoi(next());
    else if (a == "--verification-stars")
      cfg.verification_stars_per_fov = std::stoi(next());
    else if (a == "--max-error")
      cfg.pattern_max_error = std::stod(next());
    else if (a == "--multiscale-step")
      cfg.multiscale_step = std::stod(next());
    else if (a == "--simplify")
      cfg.simplify_pattern = true;
    else if (a == "--no-presort")
      cfg.presort_patterns = false;
    else if (a == "--largest-edge")
      cfg.save_largest_edge = true;
    else if (a == "--range-ra")
      cfg.range_ra_deg = range();
    else if (a == "--range-dec")
      cfg.range_dec_deg = range();
    else if (a == "--lattice")
      cfg.lattice = true;
    else if (a == "--lattice-oversampling")
      cfg.lattice_field_oversampling = std::stoi(next());
    else if (a == "--patterns-per-field")
      cfg.patterns_per_lattice_field = std::stoi(next());
    else if (a == "--linear-probe")
      cfg.linear_probe = true;
    else if (a == "--sets-out")
      sets_out = next();
    else if (a == "-o")
      out = next();
    else
      throw std::runtime_error("unknown option " + a);
  }
  if (catalog.empty() || out.empty() || !(cfg.max_fov_deg > 0))
    throw std::runtime_error("need --catalog, --max-fov and -o");
  const auto t0 = std::chrono::steady_clock::now();
  const auto stars = read_catalog(catalog);
  GeneratorReport rep;
  std::vector<std::array<std::uint32_t, 4>> sets;
  auto contents = generate_database(stars, cfg, &rep, sets_out.empty() ? nullptr : &sets);
  auto blob = build_blob(std::move(contents));
  write_file(out, blob);
  std::cout << "stars " << rep.catalog_stars << ", pattern stars " << rep.pattern_stars
            << ", verification stars " << rep.verification_stars << ", patterns " << rep.patterns
            << ", bytes " << blob.size() << ", "
            << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
            << " s\n";
  if (!sets_out.empty()) {
    std::ofstream f(sets_out);
    for (const auto& s : sets) f << s[0] << ' ' << s[1] << ' ' << s[2] << ' ' << s[3] << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "pack" && argc == 4) return cmd_pack(argv[2], argv[3]);
    if (cmd == "info" && argc == 3) return cmd_info(argv[2], false);
    if (cmd == "verify" && argc == 3) return cmd_info(argv[2], true);
    if (cmd == "generate") return cmd_generate(argc, argv);
    std::cerr << "usage: st_dbtool pack <dir> <out> | generate ... | info <file> | verify <file>\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}

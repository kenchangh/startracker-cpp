// SPDX-License-Identifier: Apache-2.0
#pragma once
// Locates downloaded fixtures (tools/fetch_fixtures.py). Tests skip when a fixture is missing.
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

namespace st::test {

inline std::string fixtures_dir() {
  if (const char* e = std::getenv("ST_FIXTURES")) return e;
  return ST_FIXTURES_DIR;
}

inline std::optional<std::string> fixture(const std::string& name) {
  const std::string p = fixtures_dir() + "/" + name;
  if (std::ifstream(p).good()) return p;
  return std::nullopt;
}

}  // namespace st::test

#define ST_REQUIRE_FIXTURE(var, name)                                                              \
  const auto var##_opt = ::st::test::fixture(name);                                                \
  if (!var##_opt) GTEST_SKIP() << "fixture " << (name) << " missing; run tools/fetch_fixtures.py"; \
  const std::string& var = *var##_opt

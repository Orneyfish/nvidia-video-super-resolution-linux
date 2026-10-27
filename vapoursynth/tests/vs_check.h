// PASS/FAIL bookkeeping for the plugin tests (the idea of core/tests/test_util.h without its libnvvsr
// dependency). One line per check, like run.sh; the exit status of main() is finish().
#pragma once

#include <cstdio>
#include <string>

namespace vt {

inline int &checks() {
  static int n = 0;
  return n;
}
inline int &failures() {
  static int n = 0;
  return n;
}

inline bool check(bool ok, const std::string &name, const std::string &detail = "") {
  ++checks();
  if (!ok) ++failures();
  if (ok) std::printf("PASS %s\n", name.c_str());
  else std::printf("FAIL %s: %s\n", name.c_str(), detail.c_str());
  std::fflush(stdout);
  return ok;
}

inline bool check_eq(const std::string &name, const std::string &got, const std::string &want) {
  return check(got == want, name, "got '" + got + "', expected '" + want + "'");
}

// Exit status: 0 all checks passed, 1 otherwise.
inline int finish(const char *test) {
  std::printf("%s: %d checks, %d failed\n", test, checks(), failures());
  return failures() ? 1 : 0;
}

}  // namespace vt

// Header-only test checks for the panel (no NVIDIA or nvvsr dependency).
// Each CHECK prints one PASS or FAIL line; finish() prints the totals and returns non-zero on failure.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <sstream>
#include <string>

inline int check_count = 0;
inline int check_failures = 0;

__attribute__((format(printf, 5, 6))) inline bool check_report(bool ok, const char *file, int line,
                                                              const char *what, const char *fmt, ...) {
  ++check_count;
  if (!ok) ++check_failures;
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  std::printf("%s %s:%d: %s -- %s\n", ok ? "PASS" : "FAIL", file, line, what, msg);
  std::fflush(stdout);
  return ok;
}

template <class A, class B>
bool check_eq(const A &a, const B &b, const char *file, int line, const char *what) {
  const bool ok = (a == b);
  std::ostringstream os;
  if (!ok) os << "got [" << a << "] expected [" << b << "]";
  return check_report(ok, file, line, what, "%s", os.str().c_str());
}

inline int finish(const char *name) {
  std::printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
  return check_failures ? 1 : 0;
}

#define CHECK(cond, ...) check_report(static_cast<bool>(cond), __FILE__, __LINE__, #cond, __VA_ARGS__)
#define CHECK_EQ(a, b) check_eq((a), (b), __FILE__, __LINE__, #a " == " #b)

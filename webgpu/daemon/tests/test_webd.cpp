// ctest for nvvsr_webd (GPU; exit 77 = skip when libnvvsr is unusable): runs the daemon as a child on an ephemeral port and
// speaks WebSocket to it. Checks the RFC 6455 accept example, handshake refusals, byte identity with nvvsr_process, garbage
// and hostile input (the daemon must keep serving), DROP / ordering of back-to-back frames.
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvvsr.h"

using Bytes = std::vector<uint8_t>;
static int fails = 0, checks = 0;
static void check(bool ok, const std::string &name) {
  checks++;
  if (!ok) fails++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
  fflush(stdout);
}

enum { IW = 640, IH = 360, OW = 1280, OH = 720 };
static int PORT;

// ------------------------------------------------------------------ WebSocket client
static int dial() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  timeval tv{30, 0};  // a hung daemon fails the test instead of hanging it
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  if (connect(fd, (sockaddr *)&a, sizeof a)) { close(fd); return -1; }
  return fd;
}
static void put(int fd, const void *p, size_t n) {
  for (auto *c = (const uint8_t *)p; n;) { ssize_t r = send(fd, c, n, MSG_NOSIGNAL); if (r <= 0) return; c += r; n -= r; }
}
static bool get(int fd, void *p, size_t n) {
  for (auto *c = (uint8_t *)p; n;) { ssize_t r = recv(fd, c, n, 0); if (r <= 0) return false; c += r; n -= r; }
  return true;
}
// Sends the upgrade request, returns the HTTP response head.
static std::string upgrade(int fd, const std::string &key, const std::string &origin = "", const std::string &host = "") {
  std::string r = "GET /vsr HTTP/1.1\r\nHost: " + (host.empty() ? "127.0.0.1:" + std::to_string(PORT) : host) +
                  "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n";
  if (!origin.empty()) r += "Origin: " + origin + "\r\n";
  put(fd, (r + "\r\n").data(), r.size() + 2);
  std::string resp;
  for (char ch; (resp.size() < 4 || resp.compare(resp.size() - 4, 4, "\r\n\r\n")) && get(fd, &ch, 1);) resp += ch;
  return resp;
}
static int ws_open(const char *origin = "chrome-extension://abcdefgh") {
  int fd = dial();
  if (fd >= 0 && upgrade(fd, "dGhlIHNhbXBsZSBub25jZQ==", origin).find(" 101 ") == std::string::npos) { close(fd); return -1; }
  return fd;
}
static Bytes ws_frame(int op, const uint8_t *p, size_t n, bool fin = true) {
  const uint8_t k[4] = {0x12, 0x34, 0x56, 0x78};
  Bytes f{uint8_t((fin ? 0x80 : 0) | op)};
  if (n < 126) f.push_back(0x80 | n);
  else if (n < 65536) { f.push_back(0x80 | 126); f.push_back(n >> 8); f.push_back(n); }
  else { f.push_back(0x80 | 127); for (int i = 7; i >= 0; i--) f.push_back((uint64_t)n >> (8 * i)); }
  f.insert(f.end(), k, k + 4);
  const size_t o = f.size();
  f.insert(f.end(), p, p + n);
  for (size_t i = 0; i < n; i++) f[o + i] ^= k[i & 3];
  return f;
}
static void ws_send(int fd, int op, const uint8_t *p, size_t n, bool fin = true) { const Bytes f = ws_frame(op, p, n, fin); put(fd, f.data(), f.size()); }
struct Msg { int op = -1; Bytes p; std::string text() const { return std::string(p.begin(), p.end()); } };
static bool ws_recv(int fd, Msg *m) {  // one server frame (the daemon never fragments)
  uint8_t h[2], e[8];
  if (!get(fd, h, 2)) return false;
  uint64_t len = h[1] & 127;
  if (len >= 126) { int n = len == 126 ? 2 : 8; if (!get(fd, e, n)) return false; for (int i = len = 0; i < n; i++) len = len << 8 | e[i]; }
  m->op = h[0] & 15;
  m->p.resize(len);
  return get(fd, m->p.data(), len);
}

// ------------------------------------------------------------------ frames
static Bytes pattern(int w, int h) {
  Bytes b((size_t)w * h * 3 / 2);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) b[(size_t)y * w + x] = 20 + ((x * 3 + y * 2 + (x * y >> 6)) & 127);
  for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) {
    b[(size_t)w * h + (size_t)y * (w / 2) + x] = 110 + ((x - y) & 31);
    b[(size_t)w * h * 5 / 4 + (size_t)y * (w / 2) + x] = 120 + ((x ^ y) & 31);
  }
  return b;
}
static Bytes request(int iw, int ih, int ow, int oh, int q, int flags, int seq, const Bytes &planes) {
  Bytes m(16);
  memcpy(m.data(), "NVSR", 4);
  const uint16_t v[4] = {uint16_t(iw), uint16_t(ih), uint16_t(ow), uint16_t(oh)}, s = seq;  // little endian host
  memcpy(&m[4], v, 8); m[12] = q; m[13] = flags; memcpy(&m[14], &s, 2);
  m.insert(m.end(), planes.begin(), planes.end());
  return m;
}
// libnvvsr called directly with the config the daemon builds for (q, full).
static Bytes reference(const Bytes &in, int q, bool full) {
  nvvsr_config cfg; nvvsr_config_init(&cfg);
  cfg.in_w = IW; cfg.in_h = IH; cfg.out_w = OW; cfg.out_h = OH; cfg.quality = q ? q : NVVSR_QUALITY_AUTO;
  cfg.in_format = NVVSR_IN_I420; cfg.out_format = NVVSR_OUT_I420; cfg.full_range = full;
  nvvsr_ctx *ctx = nullptr;
  Bytes out((size_t)OW * OH * 3 / 2);
  if (nvvsr_create(&cfg, &ctx) != NVVSR_OK) return {};
  nvvsr_image_in a{}; nvvsr_image_out b{};
  a.plane[0] = in.data(); a.stride[0] = IW; a.plane[1] = a.plane[0] + IW * IH; a.stride[1] = IW / 2; a.plane[2] = a.plane[1] + IW * IH / 4; a.stride[2] = IW / 2;
  b.plane[0] = out.data(); b.stride[0] = OW; b.plane[1] = b.plane[0] + OW * OH; b.stride[1] = OW / 2; b.plane[2] = b.plane[1] + OW * OH / 4; b.stride[2] = OW / 2;
  if (nvvsr_process(ctx, &a, &b) != NVVSR_OK) out.clear();
  nvvsr_destroy(ctx);
  return out;
}
// A binary reply with the right header for `seq`; *planes gets the payload.
static bool is_result(const Msg &m, int seq, Bytes *planes = nullptr) {
  float ms;
  if (m.op != 2 || m.p.size() != 16 + (size_t)OW * OH * 3 / 2 || memcmp(m.p.data(), "NVSO", 4)) return false;
  uint16_t w, h, s;
  memcpy(&w, &m.p[4], 2); memcpy(&h, &m.p[6], 2); memcpy(&s, &m.p[8], 2); memcpy(&ms, &m.p[12], 4);
  if (w != OW || h != OH || s != seq || !(ms > 0 && ms < 5000)) return false;
  if (planes) planes->assign(m.p.begin() + 16, m.p.end());
  return true;
}
static std::string expect_err(int fd, const Bytes &frame) {
  ws_send(fd, 2, frame.data(), frame.size());
  Msg m;
  return ws_recv(fd, &m) && m.op == 1 ? m.text() : "(no text reply)";
}

int main() {
  if (const char *d = getenv("NVVSR_LIBDIR_DEFAULT")) setenv("NVVSR_LIBDIR", d, 0);
  signal(SIGPIPE, SIG_IGN);
  if (int rc = nvvsr_probe()) { printf("test_webd: SKIP: SR unavailable: %s (%s)\n", nvvsr_strerror(rc), nvvsr_last_error()); return 77; }

  setenv("NVVSR_WEBD_ORIGINS", "https://env.example,", 1);  // the child reads it (trailing comma = empty entry, ignored)
  int pfd[2];
  if (pipe(pfd)) return 1;
  const pid_t pid = fork();
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    dup2(pfd[1], 1);
    execl(NVVSR_WEBD_BIN, "nvvsr_webd", "0", "--origin", "https://allowed.example", (char *)nullptr);  // plus env.example from the env
    _exit(127);
  }
  close(pfd[1]);
  char line[64] = {};
  pollfd pl{pfd[0], POLLIN, 0};
  if (poll(&pl, 1, 10000) != 1 || read(pfd[0], line, sizeof line - 1) <= 0 || sscanf(line, "LISTENING %d", &PORT) != 1) { printf("daemon did not start\n"); kill(pid, SIGKILL); return 1; }

  // 1. handshake: RFC 6455 example, then the refusals
  int fd = dial();
  const std::string r = upgrade(fd, "dGhlIHNhbXBsZSBub25jZQ==");
  check(r.find(" 101 ") != std::string::npos && r.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos, "RFC 6455 accept key");
  close(fd);
  fd = dial(); check(upgrade(fd, "dGhlIHNhbXBsZSBub25jZQ==", "https://evil.example").find(" 403 ") != std::string::npos, "web page Origin refused"); close(fd);
  fd = dial(); check(upgrade(fd, "dGhlIHNhbXBsZSBub25jZQ==", "", "rebind.example:80").find(" 403 ") != std::string::npos, "non-loopback Host refused"); close(fd);
  fd = dial(); check(upgrade(fd, "short").find(" 400 ") != std::string::npos, "bad Sec-WebSocket-Key refused"); close(fd);
  fd = dial(); const std::string plain = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"; put(fd, plain.data(), plain.size()); { char b[16] = {}; get(fd, b, 12); check(!strncmp(b, "HTTP/1.1 400", 12), "plain GET refused"); } close(fd);

  // 2. good frames equal nvvsr_process; ping; fragmented message
  const Bytes in = pattern(IW, IH);
  fd = ws_open();
  check(fd >= 0, "extension Origin accepted");
  uint8_t ping[2] = {'h', 'i'};
  ws_send(fd, 9, ping, 2);
  Msg m;
  check(ws_recv(fd, &m) && m.op == 10 && m.text() == "hi", "ping -> pong");
  Bytes got, want = reference(in, 3, false), req = request(IW, IH, OW, OH, 3, 0, 7, in);
  ws_send(fd, 2, req.data(), req.size());
  check(ws_recv(fd, &m) && is_result(m, 7, &got), "reply header 1280x720 seq 7");
  check(!want.empty() && got == want, "HIGH limited: payload identical to nvvsr_process");
  Bytes fq = request(IW, IH, OW, OH, 0, 1, 8, in);  // auto quality, full range: the context is recreated
  ws_send(fd, 2, fq.data(), fq.size());
  check(ws_recv(fd, &m) && is_result(m, 8, &got) && got == reference(in, 0, true), "auto + full range: identical to nvvsr_process");
  const size_t a = 1000, b = 200000;  // 3 fragments with a ping in between
  ws_send(fd, 2, req.data(), a, false);
  ws_send(fd, 9, ping, 2);
  ws_send(fd, 0, req.data() + a, b - a, false);
  ws_send(fd, 0, req.data() + b, req.size() - b, true);
  Msg p1, p2;
  check(ws_recv(fd, &p1) && p1.op == 10 && ws_recv(fd, &p2) && is_result(p2, 7, &got) && got == want, "fragmented message + ping inside");

  // 2b. page origins from --origin and NVVSR_WEBD_ORIGINS are served; anything that is not an exact match is not
  for (const char *o : {"https://allowed.example", "https://env.example"}) {
    int pf = ws_open(o);
    ws_send(pf, 2, req.data(), req.size());
    check(pf >= 0 && ws_recv(pf, &m) && is_result(m, 7, &got) && got == want, std::string("allowed page origin served: ") + o);
    close(pf);
  }
  for (const char *o : {"https://allowed.example.evil.com", "http://allowed.example", "https://allowed.example:8443", "https://other.example"}) {
    int px = dial();
    check(upgrade(px, "dGhlIHNhbXBsZSBub25jZQ==", o).find(" 403 ") != std::string::npos, std::string("other page origin refused: ") + o);
    close(px);
  }

  // 3. garbage on one connection: ERR each time, then a good frame is served
  Bytes bad = req;
  bad[0] = 'X';
  check(expect_err(fd, bad).rfind("ERR", 0) == 0, "bad magic -> ERR");
  Bytes odd = request(641, 361, OW, OH, 3, 0, 1, Bytes((size_t)641 * 361 * 3 / 2));
  check(expect_err(fd, odd) == "ERR odd dimensions", "odd dimensions -> ERR");
  check(expect_err(fd, request(IW, IH, IW / 2, IH / 2, 3, 0, 1, in)) == "ERR bad scale", "downscale -> ERR");
  check(expect_err(fd, request(IW, IH, OW, OH, 9, 0, 1, in)) == "ERR bad quality", "bad quality -> ERR");
  check(expect_err(fd, request(32, 32, 64, 64, 3, 0, 1, Bytes(32 * 32 * 3 / 2))) == "ERR size out of range", "too small -> ERR");
  Bytes trunc(req.begin(), req.end() - 1), extra = req;
  extra.push_back(0);
  check(expect_err(fd, trunc) == "ERR bad length", "truncated payload -> ERR");
  check(expect_err(fd, extra) == "ERR bad length", "payload one byte too long -> ERR");
  check(expect_err(fd, Bytes(5, 0)).rfind("ERR", 0) == 0, "5-byte message -> ERR");
  check(expect_err(fd, Bytes(24u * 1024 * 1024 + 1, 0)) == "ERR message too big", "message over 24 MiB -> ERR");
  ws_send(fd, 1, ping, 2);
  check(ws_recv(fd, &m) && m.op == 1 && m.text().rfind("ERR", 0) == 0, "text message -> ERR");
  ws_send(fd, 2, req.data(), req.size());
  check(ws_recv(fd, &m) && is_result(m, 7, &got) && got == want, "same connection still serves a good frame");

  // 4. six frames back-to-back: every seq is answered once (result or "DROP <seq>"), results in order, the newest is served
  std::vector<Bytes> burst;  // framed and masked up front, so the sends are back to back whatever the client speed
  for (int s = 10; s < 16; s++) { Bytes f = request(IW, IH, OW, OH, 3, 0, s, in); burst.push_back(ws_frame(2, f.data(), f.size())); }
  for (const Bytes &f : burst) put(fd, f.data(), f.size());
  std::string seen;
  int answered[16] = {}, drops = 0, last_result = 0;
  bool ordered = true;
  for (int i = 0; i < 6 && ws_recv(fd, &m); i++) {
    int s = 0;
    if (m.op == 1 && sscanf(m.text().c_str(), "DROP %d", &s) == 1 && s >= 10 && s < 16) { answered[s]++; drops++; }
    else for (s = 10; s < 16; s++) if (is_result(m, s)) { answered[s]++; ordered = ordered && s > last_result; last_result = s; break; }
    seen += m.op == 1 ? m.text() + " " : "R" + std::to_string(s) + " ";
  }
  printf("  back-to-back replies: %s\n", seen.c_str());
  bool once = true;
  for (int s = 10; s < 16; s++) once = once && answered[s] == 1;
  check(once && ordered && last_result == 15, "back-to-back: each seq answered once, in order, newest served");
  check(drops >= 1, "back-to-back: a newer frame replaced a waiting one (DROP)");
  close(fd);

  // 5. hostile framing: close frame with the right code, and the daemon keeps serving new connections
  struct { const char *name; Bytes bytes; int code; } hostile[] = {
      {"unmasked client frame", {0x82, 0x04, 1, 2, 3, 4}, 1002},
      {"reserved bits set", {0xC2, 0x80, 1, 2, 3, 4}, 1002},
      {"fragmented control frame", {0x09, 0x80, 1, 2, 3, 4}, 1002},
      {"continuation without start", {0x80, 0x80, 1, 2, 3, 4}, 1002},
      {"64-bit length 2^40", {0x82, 0xFF, 0, 0, 1, 0, 0, 0, 0, 0, 1, 2, 3, 4}, 1009}};
  for (auto &hc : hostile) {
    int h = ws_open();
    put(h, hc.bytes.data(), hc.bytes.size());
    check(ws_recv(h, &m) && m.op == 8 && m.p.size() == 2 && (m.p[0] << 8 | m.p[1]) == hc.code, std::string("hostile: ") + hc.name + " -> close");
    close(h);
  }
  fd = ws_open();
  ws_send(fd, 2, req.data(), req.size());
  check(fd >= 0 && ws_recv(fd, &m) && is_result(m, 7, &got) && got == want, "new connection served after hostile input");
  close(fd);

  kill(pid, SIGTERM);
  int st = 0;
  waitpid(pid, &st, 0);
  check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "daemon was alive until SIGTERM (no crash)");
  printf("test_webd: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

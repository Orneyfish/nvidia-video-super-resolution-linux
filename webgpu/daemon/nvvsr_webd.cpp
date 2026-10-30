// nvvsr_webd: localhost WebSocket daemon. A browser extension sends decoded I420 frames, libnvvsr (NVIDIA VSR, tensor
// cores) upscales them, the reply is an I420 frame. Usage: nvvsr_webd [port=41777] [--origin ORIGIN]...   (0 = ephemeral; prints "LISTENING <port>")
//
// Protocol (RFC 6455, binary messages, little endian). Client -> server, 16-byte header + planes:
//   "NVSR" u16 in_w u16 in_h u16 out_w u16 out_h u8 quality(0 = auto, else nvvsr_quality: 1-4, 21, 23) u8 flags u16 seq
//   flags: bit0 = full range, bit1 = input is NV12 (Y, then interleaved UV pairs; in_w*in_h*3/2 bytes) instead of I420 (Y, U, V);
//   any other bit -> ERR. The reply is always I420.
// Server -> client: "NVSO" u16 out_w u16 out_h u16 seq u16 reserved f32 ms, then the upscaled I420 planes. Text replies:
//   "ERR <reason>" (bad frame or core error; the connection stays open), "DROP <seq>" (a newer frame replaced this one).
//
// Security: any web page can reach 127.0.0.1, and a WebSocket has no same-origin policy, so the handshake refuses a Host that is
// not loopback (DNS rebinding) and any Origin that is not an extension (browsers always send Origin; a page's is https://...).
// A content script sends its PAGE's origin: list those exactly with --origin (repeatable) or env NVVSR_WEBD_ORIGINS (comma
// separated). Each one lets every script on that site use the GPU, so list only sites you mean to upscale.
// shortcut: a reader and a worker thread per connection, at most 4 connections, no TLS, no auth beyond Origin; upgrade if the
// daemon ever listens beyond loopback.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nvvsr.h"

static const size_t MAX_MSG = 24u << 20;  // bigger messages are drained and answered with ERR
static const uint64_t HARD_FRAME = 1u << 30;  // a single frame claiming more closes the connection (1009)
static const int MAX_CONNS = 4;
static std::mutex gpu;  // serialises create/process/destroy across connections
static std::vector<std::string> allowed_origins;  // page origins besides extensions; written in main() before any thread starts

// ------------------------------------------------------------------ SHA-1 + base64 (Sec-WebSocket-Accept)
static std::string sha1(const std::string &s) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  std::string m = s + char(0x80);
  while (m.size() % 64 != 56) m += char(0);
  for (int i = 7; i >= 0; i--) m += char((uint64_t)s.size() * 8 >> (i * 8));
  auto rol = [](uint32_t x, int n) { return x << n | x >> (32 - n); };
  for (size_t o = 0; o < m.size(); o += 64) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) { w[i] = 0; for (int j = 0; j < 4; j++) w[i] = w[i] << 8 | (uint8_t)m[o + 4 * i + j]; }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
      uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 60 && i >= 40 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
      uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::string out;
  for (uint32_t x : h) for (int i = 3; i >= 0; i--) out += char(x >> (i * 8));
  return out;
}
static std::string b64(const std::string &s) {
  static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  for (size_t i = 0; i < s.size(); i += 3) {
    const bool b1 = i + 1 < s.size(), b2 = i + 2 < s.size();
    uint32_t v = (uint8_t)s[i] << 16 | (b1 ? (uint8_t)s[i + 1] << 8 : 0) | (b2 ? (uint8_t)s[i + 2] : 0);
    o += T[v >> 18]; o += T[v >> 12 & 63]; o += b1 ? T[v >> 6 & 63] : '='; o += b2 ? T[v & 63] : '=';
  }
  return o;
}

// ------------------------------------------------------------------ socket helpers
static bool recv_all(int fd, void *p, size_t n) {
  for (auto *c = (uint8_t *)p; n;) {
    ssize_t r = recv(fd, c, n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    c += r; n -= r;
  }
  return true;
}
static bool send_all(int fd, const void *p, size_t n) {
  for (auto *c = (const uint8_t *)p; n;) {
    ssize_t r = send(fd, c, n, MSG_NOSIGNAL);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    c += r; n -= r;
  }
  return true;
}
static void unmask(uint8_t *p, size_t n, const uint8_t k[4]) {
  uint64_t kk; for (int i = 0; i < 8; i++) ((uint8_t *)&kk)[i] = k[i & 3];
  size_t i = 0;
  for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); w ^= kk; memcpy(p + i, &w, 8); }
  for (; i < n; i++) p[i] ^= k[i & 3];
}

struct Job { int iw, ih, ow, oh, quality, flags, seq; std::vector<uint8_t> msg; };

struct Conn {
  int fd = -1;
  std::mutex send_mu;  // reader (ERR/DROP/pong) and worker (results) both send
  std::mutex mu;       // guards job and done
  std::condition_variable cv;
  std::optional<Job> job;  // the one waiting frame; a newer frame replaces it
  bool done = false;
  bool send_frame(int op, const void *p, size_t n) {
    uint8_t h[10] = {uint8_t(0x80 | op)};
    size_t hn = 2;
    if (n < 126) h[1] = n;
    else if (n < 65536) { h[1] = 126; h[2] = n >> 8; h[3] = n; hn = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint64_t)n >> (56 - 8 * i); hn = 10; }
    std::lock_guard<std::mutex> g(send_mu);
    const bool ok = send_all(fd, h, hn) && send_all(fd, p, n);
    if (!ok) shutdown(fd, SHUT_RDWR);  // a half-written frame would corrupt the stream: drop the connection
    return ok;
  }
  void text(const std::string &s) { send_frame(1, s.data(), s.size()); }
};

// ------------------------------------------------------------------ frames
// Trust boundary: returns nullptr and fills *j, or the reason for an ERR reply.
static const char *parse(const std::vector<uint8_t> &m, Job *j) {
  if (m.size() < 16 || memcmp(m.data(), "NVSR", 4)) return "bad magic";
  auto u16 = [&](int o) { return m[o] | m[o + 1] << 8; };
  *j = Job{u16(4), u16(6), u16(8), u16(10), m[12], m[13], u16(14), {}};
  if ((j->iw | j->ih | j->ow | j->oh) & 1) return "odd dimensions";
  if (j->iw < 64 || j->iw > 4096 || j->ow < 64 || j->ow > 4096 || j->ih < 64 || j->ih > 2304 || j->oh < 64 || j->oh > 2304)
    return "size out of range";
  if (j->ow < j->iw || j->oh < j->ih || j->ow > 4 * j->iw || j->oh > 4 * j->ih) return "bad scale";
  const int q = j->quality;
  if (q > 4 && q != 21 && q != 23) return "bad quality";
  if (j->flags & ~3) return "bad flags";
  if (m.size() != 16 + (size_t)j->iw * j->ih * 3 / 2) return "bad length";
  return nullptr;
}

static void submit(Conn &c, std::vector<uint8_t> msg) {
  Job j;
  if (const char *why = parse(msg, &j)) return c.text(std::string("ERR ") + why);
  j.msg = std::move(msg);
  int dropped = -1;
  { std::lock_guard<std::mutex> l(c.mu); if (c.job) dropped = c.job->seq; c.job = std::move(j); }
  c.cv.notify_one();
  if (dropped >= 0) c.text("DROP " + std::to_string(dropped));
}

static void work(Conn &c) {
  nvvsr_ctx *ctx = nullptr;  // one context for the current geometry/quality/range; recreated on change (the core recycles effects)
  std::array<int, 6> key{};
  for (;;) {
    Job j;
    { std::unique_lock<std::mutex> l(c.mu); c.cv.wait(l, [&] { return c.job || c.done; }); if (c.done) break; j = std::move(*c.job); c.job.reset(); }
    std::string err;
    std::vector<uint8_t> out;
    float ms = 0;
    const bool nv12 = j.flags & 2;
    {
      std::lock_guard<std::mutex> g(gpu);
      const std::array<int, 6> k{j.iw, j.ih, j.ow, j.oh, j.quality, j.flags};
      if (!ctx || k != key) {
        nvvsr_destroy(ctx); ctx = nullptr;
        nvvsr_config cfg; nvvsr_config_init(&cfg);
        cfg.in_w = j.iw; cfg.in_h = j.ih; cfg.out_w = j.ow; cfg.out_h = j.oh;
        cfg.quality = j.quality ? j.quality : NVVSR_QUALITY_AUTO;
        cfg.in_format = nv12 ? NVVSR_IN_NV12 : NVVSR_IN_I420; cfg.out_format = NVVSR_OUT_I420; cfg.full_range = j.flags & 1;
        if (nvvsr_create(&cfg, &ctx) != NVVSR_OK) err = std::string("create: ") + nvvsr_last_error(); else key = k;
      }
      if (ctx) {
        out.assign(16 + (size_t)j.ow * j.oh * 3 / 2, 0);
        nvvsr_image_in in{};
        in.plane[0] = j.msg.data() + 16; in.stride[0] = j.iw;
        in.plane[1] = in.plane[0] + (size_t)j.iw * j.ih; in.stride[1] = nv12 ? j.iw : j.iw / 2;  // NV12: one interleaved UV plane
        if (!nv12) { in.plane[2] = in.plane[1] + (size_t)(j.iw / 2) * (j.ih / 2); in.stride[2] = j.iw / 2; }
        nvvsr_image_out o{};
        o.plane[0] = out.data() + 16; o.stride[0] = j.ow;
        o.plane[1] = o.plane[0] + (size_t)j.ow * j.oh; o.stride[1] = j.ow / 2;
        o.plane[2] = o.plane[1] + (size_t)(j.ow / 2) * (j.oh / 2); o.stride[2] = j.ow / 2;
        nvvsr_timing t{};
        if (nvvsr_process(ctx, &in, &o) != NVVSR_OK) err = std::string("process: ") + nvvsr_last_error();
        else { nvvsr_last_timing(ctx, &t); ms = (float)t.total_ms; }
      }
    }
    if (!err.empty()) { c.text("ERR " + err); continue; }
    memcpy(out.data(), "NVSO", 4);
    const uint16_t w = j.ow, h = j.oh, s = j.seq;  // little endian host assumed (x86/arm64 Linux)
    memcpy(&out[4], &w, 2); memcpy(&out[6], &h, 2); memcpy(&out[8], &s, 2); memcpy(&out[12], &ms, 4);
    c.send_frame(2, out.data(), out.size());
  }
  std::lock_guard<std::mutex> g(gpu);
  nvvsr_destroy(ctx);
}

// ------------------------------------------------------------------ WebSocket
static bool handshake(Conn &c) {
  timeval tv{10, 0};
  setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);  // a silent client must not hold a connection slot
  std::string req;
  for (char ch; req.size() < 8192 && (req.size() < 4 || req.compare(req.size() - 4, 4, "\r\n\r\n")); req += ch)
    if (recv(c.fd, &ch, 1, 0) != 1) return false;  // byte at a time: nothing past the blank line is consumed
  auto reject = [&](const char *status) {
    std::string r = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(c.fd, r.data(), r.size());
    return false;
  };
  std::istringstream is(req);
  std::string line;
  std::getline(is, line);
  if (line.rfind("GET ", 0) != 0 || line.find(" HTTP/1.1") == std::string::npos) return reject("400 Bad Request");
  std::map<std::string, std::string> hd;
  while (std::getline(is, line) && line != "\r") {
    const size_t p = line.find(':');
    if (p == std::string::npos) continue;
    std::string name = line.substr(0, p), val = line.substr(p + 1);
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    val.erase(0, val.find_first_not_of(" \t"));
    while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
    hd[name] = val;
  }
  auto lower = [&](const char *n) { std::string v = hd[n]; std::transform(v.begin(), v.end(), v.begin(), ::tolower); return v; };
  if (lower("upgrade") != "websocket" || lower("connection").find("upgrade") == std::string::npos || hd["sec-websocket-version"] != "13" ||
      hd["sec-websocket-key"].size() != 24)
    return reject("400 Bad Request");
  std::string host = hd["host"];
  host = host.substr(0, host.rfind(':') == std::string::npos ? host.size() : host.rfind(':'));
  const bool has_origin = hd.count("origin");  // before any hd["origin"], which would insert it; non-browser clients send none
  const std::string origin = hd["origin"];
  const bool ext = origin.rfind("chrome-extension://", 0) == 0 || origin.rfind("moz-extension://", 0) == 0 ||
                   std::find(allowed_origins.begin(), allowed_origins.end(), origin) != allowed_origins.end();  // exact match
  if ((host != "127.0.0.1" && host != "localhost" && host != "[::1]") || (has_origin && !ext)) return reject("403 Forbidden");
  const std::string r = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                        b64(sha1(hd["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) + "\r\n\r\n";
  tv = {5, 0};
  setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  tv = {0, 0};
  setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);  // idle (paused video) is fine once upgraded
  return send_all(c.fd, r.data(), r.size());
}

static void read_loop(Conn &c) {
  std::vector<uint8_t> msg;
  int mop = 0;  // opcode of the message being assembled, 0 = none
  bool big = false;
  static thread_local uint8_t buf[65536];
  auto fail = [&](int code) { uint8_t b[2] = {uint8_t(code >> 8), uint8_t(code)}; c.send_frame(8, b, 2); };
  for (;;) {
    uint8_t h[2], k[4], e[8];
    if (!recv_all(c.fd, h, 2)) return;
    const int op = h[0] & 15;
    const bool fin = h[0] & 0x80;
    uint64_t len = h[1] & 127;
    if ((h[0] & 0x70) || !(h[1] & 0x80)) return fail(1002);  // RSV bits, or an unmasked client frame
    if (len >= 126) {
      const int n = len == 126 ? 2 : 8;
      if (!recv_all(c.fd, e, n)) return;
      for (int i = len = 0; i < n; i++) len = len << 8 | e[i];
    }
    if (!recv_all(c.fd, k, 4)) return;
    if (op >= 8) {  // control frame
      if (!fin || len > 125 || !recv_all(c.fd, buf, len)) return fail(1002);
      unmask(buf, len, k);
      if (op == 8) { c.send_frame(8, buf, len >= 2 ? 2 : 0); return; }
      if (op == 9) c.send_frame(10, buf, len);
      else if (op != 10) return fail(1002);
      continue;
    }
    if (op > 2 || (op == 0) == (mop == 0)) return fail(1002);  // a continuation needs a message in progress, a new one needs none
    if (op) { mop = op; msg.clear(); big = false; }
    if (len > HARD_FRAME) return fail(1009);
    big = big || msg.size() + len > MAX_MSG;
    if (big) {  // drain without storing, answer ERR at the end
      msg = {};
      for (uint64_t left = len; left;) {
        const size_t n = std::min<uint64_t>(left, sizeof buf);
        if (!recv_all(c.fd, buf, n)) return;
        left -= n;
      }
    } else {
      const size_t o = msg.size();
      msg.resize(o + len);
      if (!recv_all(c.fd, msg.data() + o, len)) return;
      unmask(msg.data() + o, len, k);
    }
    if (!fin) continue;
    if (big) c.text("ERR message too big");
    else if (mop == 2) submit(c, std::move(msg));
    else c.text("ERR binary messages only");
    msg.clear(); mop = 0;
  }
}

static std::atomic<int> conns{0};

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  int port = 41777, i = 1;
  if (i < argc && argv[i][0] != '-') port = atoi(argv[i++]);
  if (const char *env = getenv("NVVSR_WEBD_ORIGINS")) {
    std::istringstream is(env);
    for (std::string o; std::getline(is, o, ',');) if (!o.empty()) allowed_origins.push_back(o);
  }
  for (; i + 1 < argc && !strcmp(argv[i], "--origin"); i += 2) allowed_origins.push_back(argv[i + 1]);
  if (i != argc || port < 0 || port > 65535) { fprintf(stderr, "usage: nvvsr_webd [port] [--origin ORIGIN]...\n"); return 2; }
  std::string shown = "chrome-extension://*, moz-extension://*";
  for (const std::string &o : allowed_origins) shown += ", " + o;
  fprintf(stderr, "nvvsr_webd: allowed origins: %s (and clients that send no Origin)\n", shown.c_str());
  const int ls = socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // loopback only, never 0.0.0.0
  socklen_t al = sizeof a;
  if (ls < 0 || bind(ls, (sockaddr *)&a, sizeof a) || listen(ls, 8) || getsockname(ls, (sockaddr *)&a, &al)) { perror("nvvsr_webd"); return 1; }
  printf("LISTENING %d\n", ntohs(a.sin_port));
  fflush(stdout);
  for (;;) {
    const int fd = accept(ls, nullptr, nullptr);
    if (fd < 0) { if (errno != EINTR) usleep(10000); continue; }
    if (conns >= MAX_CONNS) { close(fd); continue; }
    conns++;
    std::thread([fd] {
      Conn c;
      c.fd = fd;
      if (handshake(c)) {
        std::thread w(work, std::ref(c));
        read_loop(c);
        { std::lock_guard<std::mutex> l(c.mu); c.done = true; }
        c.cv.notify_one();
        w.join();
        // Unread input at close() would turn into an RST that eats our close frame: finish the close, drain a little.
        shutdown(fd, SHUT_WR);
        timeval tv{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        uint8_t d[4096];
        for (size_t n = 0; n < (1u << 20);) { ssize_t r = recv(fd, d, sizeof d, 0); if (r <= 0) break; n += r; }
      }
      close(fd);
      conns--;
    }).detach();
  }
}

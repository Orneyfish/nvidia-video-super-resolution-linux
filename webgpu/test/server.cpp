// Test-harness helper for webgpu/test/run.sh (the only non-browser code besides the shell scripts).
//
//   server --webgpu DIR --media DIR --out DIR --wayland /abs/path/to/cage/wayland-0
//
// Two listeners on 127.0.0.1 (ephemeral ports, printed as "PORTS a b"):
//   A (primary): static files + helper endpoints used by harness.js
//   B (secondary origin): static files only, so a video served from B is cross-origin for a page served from A
// Static roots: /src/ (webgpu/src), /test/ (webgpu/test), /media/ (test clips). A "/cors/" prefix in front of any of them adds
// "Access-Control-Allow-Origin: *". Range requests are supported (needed to seek in <video>).
// Helper endpoints (A only):
//   POST /post?name=F      store the body as OUT/F (JSON/markdown results, PNG captures)
//   GET  /root?pid=N       process-tree root (cage) for the CPU monitor
//   GET  /mon?phase=start|stop   begin / end a measurement window; stop returns JSON (Chrome CPU per process type, nvidia-smi stats)
//   GET  /shot?name=N      grim screenshot of the headless cage output -> OUT/N.png
//   GET  /psnr?a=X&b=Y[&box=W:H:X:Y]   ffmpeg psnr between OUT/X.png and OUT/Y.png
//   GET  /mean?name=N[&box=W:H:X:Y]    mean colour "r,g,b" of OUT/N.png
//   GET  /key              wtype a key into cage (a real keydown = user activation for fullscreen / picture-in-picture)
//   GET  /smi              plain `nvidia-smi` text
// shortcut: thread per connection, Connection: close, no TLS; localhost test tool only. Names/boxes are validated before they reach a shell.
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
static string WEBGPU, MEDIA, OUT, WAYLAND;
static atomic<int> root_pid{0};

static double now_s() { return chrono::duration<double>(chrono::system_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------------------------------------ helpers
static string url_decode(const string& s) {
  string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else if (s[i] == '+') o += ' ';
    else o += s[i];
  }
  return o;
}
static map<string, string> parse_query(const string& q) {
  map<string, string> m; stringstream ss(q); string kv;
  while (getline(ss, kv, '&')) { auto e = kv.find('='); if (e != string::npos) m[url_decode(kv.substr(0, e))] = url_decode(kv.substr(e + 1)); }
  return m;
}
static bool safe_name(const string& s) {
  if (s.empty() || s[0] == '.') return false;
  return all_of(s.begin(), s.end(), [](char c) { return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'; });
}
static bool safe_box(const string& s) { return all_of(s.begin(), s.end(), [](char c) { return isdigit((unsigned char)c) || c == ':'; }); }
static string mime(const string& p) {
  auto ext = p.substr(p.rfind('.') == string::npos ? p.size() : p.rfind('.'));
  static const map<string, string> m = {{".html", "text/html"}, {".js", "text/javascript"}, {".mjs", "text/javascript"}, {".json", "application/json"},
    {".mp4", "video/mp4"}, {".png", "image/png"}, {".txt", "text/plain"}, {".key", "text/plain"}, {".md", "text/plain"}};
  auto it = m.find(ext); return it == m.end() ? "application/octet-stream" : it->second;
}
static string sh_out(const string& cmd, int* rc = nullptr) {   // run a shell command, return stdout
  string out; FILE* f = popen(cmd.c_str(), "r"); char buf[4096]; size_t n;
  if (!f) return out;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  int st = pclose(f); if (rc) *rc = st; return out;
}
static bool send_all(int fd, const char* p, size_t n) {
  while (n) { ssize_t k = send(fd, p, n, MSG_NOSIGNAL); if (k <= 0) return false; p += k; n -= k; }
  return true;
}
static void reply(int fd, int code, const string& type, const string& body, const string& extra = "") {
  const char* txt = code == 200 ? "OK" : code == 204 ? "No Content" : code == 206 ? "Partial Content" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error";
  string h = "HTTP/1.1 " + to_string(code) + " " + txt + "\r\nContent-Type: " + type + "\r\nContent-Length: " + to_string(body.size()) +
             "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n";
  send_all(fd, h.data(), h.size()); send_all(fd, body.data(), body.size());
}

// ------------------------------------------------------------------------------------------------ monitors
struct CpuSnap { double t; map<string, double> ticks; };
static mutex mon_mu;
static vector<vector<string>> smi_rows; static vector<double> smi_t;   // nvidia-smi samples with arrival time
static double win_t0 = 0; static CpuSnap win_c0;

static string proc_type(int pid) {   // Chrome's children rewrite argv into one space-separated string
  ifstream f("/proc/" + to_string(pid) + "/cmdline", ios::binary); string s((istreambuf_iterator<char>(f)), {});
  replace(s.begin(), s.end(), '\0', ' ');
  string first = s.substr(0, s.find(' '));
  if (first.find("cage") != string::npos) return "cage";
  if (first.find("chrome") == string::npos) return "other";
  auto p = s.find("--type="); if (p == string::npos) return "browser";
  return s.substr(p + 7, s.find(' ', p) - p - 7);
}
static CpuSnap cpu_snapshot() {
  CpuSnap c{now_s(), {}}; int root = root_pid; if (!root) return c;
  map<int, pair<int, double>> tab;   // pid -> (ppid, ticks)
  DIR* d = opendir("/proc"); dirent* e;
  while (d && (e = readdir(d))) {
    int pid = atoi(e->d_name); if (pid <= 0) continue;
    ifstream f("/proc/" + to_string(pid) + "/stat"); string s; getline(f, s); auto r = s.rfind(')'); if (r == string::npos) continue;
    istringstream ss(s.substr(r + 2)); string tok; vector<string> v; while (ss >> tok) v.push_back(tok);
    if (v.size() > 12) tab[pid] = {atoi(v[1].c_str()), atof(v[11].c_str()) + atof(v[12].c_str())};
  }
  if (d) closedir(d);
  set<int> in{root}; for (bool ch = true; ch;) { ch = false; for (auto& [pid, pp] : tab) if (!in.count(pid) && in.count(pp.first)) { in.insert(pid); ch = true; } }
  for (int pid : in) if (tab.count(pid)) c.ticks[proc_type(pid)] += tab[pid].second;
  return c;
}
static void smi_thread() {
  FILE* f = popen("nvidia-smi --query-gpu=power.draw,clocks.gr,clocks.mem,memory.used,utilization.gpu,utilization.memory,pstate,temperature.gpu --format=csv,noheader,nounits -lms 250", "r");
  char line[512];
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    vector<string> v; stringstream ss(line); string t;
    while (getline(ss, t, ',')) { t.erase(0, t.find_first_not_of(" ")); while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back(); v.push_back(t); }
    lock_guard<mutex> g(mon_mu); smi_rows.push_back(v); smi_t.push_back(now_s());
  }
}
static string mon_stop() {
  CpuSnap c1 = cpu_snapshot(); double t0, t1 = c1.t; CpuSnap c0; vector<vector<string>> rows;
  { lock_guard<mutex> g(mon_mu); t0 = win_t0; c0 = win_c0; for (size_t i = 0; i < smi_t.size(); i++) if (smi_t[i] >= t0 && smi_t[i] <= t1) rows.push_back(smi_rows[i]); }
  char b[256]; ostringstream o; double dt = max(1e-3, t1 - t0);
  snprintf(b, sizeof b, "{\"seconds\":%.1f,\"chromeCpuPercentOfOneCore\":{", dt); o << b;
  bool first = true;
  for (auto& [k, v] : c1.ticks) { double d = (v - (c0.ticks.count(k) ? c0.ticks[k] : 0)) / sysconf(_SC_CLK_TCK) / dt * 100; snprintf(b, sizeof b, "%s\"%s\":%.1f", first ? "" : ",", k.c_str(), d); o << b; first = false; }
  o << "},\"gpu\":{";
  auto col = [&](size_t i) { vector<double> x; for (auto& r : rows) if (r.size() > i) { char* e; double d = strtod(r[i].c_str(), &e); if (e != r[i].c_str()) x.push_back(d); } return x; };
  auto avg = [](vector<double> x) { double s = 0; for (double d : x) s += d; return x.empty() ? 0 : s / x.size(); };
  auto mx = [](vector<double> x) { return x.empty() ? 0 : *max_element(x.begin(), x.end()); };
  auto med = [](vector<double> x) { if (x.empty()) return 0.0; sort(x.begin(), x.end()); return x[x.size() / 2]; };
  set<string> ps; for (auto& r : rows) if (r.size() > 6) ps.insert(r[6]);
  string pst; for (auto& p : ps) pst += (pst.empty() ? "" : "/") + p;
  snprintf(b, sizeof b, "\"samples\":%zu,\"powerW\":{\"avg\":%.1f,\"max\":%.1f},\"clockMHz\":{\"median\":%.0f,\"max\":%.0f},\"memClockMHz\":%.0f,\"vramMiB\":{\"max\":%.0f},\"utilPct\":{\"avg\":%.1f,\"max\":%.0f},\"pstates\":\"%s\",\"tempC\":%.0f}}",
           rows.size(), avg(col(0)), mx(col(0)), med(col(1)), mx(col(1)), med(col(2)), mx(col(3)), avg(col(4)), mx(col(4)), pst.c_str(), mx(col(7)));
  o << b;
  return o.str();
}

// ------------------------------------------------------------------------------------------------ http
static void serve_file(int fd, bool head, const string& file, const string& range, const string& cors) {
  struct stat st; if (stat(file.c_str(), &st) || !S_ISREG(st.st_mode)) return reply(fd, 404, "text/plain", "not found");
  long a = 0, b = st.st_size - 1; bool part = false;
  if (!range.empty() && sscanf(range.c_str(), "bytes=%ld-%ld", &a, &b) >= 1) {
    if (range.find("bytes=-") == 0) { a = max(0L, (long)st.st_size - atol(range.c_str() + 7)); b = st.st_size - 1; }
    b = min(b, (long)st.st_size - 1); part = a <= b;
    if (!part) { a = 0; b = st.st_size - 1; }
  }
  string extra = "Accept-Ranges: bytes\r\n" + cors;
  if (part) extra += "Content-Range: bytes " + to_string(a) + "-" + to_string(b) + "/" + to_string(st.st_size) + "\r\n";
  string h = string("HTTP/1.1 ") + (part ? "206 Partial Content" : "200 OK") + "\r\nContent-Type: " + mime(file) + "\r\nContent-Length: " + to_string(b - a + 1) +
             "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n";
  if (!send_all(fd, h.data(), h.size()) || head) return;
  FILE* f = fopen(file.c_str(), "rb"); if (!f) return; fseek(f, a, SEEK_SET);
  char buf[65536]; long left = b - a + 1;
  while (left > 0) { size_t n = fread(buf, 1, min<long>(sizeof buf, left), f); if (!n || !send_all(fd, buf, n)) break; left -= n; }
  fclose(f);
}

static void handle(int fd, bool primary) {
  string req; char buf[8192]; size_t hdr_end = string::npos;
  while (hdr_end == string::npos && req.size() < 65536) {
    ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) { close(fd); return; }
    req.append(buf, n); hdr_end = req.find("\r\n\r\n");
  }
  if (hdr_end == string::npos) { close(fd); return; }
  string head = req.substr(0, hdr_end), body = req.substr(hdr_end + 4);
  istringstream hs(head); string method, target, ver; hs >> method >> target >> ver;
  string range; long clen = 0; string line; getline(hs, line);
  while (getline(hs, line)) {
    string low = line; transform(low.begin(), low.end(), low.begin(), ::tolower);
    if (low.rfind("range:", 0) == 0) { range = line.substr(6); range.erase(0, range.find_first_not_of(" ")); while (!range.empty() && isspace((unsigned char)range.back())) range.pop_back(); }
    if (low.rfind("content-length:", 0) == 0) clen = atol(line.c_str() + 15);
  }
  string path = target, query; auto q = target.find('?'); if (q != string::npos) { path = target.substr(0, q); query = target.substr(q + 1); }
  path = url_decode(path); auto Q = parse_query(query);
  if (path.find("..") != string::npos) { reply(fd, 400, "text/plain", "bad path"); close(fd); return; }
  if (method == "OPTIONS") { reply(fd, 204, "text/plain", "", "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: range, content-type\r\nAccess-Control-Allow-Methods: GET, HEAD, POST\r\n"); close(fd); return; }

  string cors;
  if (path.rfind("/cors/", 0) == 0) { path = path.substr(5); cors = "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: Content-Range, Content-Length\r\n"; }
  if (primary && method == "POST" && path == "/post") {
    string name = Q["name"]; if (!safe_name(name)) { reply(fd, 400, "text/plain", "bad name"); close(fd); return; }
    while ((long)body.size() < clen) { ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) break; body.append(buf, n); }
    ofstream(OUT + "/" + name, ios::binary | (Q["append"] == "1" ? ios::app : ios::trunc)) << body; reply(fd, 200, "text/plain", "ok", "Access-Control-Allow-Origin: *\r\n"); close(fd); return;
  }
  if (primary && path == "/root") { root_pid = atoi(Q["pid"].c_str()); reply(fd, 200, "text/plain", "ok"); close(fd); return; }
  if (primary && path == "/mon") {
    if (Q["phase"] == "start") { CpuSnap c = cpu_snapshot(); lock_guard<mutex> g(mon_mu); win_t0 = c.t; win_c0 = c; reply(fd, 200, "application/json", "{}"); }
    else reply(fd, 200, "application/json", mon_stop());
    close(fd); return;
  }
  if (primary && path == "/shot" && safe_name(Q["name"])) {
    int rc; string out = "WAYLAND_DISPLAY='" + WAYLAND + "' grim '" + OUT + "/" + Q["name"] + ".png' 2>&1"; string r = sh_out(out, &rc);
    reply(fd, rc ? 500 : 200, "text/plain", rc ? r : "ok"); close(fd); return;
  }
  if (primary && path == "/psnr" && safe_name(Q["a"]) && safe_name(Q["b"]) && safe_box(Q["box"])) {
    string f = Q["box"].empty() ? "" : "crop=" + Q["box"] + ",";
    string cmd = "ffmpeg -hide_banner -nostdin -i '" + OUT + "/" + Q["a"] + ".png' -i '" + OUT + "/" + Q["b"] + ".png' -lavfi \"[0:v]" + f + "format=rgb24[a];[1:v]" + f + "format=rgb24[b];[a][b]psnr\" -f null - 2>&1";
    string r = sh_out(cmd); auto p = r.rfind("average:"); string v = p == string::npos ? "nan" : r.substr(p + 8, r.find(' ', p + 8) - p - 8);
    reply(fd, 200, "text/plain", v); close(fd); return;
  }
  if (primary && path == "/mean" && safe_name(Q["name"]) && safe_box(Q["box"])) {
    string f = Q["box"].empty() ? "" : "crop=" + Q["box"] + ",";
    string cmd = "ffmpeg -v error -nostdin -i '" + OUT + "/" + Q["name"] + ".png' -vf \"" + f + "scale=1:1:flags=area,format=rgb24\" -f rawvideo - 2>/dev/null";
    string r = sh_out(cmd); string v = r.size() >= 3 ? to_string((unsigned char)r[0]) + "," + to_string((unsigned char)r[1]) + "," + to_string((unsigned char)r[2]) : "";
    reply(fd, 200, "text/plain", v); close(fd); return;
  }
  if (primary && path == "/key") { sh_out("WAYLAND_DISPLAY='" + WAYLAND + "' wtype x 2>&1; sleep 0.3; WAYLAND_DISPLAY='" + WAYLAND + "' wtype x 2>&1"); reply(fd, 200, "text/plain", "ok"); close(fd); return; }
  if (primary && path == "/smi") { reply(fd, 200, "text/plain", sh_out("nvidia-smi 2>&1")); close(fd); return; }

  const pair<string, string*> roots[] = {{"/src/", &WEBGPU}, {"/test/", &WEBGPU}, {"/media/", &MEDIA}};
  for (auto& [pre, dir] : roots) {
    if (path.rfind(pre, 0) != 0) continue;
    string file = pre == "/media/" ? *dir + "/" + path.substr(7) : *dir + path;   // /src and /test live under the webgpu dir
    serve_file(fd, method == "HEAD", file, range, cors); close(fd); return;
  }
  reply(fd, 404, "text/plain", "nope"); close(fd);
}

static int listen_any(int* port) {
  int s = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
  if (bind(s, (sockaddr*)&a, sizeof a) || listen(s, 64)) { perror("bind/listen"); exit(1); }
  socklen_t l = sizeof a; getsockname(s, (sockaddr*)&a, &l); *port = ntohs(a.sin_port); return s;
}

int main(int argc, char** argv) {
  signal(SIGPIPE, SIG_IGN);
  for (int i = 1; i + 1 < argc; i += 2) {
    string k = argv[i], v = argv[i + 1];
    if (k == "--webgpu") WEBGPU = v; else if (k == "--media") MEDIA = v; else if (k == "--out") OUT = v; else if (k == "--wayland") WAYLAND = v;
  }
  if (WEBGPU.empty() || MEDIA.empty() || OUT.empty()) { fprintf(stderr, "usage: server --webgpu DIR --media DIR --out DIR --wayland SOCKET\n"); return 2; }
  int pa, pb, sa = listen_any(&pa), sb = listen_any(&pb);
  printf("PORTS %d %d\n", pa, pb); fflush(stdout);
  thread(smi_thread).detach();
  thread([sb] { for (;;) { int c = accept(sb, nullptr, nullptr); if (c >= 0) thread(handle, c, false).detach(); } }).detach();
  for (;;) { int c = accept(sa, nullptr, nullptr); if (c >= 0) thread(handle, c, true).detach(); }
}

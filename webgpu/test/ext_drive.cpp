// Drives a real Chrome window through its DevTools pipe to test webgpu/extension on a live page (YouTube, ...):
// loads the unpacked extension, opens URL in a new window, presses a key (f = fullscreen on YouTube) as a trusted key event,
// reads the page state and screenshots the page, presses Escape, reads again, closes Chrome.
//   ext_drive CHROME EXT_DIR URL OUT_DIR WAIT_LOAD_S WAIT_FS_S [chrome flags...]
// Writes OUT_DIR/state-{fs,after}.json (the page's own view) and OUT_DIR/shot-fs.b64 (base64 PNG: `base64 -d`), prints the
// extension's console lines ("[nvvsr] ...") and every protocol error to stdout.
// shortcut: no JSON parser, the few fields needed are cut out with find(); localhost test tool only.
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using std::string;

struct Cdp {
  int wr, rd, next = 1;
  string buf;
  int send(const string& method, const string& params = "{}", const string& session = "") {
    int id = next++;
    string m = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\",\"params\":" + params +
               (session.empty() ? "" : ",\"sessionId\":\"" + session + "\"") + "}";
    m.push_back('\0');
    if (write(wr, m.data(), m.size()) < 0) perror("write");
    return id;
  }
  string recv(int ms) {  // one NUL-terminated message, "" on timeout or EOF
    for (;;) {
      size_t z = buf.find('\0');
      if (z != string::npos) { string m = buf.substr(0, z); buf.erase(0, z + 1); return m; }
      pollfd p{rd, POLLIN, 0};
      if (poll(&p, 1, ms) <= 0) return "";
      char tmp[65536];
      ssize_t n = read(rd, tmp, sizeof tmp);
      if (n <= 0) return "";
      buf.append(tmp, n);
    }
  }
  static void note(const string& m) {
    if (m.find("[nvvsr]") != string::npos || m.find("\"error\":{") != string::npos) printf("%s\n", m.substr(0, 600).c_str());
  }
  string call(const string& method, const string& params = "{}", const string& session = "") {
    string want = "{\"id\":" + std::to_string(send(method, params, session)) + ",";
    for (;;) {
      string m = recv(30000);
      if (m.empty()) return "";
      if (m.compare(0, want.size(), want) == 0) { note(m); return m; }
      note(m);
    }
  }
  void pump(int seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (;;) {
      int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
      if (left <= 0) return;
      string m = recv(left);
      if (!m.empty()) note(m);
    }
  }
};

static string field(const string& m, const string& key) {  // "key":"value" -> value (no unescaping)
  size_t a = m.find("\"" + key + "\":\"");
  if (a == string::npos) return "";
  a += key.size() + 4;
  return m.substr(a, m.find('"', a) - a);
}

static const char* kState =
    "(() => { const v = [...document.querySelectorAll('video')].sort((a, b) => b.clientWidth * b.clientHeight - a.clientWidth * a.clientHeight)[0];"
    " const c = v && v.nextElementSibling; return JSON.stringify({ fs: !!document.fullscreenElement, title: document.title,"
    " video: v && [v.videoWidth, v.videoHeight, v.clientWidth, v.clientHeight, v.paused, v.currentTime],"
    " canvas: c && c.tagName === 'CANVAS' ? [c.width, c.height, c.style.cssText] : null }); })()";

static void save(const string& path, const string& text) { std::ofstream(path) << text << "\n"; }

static string evaluate(Cdp& c, const string& session) {  // the page's JSON string, unescaped just enough for jq
  string r = c.call("Runtime.evaluate", string("{\"expression\":\"") + [] { string e; for (char ch : string(kState)) { if (ch == '"' || ch == '\\') e += '\\'; e += ch; } return e; }() + "\",\"returnByValue\":true}", session);
  size_t a = r.find("\"value\":\"");
  if (a == string::npos) return r;
  string out;
  for (size_t i = a + 9; i < r.size() && r[i] != '"'; ++i) { if (r[i] == '\\' && i + 1 < r.size()) ++i; out += r[i]; }
  return out;
}

static void key(Cdp& c, const string& session, const string& k, const string& code, int vk, const string& text) {
  string base = "\"key\":\"" + k + "\",\"code\":\"" + code + "\",\"windowsVirtualKeyCode\":" + std::to_string(vk);
  c.call("Input.dispatchKeyEvent", "{\"type\":\"keyDown\"," + base + (text.empty() ? "" : ",\"text\":\"" + text + "\"") + "}", session);
  c.call("Input.dispatchKeyEvent", "{\"type\":\"keyUp\"," + base + "}", session);
}

int main(int argc, char** argv) {
  if (argc < 7) { fprintf(stderr, "usage: ext_drive CHROME EXT_DIR URL OUT_DIR WAIT_LOAD_S WAIT_FS_S [chrome flags...]\n"); return 2; }
  const string chrome = argv[1], ext = argv[2], url = argv[3], out = argv[4];
  const int wait_load = atoi(argv[5]), wait_fs = atoi(argv[6]);
  int to[2], from[2];
  if (pipe(to) || pipe(from)) return 1;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(to[0], 3); dup2(from[1], 4);  // Chrome reads commands on fd 3 and writes replies on fd 4
    std::vector<string> a = {chrome, "--remote-debugging-pipe", "--enable-unsafe-extension-debugging", "--no-first-run", "--no-default-browser-check"};
    for (int i = 7; i < argc; ++i) a.push_back(argv[i]);
    a.push_back("about:blank");
    std::vector<char*> v;
    for (auto& s : a) v.push_back(s.data());
    v.push_back(nullptr);
    execv(chrome.c_str(), v.data());
    _exit(127);
  }
  close(to[0]); close(from[1]);
  Cdp c{to[1], from[0]};

  printf("loadUnpacked: %s\n", c.call("Extensions.loadUnpacked", "{\"path\":\"" + ext + "\"}").substr(0, 300).c_str());
  // the one window Chrome opened (about:blank), navigated only now so the extension's content script is injected into the page
  string targets = c.call("Target.getTargets");
  size_t page = targets.find("\"type\":\"page\"");
  size_t id = page == string::npos ? string::npos : targets.rfind("\"targetId\":\"", page);
  string target = id == string::npos ? "" : field(targets.substr(id), "targetId");
  string session = field(c.call("Target.attachToTarget", "{\"targetId\":\"" + target + "\",\"flatten\":true}"), "sessionId");
  c.call("Runtime.enable", "{}", session);
  c.call("Page.navigate", "{\"url\":\"" + url + "\"}", session);
  c.pump(wait_load);
  printf("before: %s\n", evaluate(c, session).c_str());
  if (const char* probe = getenv("EXT_DRIVE_EVAL")) {  // optional async expression run in the page itself, result printed raw
    string e;
    for (char ch : string(probe)) { if (ch == '"' || ch == '\\') e += '\\'; e += ch; }
    printf("probe: %s\n", c.call("Runtime.evaluate", "{\"expression\":\"" + e + "\",\"awaitPromise\":true,\"returnByValue\":true}", session).substr(0, 800).c_str());
  }
  key(c, session, "f", "KeyF", 70, "f");
  c.pump(wait_fs);
  string st = evaluate(c, session);
  save(out + "/state-fs.json", st);
  printf("fullscreen: %s\n", st.c_str());
  // pause so both pictures show the same frame, then once with the extension's canvas and once with it hidden
  c.call("Runtime.evaluate", "{\"expression\":\"document.querySelector('video').pause()\"}", session);
  c.pump(1);
  save(out + "/shot-fs.b64", field(c.call("Page.captureScreenshot", "{\"format\":\"png\"}", session), "data"));
  c.call("Runtime.evaluate", "{\"expression\":\"{ const c = document.querySelector('video').nextElementSibling; if (c && c.tagName === 'CANVAS') c.style.visibility = 'hidden'; }\"}", session);
  c.pump(1);
  save(out + "/shot-fs-nocanvas.b64", field(c.call("Page.captureScreenshot", "{\"format\":\"png\"}", session), "data"));
  key(c, session, "Escape", "Escape", 27, "");
  c.pump(3);
  st = evaluate(c, session);
  save(out + "/state-after.json", st);
  printf("after: %s\n", st.c_str());
  if (const char* bc = getenv("EXT_DRIVE_BROWSER_CALL")) printf("browser call: %s\n", c.call(bc).substr(0, 200000).c_str());  // e.g. SystemInfo.getInfo
  c.call("Browser.close");
  waitpid(pid, nullptr, 0);
  return 0;
}

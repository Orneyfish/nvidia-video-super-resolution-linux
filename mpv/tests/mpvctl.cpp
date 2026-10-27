// mpvctl: one-shot client for mpv's JSON IPC socket (replaces mpvipc.py for the shell tests).
// Usage: mpvctl SOCKET get PROP | set PROP JSON_VALUE | cmd JSON_ARRAY
// Prints the reply's data (raw JSON) on stdout. Errors and timeouts go to stderr with exit 1.
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const size_t npos = std::string::npos;
const int kRequestId = 1;
const int kTimeoutMs = 3000;

[[noreturn]] void die(const std::string& msg) {
    fprintf(stderr, "error: %s\n", msg.c_str());
    exit(1);
}

[[noreturn]] void usage() {
    fputs("usage: mpvctl SOCKET get PROP | set PROP JSON_VALUE | cmd JSON_ARRAY\n", stderr);
    exit(2);
}

size_t skip_ws(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return i;
}

// End (exclusive) of the JSON value starting at i (after blanks), or npos if malformed.
size_t value_end(const std::string& s, size_t i) {
    i = skip_ws(s, i);
    if (i >= s.size()) return npos;
    if (s[i] == '"') {
        for (++i; i < s.size(); ++i) {
            if (s[i] == '\\') ++i;
            else if (s[i] == '"') return i + 1;
        }
        return npos;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            if (s[i] == '"') {
                i = value_end(s, i);
                if (i == npos) return npos;
                continue;
            }
            if (s[i] == '{' || s[i] == '[') ++depth;
            else if ((s[i] == '}' || s[i] == ']') && --depth == 0) return i + 1;
            ++i;
        }
        return npos;
    }
    size_t start = i;  // number, true, false, null
    while (i < s.size() && std::strchr(",}] \t\r\n", s[i]) == nullptr) ++i;
    return i > start ? i : npos;
}

// Finds the raw text of the top-level member `key` of the JSON object `obj`.
bool find_member(const std::string& obj, const std::string& key, size_t& begin, size_t& end) {
    size_t i = skip_ws(obj, 0);
    if (i >= obj.size() || obj[i] != '{') return false;
    for (++i;;) {
        i = skip_ws(obj, i);
        if (i < obj.size() && obj[i] == ',') {
            ++i;
            continue;
        }
        if (i >= obj.size() || obj[i] == '}') return false;
        size_t kend = value_end(obj, i);
        if (kend == npos || obj[i] != '"') return false;
        std::string name = obj.substr(i + 1, kend - i - 2);  // mpv keys contain no escapes
        i = skip_ws(obj, kend);
        if (i >= obj.size() || obj[i] != ':') return false;
        i = skip_ws(obj, i + 1);
        size_t vend = value_end(obj, i);
        if (vend == npos) return false;
        if (name == key) {
            begin = i;
            end = vend;
            return true;
        }
        i = vend;
    }
}

// Handles one line from mpv. Returns -1 if it is not the reply to our request, else the exit code.
int handle_line(const std::string& line) {
    size_t b = 0, e = 0;
    if (!find_member(line, "request_id", b, e)) return -1;  // events carry no request_id
    if (std::strtol(line.substr(b, e - b).c_str(), nullptr, 10) != kRequestId) return -1;
    if (!find_member(line, "error", b, e)) die("malformed reply, no error field");
    std::string err = line.substr(b, e - b);
    if (err == "\"success\"") {
        if (find_member(line, "data", b, e)) printf("%s\n", line.substr(b, e - b).c_str());
        return 0;
    }
    fprintf(stderr, "error: %s\n", err.size() >= 2 ? err.substr(1, err.size() - 2).c_str() : err.c_str());
    return 1;
}

// Connects by changing into the socket's directory, so paths longer than sun_path still work.
int connect_socket(const std::string& path) {
    size_t slash = path.rfind('/');
    std::string dir = slash == npos ? "." : path.substr(0, slash + 1);
    std::string base = slash == npos ? path : path.substr(slash + 1);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (base.size() >= sizeof addr.sun_path) die("socket name too long: " + base);
    std::strcpy(addr.sun_path, base.c_str());
    int here = open(".", O_RDONLY | O_DIRECTORY);
    if (here < 0 || chdir(dir.c_str()) != 0) die("cannot enter " + dir + ": " + strerror(errno));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int rc = fd < 0 ? -1 : connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    int saved = errno;
    fchdir(here);
    close(here);
    if (rc != 0) die("cannot connect to " + path + ": " + strerror(saved));
    return fd;
}

void send_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die(std::string("send failed: ") + strerror(errno));
        off += static_cast<size_t>(n);
    }
}

int await_reply(int fd) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    std::string buf;
    char chunk[4096];
    for (;;) {
        size_t nl;
        while ((nl = buf.find('\n')) != npos) {
            int rc = handle_line(buf.substr(0, nl));
            if (rc >= 0) return rc;
            buf.erase(0, nl + 1);
        }
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) die("no reply from mpv within 3 s");
        pollfd p{fd, POLLIN, 0};
        int pr = poll(&p, 1, static_cast<int>(left.count()));
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) die(std::string("poll failed: ") + strerror(errno));
        if (pr == 0) continue;
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) die("mpv closed the socket before replying");
        buf.append(chunk, static_cast<size_t>(n));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) usage();
    std::string verb = argv[2];
    std::string command;
    if (verb == "get" && argc == 4) {
        command = "[\"get_property\",\"" + std::string(argv[3]) + "\"]";
    } else if (verb == "set" && argc == 5) {
        command = "[\"set_property\",\"" + std::string(argv[3]) + "\"," + argv[4] + "]";
    } else if (verb == "cmd" && argc == 4) {
        command = argv[3];
    } else {
        usage();
    }
    std::string request = "{\"command\":" + command + ",\"request_id\":" + std::to_string(kRequestId) + "}\n";
    int fd = connect_socket(argv[1]);
    send_all(fd, request);
    int rc = await_reply(fd);
    close(fd);
    return rc;
}

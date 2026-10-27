// Compares the frames VLC wrote through the nvvsr module (the `yuv` video output: a header line, then
// "FRAME\n" + planar frame per picture) with a reference made from the same source frames.
//
//   compare OUT.yuv REF.i420 BIC.i420 WxH [--frames N] [--expect-exact]
//   compare --count-frames CAPTURE.yuv        prints the number of FRAME records (or `bad` at the first other record)
//   compare --best-psnr FRAME.i420 REF.i420 BIC.i420 WxH
//                                              prints "PSNR_REF PSNR_BIC": the full-frame PSNR of FRAME against the REF
//                                              frame with the best luma PSNR, and against the BIC frame at that index
//
// REF.i420 is the libnvvsr result for the first source frames (nvvsr_vlc_ref), BIC.i420 a bicubic upscale of the same
// frames (or `-` to compare with REF only). For every checked output frame the matching source frame is found by the
// best luma PSNR against REF. Prints one line, e.g. `frames=30 header='...' size=2560x1440 exact=30/30 ...`, and exits
// 0 when the frames match REF (exactly with --expect-exact, otherwise PSNR >= 45 dB) and are closer to REF than a
// different frame would be. Exit status: 0 match, 1 mismatch or failure, 2 usage error.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
const double kInf = std::numeric_limits<double>::infinity();

const char* kDoc =
    "compare OUT.yuv REF.i420 BIC.i420 WxH [--frames N] [--expect-exact]\n"
    "\n"
    "REF.i420 is the libnvvsr result for the first source frames, BIC.i420 a bicubic upscale of the same frames\n"
    "(or `-` to compare with REF only). The matching source frame of each output frame is the one with the best\n"
    "luma PSNR against REF. Exits 0 when the frames match REF (exactly with --expect-exact, otherwise PSNR >= 45 dB).\n";

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "compare: %s\n", msg.c_str());
    std::exit(1);
}

[[noreturn]] void usage(const std::string& msg) {
    std::fprintf(stderr, "usage: compare OUT.yuv REF.i420 BIC.i420 WxH [--frames N] [--expect-exact]\n"
                         "compare: error: %s\n", msg.c_str());
    std::exit(2);
}

bool parse_long(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 10);
    return *end == '\0';
}

// Python's readline(): one line including its '\n', empty at end of file.
std::string readline(FILE* f) {
    std::string s;
    int c;
    while ((c = std::getc(f)) != EOF) {
        s.push_back(char(c));
        if (c == '\n') break;
    }
    return s;
}

// The header as the Python port printed it: ASCII-decoded with 'replace', stripped, then repr()'d.
std::string header_repr(const std::string& raw) {
    std::string s;
    for (unsigned char c : raw) {
        if (c >= 0x80) s += "\xEF\xBF\xBD";  // U+FFFD, what the 'replace' handler gives for each bad byte
        else s.push_back(char(c));
    }
    const char* ws = " \t\n\v\f\r\x1c\x1d\x1e\x1f";
    size_t b = s.find_first_not_of(ws);
    s = b == std::string::npos ? "" : s.substr(b, s.find_last_not_of(ws) - b + 1);
    char q = (s.find('\'') != std::string::npos && s.find('"') == std::string::npos) ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
        if (c == '\\' || c == q) { out += '\\'; out.push_back(char(c)); }
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20 || c == 0x7f) { char buf[8]; std::snprintf(buf, sizeof buf, "\\x%02x", c); out += buf; }
        else out.push_back(char(c));
    }
    return out + q;
}

struct Stream {
    std::string header;
    std::vector<Bytes> frames;
};

// The first `limit` FRAME records of a VLC yuv capture.
Stream read_stream(const std::string& path, size_t fsize, long limit) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) die("cannot open " + path);
    Stream s;
    s.header = readline(f);
    while (long(s.frames.size()) < limit) {
        std::string tag = readline(f);
        if (tag.empty()) break;
        if (tag != "FRAME\n") die("bad stream: expected FRAME, got '" + tag.substr(0, 16) + "'");
        Bytes buf(fsize);
        if (std::fread(buf.data(), 1, fsize, f) != fsize) break;
        s.frames.push_back(std::move(buf));
    }
    std::fclose(f);
    return s;
}

// Frames of a raw I420 file, read on demand (the Python port used a memory map).
struct RawFile {
    FILE* f = nullptr;
    size_t fsize = 0;
    size_t n = 0;

    RawFile(const std::string& path, size_t fs) : fsize(fs) {
        f = std::fopen(path.c_str(), "rb");
        if (!f) die("cannot open " + path);
        n = size_t(std::filesystem::file_size(path)) / fsize;
    }
    ~RawFile() { std::fclose(f); }
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    void read(size_t i, uint8_t* buf) {
        if (std::fseek(f, long(i * fsize), SEEK_SET) != 0 || std::fread(buf, 1, fsize, f) != fsize)
            die("short read of frame " + std::to_string(i));
    }
};

double psnr_from(double sum_sq, double count) {
    double mse = sum_sq / count;
    return mse == 0 ? kInf : 10.0 * std::log10(255.0 * 255.0 / mse);
}

double psnr_full(const uint8_t* a, const uint8_t* b, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = int(a[i]) - int(b[i]);
        s += uint64_t(d * d);
    }
    return psnr_from(double(s), double(n));
}

// Luma only, every 2nd row: the search for the matching source frame.
double psnr_luma_half(const uint8_t* a, const uint8_t* b, int w, int h) {
    uint64_t s = 0;
    size_t cnt = 0;
    for (int r = 0; r < h; r += 2)
        for (int c = 0; c < w; ++c) {
            size_t i = size_t(r) * w + c;
            int d = int(a[i]) - int(b[i]);
            s += uint64_t(d * d);
            ++cnt;
        }
    return psnr_from(double(s), double(cnt));
}

// Mean SSIM of the luma plane on 8x8 blocks (a cheap version, enough to compare two upscales).
double ssim_luma(const uint8_t* a, const uint8_t* b, int w, int h) {
    const double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    int h8 = h / 8 * 8, w8 = w / 8 * 8;
    double total = 0;
    size_t blocks = 0;
    for (int by = 0; by < h8; by += 8)
        for (int bx = 0; bx < w8; bx += 8) {
            double xa[64], xb[64];
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) {
                    size_t i = size_t(by + r) * w + bx + c;
                    xa[r * 8 + c] = a[i];
                    xb[r * 8 + c] = b[i];
                }
            double ma = 0, mb = 0;
            for (int k = 0; k < 64; ++k) { ma += xa[k]; mb += xb[k]; }
            ma /= 64;
            mb /= 64;
            double va = 0, vb = 0, cov = 0;
            for (int k = 0; k < 64; ++k) {
                double da = xa[k] - ma, db = xb[k] - mb;
                va += da * da;
                vb += db * db;
                cov += da * db;
            }
            va /= 64;
            vb /= 64;
            cov /= 64;
            total += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++blocks;
        }
    return total / double(blocks);
}

// --count-frames: the same walk as read_stream without keeping the pictures; a truncated last picture still counts.
int count_frames(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) die("cannot open " + path);
    std::istringstream hdr(readline(f));
    std::vector<std::string> tok;
    for (std::string t; hdr >> t;) tok.push_back(t);
    long w = 0, h = 0;
    if (tok.size() < 3 || !parse_long(tok[1].substr(1), w) || !parse_long(tok[2].substr(1), h)) die("bad header");
    long fs = w * h * 3 / 2;
    long n = 0;
    for (;;) {
        std::string tag = readline(f);
        if (tag.empty()) break;
        if (tag != "FRAME\n") {
            std::printf("bad\n");
            return 0;
        }
        if (std::fseek(f, fs, SEEK_CUR) != 0) break;
        ++n;
    }
    std::fclose(f);
    std::printf("%ld\n", n);
    return 0;
}

// PSNR as the check of section 3 always printed it: identical frames are 99 dB.
double psnr_capped(const uint8_t* a, const uint8_t* b, size_t n) {
    double p = psnr_full(a, b, n);
    return std::isinf(p) ? 99.0 : p;
}

// --best-psnr: the matching reference frame is the one with the best luma PSNR (first one on ties).
int best_psnr(int argc, char** argv) {
    int w = 0, h = 0;
    if (argc != 6 || std::sscanf(argv[5], "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0)
        usage("--best-psnr FRAME.i420 REF.i420 BIC.i420 WxH");
    size_t fsize = size_t(w) * size_t(h) * 3 / 2, luma = size_t(w) * size_t(h);
    Bytes fr(fsize);
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) die(std::string("cannot open ") + argv[2]);
    size_t got = std::fread(fr.data(), 1, fsize, f);
    std::fclose(f);
    if (got != fsize) die(std::string("short frame in ") + argv[2]);
    RawFile ref(argv[3], fsize), bic(argv[4], fsize);
    Bytes buf(fsize);
    double best = -kInf;
    size_t best_i = 0;
    for (size_t i = 0; i < ref.n; ++i) {
        ref.read(i, buf.data());
        double p = psnr_capped(fr.data(), buf.data(), luma);
        if (p > best) {
            best = p;
            best_i = i;
        }
    }
    ref.read(best_i, buf.data());
    double p_ref = psnr_capped(fr.data(), buf.data(), fsize);
    bic.read(best_i, buf.data());
    double p_bic = psnr_capped(fr.data(), buf.data(), fsize);
    std::printf("%.1f %.1f\n", p_ref, p_bic);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--count-frames") {
        if (argc != 3) usage("--count-frames CAPTURE.yuv");
        return count_frames(argv[2]);
    }
    if (argc >= 2 && std::string(argv[1]) == "--best-psnr") return best_psnr(argc, argv);
    std::vector<std::string> pos;
    long frames = 30;
    bool expect_exact = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::fputs(kDoc, stdout);
            return 0;
        } else if (a == "--expect-exact") {
            expect_exact = true;
        } else if (a == "--frames" || a.rfind("--frames=", 0) == 0) {
            std::string v;
            if (a == "--frames") {
                if (++i >= argc) usage("argument --frames: expected one argument");
                v = argv[i];
            } else {
                v = a.substr(9);
            }
            if (!parse_long(v, frames)) usage("argument --frames: invalid int value: '" + v + "'");
        } else if (a.size() > 1 && a[0] == '-') {
            usage("unrecognized arguments: " + a);
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() < 4) usage("the following arguments are required: OUT, REF, BIC, WxH");
    if (pos.size() > 4) usage("unrecognized arguments: " + pos[4]);

    const std::string &out_path = pos[0], &ref_path = pos[1], &bic_path = pos[2], &size = pos[3];
    long wl = 0, hl = 0;
    size_t x = size.find('x');
    if (x == std::string::npos || size.find('x', x + 1) != std::string::npos ||
        !parse_long(size.substr(0, x), wl) || !parse_long(size.substr(x + 1), hl) || wl <= 0 || hl <= 0)
        die("bad size '" + size + "', expected WxH");
    int w = int(wl), h = int(hl);
    size_t fsize = size_t(w) * size_t(h) * 3 / 2;

    Stream st = read_stream(out_path, fsize, frames);
    if (st.frames.empty()) {
        std::printf("frames=0 header=%s\n", header_repr(st.header).c_str());
        return 1;
    }
    RawFile ref(ref_path, fsize);
    std::unique_ptr<RawFile> own_bic;
    if (bic_path != "-") own_bic = std::make_unique<RawFile>(bic_path, fsize);
    RawFile& bic = own_bic ? *own_bic : ref;
    size_t n = std::min(ref.n, bic.n);
    if (n == 0) die("no reference frames in " + ref_path);

    size_t exact = 0;
    double worst_ref = kInf, sum_bic = 0, sum_ssim_bic = 0, sum_ssim_ref = 0;
    int bad = 0;
    Bytes rbuf(fsize), bbuf(fsize);
    for (const Bytes& fr : st.frames) {
        double best = -1.0;
        size_t best_i = 0;
        const uint8_t* ya = fr.data();
        for (size_t i = 0; i < n; ++i) {
            ref.read(i, rbuf.data());
            double p = psnr_luma_half(ya, rbuf.data(), w, h);
            if (p > best) {
                best = p;
                best_i = i;
            }
        }
        ref.read(best_i, rbuf.data());
        bic.read(best_i, bbuf.data());
        double full = psnr_full(fr.data(), rbuf.data(), fsize);
        if (full == kInf) ++exact;
        worst_ref = std::min(worst_ref, full);
        sum_bic += psnr_full(fr.data(), bbuf.data(), fsize);
        sum_ssim_bic += ssim_luma(fr.data(), bbuf.data(), w, h);
        sum_ssim_ref += ssim_luma(fr.data(), rbuf.data(), w, h);
        if ((expect_exact && full != kInf) || full < 45.0) ++bad;
    }
    double count = double(st.frames.size());
    char worst_s[32];
    if (worst_ref == kInf) std::snprintf(worst_s, sizeof worst_s, "inf");
    else std::snprintf(worst_s, sizeof worst_s, "%.2f", worst_ref);
    std::printf("frames=%zu header=%s size=%dx%d exact=%zu/%zu psnr_ref_min=%s psnr_bicubic_mean=%.2f "
                "ssim_vs_bicubic=%.4f ssim_vs_ref=%.4f\n",
                st.frames.size(), header_repr(st.header).c_str(), w, h, exact, st.frames.size(), worst_s,
                sum_bic / count, sum_ssim_bic / count, sum_ssim_ref / count);
    return bad == 0 ? 0 : 1;
}

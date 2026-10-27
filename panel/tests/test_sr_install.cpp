// Port of panel/tests/test_sr.py: InstallJson (all four cases) and test_vsscript_entry_and_libdir, plus checks for the
// vapoursynth.toml scan and the install.json read errors.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "sr_install.h"
#include "support_home.h"

namespace fs = std::filesystem;

namespace {

// A scratch HOME with the XDG overrides unset, removed on destruction.
struct Scratch {
  std::string root = (fs::temp_directory_path() / "nvvsr-sr-install-XXXXXX").string();
  std::string home;

  Scratch() {
    if (!mkdtemp(root.data())) std::abort();
    home = root + "/home";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    for (const char *v : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME"}) unsetenv(v);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
};

}  // namespace

int main() {
  Scratch s;
  const std::string json_path = s.root + "/install.json";

  // test_absent
  {
    const InstallInfo i = read_install_json();
    CHECK(!i.present, "absent install.json is not present");
    CHECK(i.paths.empty() && i.components.empty() && !i.version, "absent install.json has no content");
    CHECK(!i.error, "absent install.json has no error");
  }

  // test_odd_shapes_do_not_raise
  {
    const char *payloads[] = {"not json", "[1,2]",
                              R"({"paths": 5, "components": {"a": "x", "b": {"status": 3}}, "version": 7})",
                              R"({"paths": {"libnvvsr": 5}})"};
    for (const char *p : payloads) {
      support::write_file(json_path, p);
      CHECK(read_install_json(json_path).present, "odd install.json is present: %s", p);
    }
    support::write_file(json_path, "not json");
    CHECK(read_install_json(json_path).error.has_value(), "invalid JSON sets an error");
    support::write_file(json_path, "");
    CHECK_EQ(read_install_json(json_path).error.value_or(""), std::string("unreadable (invalid JSON)"));
    support::write_file(json_path, "[1,2]");
    CHECK_EQ(read_install_json(json_path).error.value_or(""), std::string("not a JSON object"));

    support::write_file(json_path, payloads[2]);
    const InstallInfo odd = read_install_json(json_path);
    CHECK(odd.paths.empty() && !odd.version && !odd.error, "non-object paths and non-string version are ignored");
    CHECK_EQ(odd.components.size(), size_t{1});  // "a" is not an object, so it is dropped
    CHECK(odd.components.count("b") && odd.components.at("b").first == "3" && odd.components.at("b").second.empty(),
          "a non-string status is kept as text, a missing detail is empty");

    support::write_file(json_path, payloads[3]);
    CHECK(read_install_json(json_path).paths.empty(), "non-string path entries are dropped");

    fs::remove(json_path);
    fs::create_directories(json_path);  // a directory is present but unreadable
    const InstallInfo dir = read_install_json(json_path);
    CHECK(dir.present && dir.error.has_value(), "a directory at the install.json path is an unreadable error");
    fs::remove_all(json_path);
  }

  // test_schema_one_with_unknown_keys
  {
    const std::string repo = s.root + "/repo";
    support::make_home(s.home, repo);
    const InstallInfo i = read_install_json();
    CHECK(i.present && !i.error, "schema 1 install.json is read");
    CHECK_EQ(i.version.value_or(""), std::string("0.1.0"));
    CHECK(i.components.count("chrome_handoff") &&
              i.components.at("chrome_handoff") == std::make_pair(std::string("skipped"), std::string("not requested")),
          "component status and detail are read, unknown keys ignored");
    CHECK(i.components.count("core") && i.components.at("core").first == "installed", "core component is installed");
    CHECK(i.paths.count("libnvvsr") && i.paths.at("libnvvsr") == repo + "/build/core/libnvvsr.so",
          "recorded library path is read");
  }

  // test_vsscript_entry_and_libdir
  {
    const support::Home h = support::make_home(s.home);
    const VsscriptEntry v = vsscript_entry();
    CHECK(v.present, "vapoursynth.toml has a libvsscript.so entry");
    CHECK_EQ(v.detail, std::string("/usr/lib/libvsscript.so"));

    const auto dirs = read_libdir();
    CHECK(dirs && *dirs == std::vector<std::string>({h.vfx, h.wheel}), "libdir file lists the two directories");
    CHECK_EQ(libdir_env().value_or("none"), h.vfx + ":" + h.wheel);

    support::write_file(s.home + "/.config/nvvsr/libdir", "# comment\n~/x:~/y\n");
    const auto expanded = read_libdir();
    CHECK(expanded && *expanded == std::vector<std::string>({s.home + "/x", s.home + "/y"}),
          "comment lines are skipped and ~ is expanded");

    fs::remove(s.home + "/.config/nvvsr/libdir");
    CHECK(!libdir_env() && !read_libdir(), "no libdir file means no directories");
  }

  // libdir edge cases: only colons gives an empty list (and no env value); only comments gives no list
  {
    const std::string libdir = s.home + "/.config/nvvsr/libdir";
    support::write_file(libdir, "::\n");
    CHECK(read_libdir() && read_libdir()->empty(), "a line of colons gives an empty list");
    CHECK(!libdir_env(), "an empty list gives no NVVSR_LIBDIR");
    support::write_file(libdir, "# only a comment\n\n   \n");
    CHECK(!read_libdir(), "a file with only comments and blank lines gives no list");
    fs::remove(libdir);
  }

  // vapoursynth.toml scan: top-level keys only, quoted keys may hold spaces, the key must end in libvsscript.so
  {
    const std::string toml = s.home + "/.config/vapoursynth/vapoursynth.toml";
    support::write_file(toml, "\"/opt/My Libs/libvsscript.so\" = [\"a\"]\n");
    CHECK_EQ(vsscript_entry().detail, std::string("/opt/My Libs/libvsscript.so"));
    support::write_file(toml, "[tool]\n\"/x/libvsscript.so\" = [\"a\"]\n");
    CHECK(!vsscript_entry().present, "keys under a [table] header are not top-level");
    support::write_file(toml, "# \"/x/libvsscript.so\" = 1\n\"/x/libvsscript.so.bak\" = [\"a\"]\n");
    CHECK_EQ(vsscript_entry().detail, std::string("no libvsscript.so entry"));
    support::write_file(toml, "'/x/libvsscript.so' = [\"a\"]\n");
    CHECK(vsscript_entry().present, "single-quoted keys count");
    fs::remove(toml);
    CHECK_EQ(vsscript_entry().detail, std::string("no vapoursynth.toml"));
  }

  return finish("test_sr_install");
}

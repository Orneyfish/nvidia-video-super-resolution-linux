#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "check.h"
#include "settings.h"
#include "support_home.h"

namespace fs = std::filesystem;

namespace {

std::string slurp(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int loaded(const std::string &json, const std::string &path) {
  support::write_file(path, json);
  Settings s;
  s.poll_interval = 99;  // load() must reset it, not keep a stale value
  s.load(path);
  return s.poll_interval;
}

}  // namespace

int main() {
  std::string tmpl = (fs::temp_directory_path() / "nvc-settings-XXXXXX").string();
  const std::string dir = mkdtemp(tmpl.data());
  const std::string f = dir + "/panel.json";

  Settings d;
  CHECK_EQ(d.poll_interval, 2);
  d.load(dir + "/missing.json");
  CHECK_EQ(d.poll_interval, 2);

  CHECK_EQ(loaded("{\"poll_interval\": 5}", f), 5);
  CHECK_EQ(loaded("{\"poll_interval\": 0}", f), 0);  // manual is a valid choice
  CHECK_EQ(loaded("{\"poll_interval\": 3}", f), 2);  // not one of the choices
  CHECK_EQ(loaded("{\"poll_interval\": \"5\"}", f), 2);
  CHECK_EQ(loaded("{\"poll_interval\": true}", f), 2);
  CHECK_EQ(loaded("{\"other\": 5}", f), 2);
  CHECK_EQ(loaded("[5]", f), 2);
  CHECK_EQ(loaded("not json", f), 2);

  Settings s;
  s.poll_interval = 10;
  CHECK(s.save(dir + "/new/sub/panel.json"), "creates missing parent directories");
  CHECK_EQ(slurp(dir + "/new/sub/panel.json"), std::string("{\n  \"poll_interval\": 10\n}\n"));
  Settings back;
  back.load(dir + "/new/sub/panel.json");
  CHECK_EQ(back.poll_interval, 10);

  support::write_file(dir + "/real/target.json", "{}");
  fs::create_symlink(dir + "/real/target.json", dir + "/link.json");
  s.poll_interval = 1;
  CHECK(s.save(dir + "/link.json"), "save through a symlink");
  CHECK(fs::is_symlink(dir + "/link.json"), "the symlink survives");
  CHECK_EQ(slurp(dir + "/real/target.json"), std::string("{\n  \"poll_interval\": 1\n}\n"));

  CHECK(!s.save(dir + "/real/target.json/x/panel.json"), "a file in the way is reported, not thrown");

  std::error_code ec;
  fs::remove_all(dir, ec);
  return finish("test_settings");
}

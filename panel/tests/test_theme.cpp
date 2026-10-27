// Port of the Theme tests in panel/tests/test_misc.py (4 methods, 7 assertions) plus cases for the C++ validation.
#include <cstdlib>
#include <filesystem>
#include <string>

#include "check.h"
#include "support_home.h"
#include "theme.h"

namespace fs = std::filesystem;

namespace {

// The caelestia scheme from panel/tests/support.py SCHEME.
const char *kNested =
    R"({"name": "test", "flavour": "default", "mode": "dark", "variant": "tonalspot", "colours": {)"
    R"("surface": "141218", "surfaceContainer": "211f26", "surfaceContainerHigh": "2b2930", )"
    R"("surfaceContainerHighest": "36343b", "surfaceContainerLow": "1d1b20", "onSurface": "e6e0e9", )"
    R"("onSurfaceVariant": "cac4d0", "primary": "7fd9c3", "onPrimary": "00382d", "primaryContainer": "005143", )"
    R"("onPrimaryContainer": "9cf5df", "secondaryContainer": "334b44", "onSecondaryContainer": "cde8df", )"
    R"("tertiary": "a8c8ff", "outline": "938f99", "outlineVariant": "49454f", "error": "f2b8b5", )"
    R"("onError": "601410", "errorContainer": "8c1d18", "onErrorContainer": "f9dedc", "success": "b5ccba", )"
    R"("onSuccess": "213528"}})";

// Writes text to a fresh temporary directory and returns the file path.
std::string scratch_file(const std::string &dir, const std::string &text) {
  return support::write_file(dir + "/scheme.json", text);
}

}  // namespace

int main() {
  std::string tmpl = (fs::temp_directory_path() / "nvc-theme-XXXXXX").string();
  const std::string dir = mkdtemp(tmpl.data());

  // test_caelestia_nested_scheme
  {
    Scheme s = load_scheme(scratch_file(dir, kNested));
    CHECK_EQ(s.colours["primary"], std::string("#7fd9c3"));
    CHECK_EQ(s.mode, std::string("dark"));
    CHECK(s.colours.count("surfaceContainer"), "nested scheme keeps surfaceContainer");
  }

  // test_flat_scheme_and_light_mode (values without '#' get one)
  {
    Scheme s = load_scheme(scratch_file(dir, R"({"mode": "light", "surface": "ffffff", "primary": "#123456"})"));
    CHECK_EQ(s.colours["primary"], std::string("#123456"));
    CHECK_EQ(s.colours["surface"], std::string("#ffffff"));
    CHECK_EQ(s.mode, std::string("light"));
  }

  // test_missing_or_broken_falls_back: each of the four paths gives the static palette in dark mode
  const std::string broken[] = {
      dir + "/nonexistent/scheme.json",
      scratch_file(dir, "{not json"),
      scratch_file(dir, R"({"colours": {}})"),
      scratch_file(dir, "[]"),
  };
  for (const std::string &p : broken) {
    Scheme s = load_scheme(p);
    CHECK(s.colours == fallback_palette() && s.mode == "dark", "fallback for %s", p.c_str());
  }

  // Added: a value that is not six hex digits keeps the fallback for that name only.
  {
    Scheme s = load_scheme(scratch_file(dir, R"({"surface": "141218", "primary": "7fd9c3", "error": "#abc"})"));
    CHECK_EQ(s.colours["error"], fallback_palette().at("error"));
    CHECK_EQ(s.colours["surface"], std::string("#141218"));
    CHECK_EQ(s.colours["primary"], std::string("#7fd9c3"));
  }
  // Added: a file whose primary is not hex has no usable accent, so the whole palette is the fallback.
  {
    Scheme s = load_scheme(scratch_file(dir, R"({"surface": "141218", "primary": "zz"})"));
    CHECK(s.colours == fallback_palette(), "bad primary falls back");
  }

  // Added: an unknown mode string is dark.
  {
    Scheme s = load_scheme(scratch_file(dir, R"({"mode": "sepia", "surface": "141218", "primary": "7fd9c3"})"));
    CHECK_EQ(s.mode, std::string("dark"));
  }

  // test_css_sets_accent_buttons_explicitly
  const std::string css = build_material_css(fallback_palette());
  CHECK(css.find("button.suggested-action { background-color: #f7b6b9; color: #603337; }") != std::string::npos,
        "suggested-action buttons get explicit colours");
  CHECK(css.find("button.destructive-action { background-color: #f97758;") != std::string::npos,
        "destructive-action buttons get explicit colours");
  CHECK(css.find('$') == std::string::npos, "no unrendered template token");

  // Added: alpha colours are the rgba() of the palette colour, and a live palette reaches the CSS.
  CHECK(css.find("button.suggested-action:hover { background-color: rgba(247, 182, 185, 0.9); }") != std::string::npos,
        "hover uses rgba of primary");
  Scheme live = load_scheme(scratch_file(dir, kNested));
  const std::string live_css = build_material_css(live.colours);
  CHECK(live_css.find("button.suggested-action { background-color: #7fd9c3; color: #00382d; }") != std::string::npos,
        "live primary and onPrimary reach the accent button");

  fs::remove_all(dir);
  return finish("test_theme");
}

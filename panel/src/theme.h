// caelestia-shell's live Material You palette and the libadwaita CSS built from it.
#pragma once

#include <map>
#include <string>

#include "paths.h"

// Colour names (surface, primary, ...) to "#rrggbb".
using Palette = std::map<std::string, std::string>;

struct Scheme {
  Palette colours;
  std::string mode;  // "dark" or "light"
};

// The static palette used when scheme.json is missing, broken or has no usable surface/primary.
const Palette &fallback_palette();

// Reads a caelestia scheme (nested "colours" or flat). Values without '#' get one; a value that is not
// six hex digits is ignored (keeps the fallback for that name).
Scheme load_scheme(const std::string &path = scheme_json());

// The CSS text that remaps libadwaita's named colours and styles the panel.
std::string build_material_css(const Palette &colours);

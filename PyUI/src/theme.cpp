#include "pyui/theme.h"

#include <cstring>

namespace pyui {

const Theme* themeByName(const char* name) {
  if (name == nullptr || name[0] == '\0') return &kLightTheme;
  if (std::strcmp(name, "dark") == 0) return &kDarkTheme;
  if (std::strcmp(name, "light") == 0) return &kLightTheme;
  return &kLightTheme;
}

}  // namespace pyui

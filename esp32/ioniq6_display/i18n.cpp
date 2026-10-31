#include "i18n.h"

#include <Preferences.h>

namespace i18n {

void load() {
  Preferences p;
  p.begin("ui", true);
  lang = p.getInt("lang", DE) == EN ? EN : DE;
  p.end();
}

void set(Lang l) {
  lang = l;
  Preferences p;
  p.begin("ui", false);
  p.putInt("lang", l);
  p.end();
}

}  // namespace i18n

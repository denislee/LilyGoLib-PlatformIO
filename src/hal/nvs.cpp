#include "nvs.h"

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace hal {

std::string nvs_get_str(const char *ns, const char *key, const char *dflt)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return dflt ? dflt : "";
    String v = p.getString(key, dflt ? dflt : "");
    p.end();
    return std::string(v.c_str());
#else
    (void)ns; (void)key;
    return dflt ? dflt : "";
#endif
}

void nvs_set_str(const char *ns, const char *key, const char *value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    if (value && *value) p.putString(key, value);
    else                 p.remove(key);
    p.end();
#else
    (void)ns; (void)key; (void)value;
#endif
}

bool nvs_get_bool(const char *ns, const char *key, bool dflt)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, true)) return dflt;
    bool v = p.getBool(key, dflt);
    p.end();
    return v;
#else
    (void)ns; (void)key;
    return dflt;
#endif
}

void nvs_set_bool(const char *ns, const char *key, bool value)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(ns, false)) return;
    p.putBool(key, value);
    p.end();
#else
    (void)ns; (void)key; (void)value;
#endif
}

} // namespace hal

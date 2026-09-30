/* See uber_json.h. */
#include "uber_json.h"

#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Past a string that starts at its opening quote. */
static const char *str_end(const char *p, const char *e)
{
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return e;
}

const char *jskip(const char *v, const char *e)
{
    v = ws(v, e);
    if (v >= e) return e;
    if (*v == '"') return str_end(v, e);
    if (*v == '{' || *v == '[') {
        int depth = 0;
        for (const char *p = v; p < e; p++) {
            if (*p == '"') { p = str_end(p, e) - 1; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
        }
        return e;
    }
    while (v < e && *v != ',' && *v != '}' && *v != ']') v++;
    return v;
}

const char *jkey(const char *obj, const char *e, const char *key)
{
    if (!obj) return NULL;
    const char *p = ws(obj, e);
    if (p >= e || *p != '{') return NULL;
    const size_t kl = strlen(key);
    p++;
    for (;;) {
        p = ws(p, e);
        if (p >= e || *p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *k = p + 1, *ke = str_end(p, e) - 1;
        p = ws(ke + 1, e);
        if (p >= e || *p != ':') return NULL;
        p = ws(p + 1, e);
        if ((size_t)(ke - k) == kl && !memcmp(k, key, kl)) return p;
        p = ws(jskip(p, e), e);
        if (p < e && *p == ',') p++;
    }
}

bool jstr(const char *v, const char *e, char *out, size_t cap)
{
    if (!cap) return false;
    out[0] = 0;
    if (!v) return false;
    v = ws(v, e);
    if (v >= e || *v != '"') return false;
    size_t o = 0;
    for (const char *p = v + 1; p < e && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p + 1 < e) {
            c = *++p;
            if (c == 'n' || c == 't' || c == 'r') c = ' ';
            else if (c == 'u') {                 /* é: a dial has no use for it */
                p += p + 4 < e ? 4 : 0;
                c = '?';
            }
        }
        if (o + 1 < cap) out[o++] = c;
    }
    out[o] = 0;
    return true;
}

double jnum(const char *v, const char *e, double def)
{
    if (!v) return def;
    v = ws(v, e);
    if (v >= e || !(*v == '-' || (*v >= '0' && *v <= '9'))) return def;
    char b[32];
    size_t n = 0;
    while (v < e && n + 1 < sizeof b && strchr("+-.eE0123456789", *v)) b[n++] = *v++;
    b[n] = 0;
    return strtod(b, NULL);
}

bool jbool(const char *v, const char *e)
{
    if (!v) return false;
    v = ws(v, e);
    return e - v >= 4 && !memcmp(v, "true", 4);
}

const char *jnext(const char **it, const char *e)
{
    const char *p = ws(*it, e);
    if (p >= e) return NULL;
    if (*p == '[' || *p == ',') p = ws(p + 1, e);
    if (p >= e || *p == ']') { *it = e; return NULL; }
    *it = jskip(p, e);
    return p;
}

bool jo_str(const char *obj, const char *e, const char *key, char *out, size_t cap)
{
    return jstr(jkey(obj, e, key), e, out, cap);
}

double jo_num(const char *obj, const char *e, const char *key, double def)
{
    return jnum(jkey(obj, e, key), e, def);
}

bool jo_bool(const char *obj, const char *e, const char *key)
{
    return jbool(jkey(obj, e, key), e);
}

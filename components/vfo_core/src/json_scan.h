/* A JSON reader with no tree, for vfo_core's parsers (adsb.c, ais.c): it
 * walks the text once; each value is read where it is wanted, or stepped
 * over. Bounded by `end`, so the text need not end in a NUL. Static, in
 * each file that includes it: no symbols of its own. */
#ifndef VFO_JSON_SCAN_H
#define VFO_JSON_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p, *end; } js_t;

__attribute__((unused)) static void ws(js_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t')) s->p++;
}

__attribute__((unused)) static char peek(js_t *s)
{
    ws(s);
    return s->p < s->end ? *s->p : 0;
}

__attribute__((unused)) static bool eat(js_t *s, char c)
{
    if (peek(s) != c) return false;
    s->p++;
    return true;
}

/* A string, into out (cap bytes, NUL-ended) or, out NULL, stepped over. An
 * escape keeps the character after the backslash; \uXXXX becomes '?'. */
__attribute__((unused)) static bool str(js_t *s, char *out, size_t cap)
{
    if (!eat(s, '"')) return false;
    size_t n = 0;
    while (s->p < s->end) {
        char c = *s->p++;
        if (c == '"') {
            if (out && cap) out[n] = 0;
            return true;
        }
        if (c == '\\') {
            if (s->p >= s->end) return false;
            c = *s->p++;
            if (c == 'u') {
                if (s->end - s->p < 4) return false;
                s->p += 4;
                c = '?';
            }
        }
        if (out && n + 1 < cap) out[n++] = c;
    }
    return false;
}

__attribute__((unused)) static bool num(js_t *s, double *v)
{
    ws(s);
    char b[32];
    size_t n = 0;
    while (s->p < s->end && n < sizeof b - 1 && *s->p && strchr("+-0123456789.eE", *s->p)) b[n++] = *s->p++;
    if (!n) return false;
    b[n] = 0;
    char *e;
    *v = strtod(b, &e);
    return e != b;
}

__attribute__((unused)) static bool skip(js_t *s, int depth)
{
    const char c = peek(s);
    if (c == '"') return str(s, NULL, 0);
    if (c == '{' || c == '[') {
        if (depth > 32) return false;
        const char close = c == '{' ? '}' : ']';
        s->p++;
        if (eat(s, close)) return true;
        for (;;) {
            if (c == '{' && (!str(s, NULL, 0) || !eat(s, ':'))) return false;
            if (!skip(s, depth + 1)) return false;
            if (eat(s, ',')) continue;
            return eat(s, close);
        }
    }
    const char *start = s->p;                /* a number, true, false, null */
    while (s->p < s->end && *s->p != ',' && *s->p != '}' && *s->p != ']' && *s->p != ' ' &&
           *s->p != '\n' && *s->p != '\r' && *s->p != '\t') s->p++;
    return s->p > start;
}

/* A text field: a string copied and its trailing blanks cut; anything else
 * (null) stepped over and the field left empty. */
__attribute__((unused)) static bool text(js_t *s, char *out, size_t cap)
{
    out[0] = 0;
    if (peek(s) != '"') return skip(s, 0);
    if (!str(s, out, cap)) return false;
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = 0;
    return true;
}

/* A number field: false only for broken text; *got says if it was one. */
__attribute__((unused)) static bool number(js_t *s, double *v, bool *got)
{
    const char c = peek(s);
    if (c == '-' || (c >= '0' && c <= '9')) return (*got = num(s, v));
    *got = false;
    return skip(s, 0);
}

#endif /* VFO_JSON_SCAN_H */

/* A device's battery, from its AT commands. See batt.h. */
#include "batt.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static const char *skip(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* A whole number, with spaces either side: past it and them, or NULL for
 * none -- or one longer than any of these commands carries. */
static const char *number(const char *s, long *v)
{
    s = skip(s);
    const bool neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    if (!isdigit((unsigned char)*s)) return NULL;
    long n = 0;
    for (int digits = 0; isdigit((unsigned char)*s); s++) {
        if (++digits > 6) return NULL;
        n = n * 10 + (*s - '0');
    }
    *v = neg ? -n : n;
    return skip(s);
}

/* AT+XAPL=<vendor>-<product>-<version>,<features>: the device as it names
 * itself, whatever is before the comma, and its features after it. */
static bool xapl(const char *s, batt_at_t *out)
{
    const char *comma = strchr(s, ',');
    if (!comma) return false;
    const char *e = comma;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    if (e == s) return false;
    size_t n = (size_t)(e - s);
    if (n > sizeof out->id - 1) n = sizeof out->id - 1;
    for (size_t i = 0; i < n; i++) out->id[i] = isprint((unsigned char)s[i]) ? s[i] : '?';
    out->id[n] = 0;
    long f;
    const char *t = number(comma + 1, &f);
    if (!t || *t || f < 0) return false;
    out->features = (int)f;
    return true;
}

/* AT+IPHONEACCEV=<n>,<key>,<value>,...: n pairs, no more and no fewer. Key
 * 1 is the battery, 0-9 for 10-100 % -- the first in range, if it says it
 * twice; one out of range is none. The rest (2 docked, 3 Siri, 4 noise
 * reduction) are taken and passed over. */
static bool accev(const char *s, batt_at_t *out)
{
    long n;
    const char *t = number(s, &n);
    if (!t || n < 0) return false;
    int8_t pct = -1;
    for (long i = 0; i < n; i++) {
        long k, v;
        if (*t != ',') return false;
        t = number(t + 1, &k);
        if (!t || *t != ',') return false;
        t = number(t + 1, &v);
        if (!t) return false;
        if (k == 1 && pct < 0 && v >= 0 && v <= 9) pct = (int8_t)((v + 1) * 10);
    }
    if (*t) return false;
    out->pct = pct;
    return true;
}

/* AT+BIEV=2,<0-100>: the hands-free profile's battery indicator. Any other
 * indicator, or a charge out of range, is not taken. */
static bool biev(const char *s, batt_at_t *out)
{
    long ind, v;
    const char *t = number(s, &ind);
    if (!t || *t != ',') return false;
    t = number(t + 1, &v);
    if (!t || *t || ind != 2 || v < 0 || v > 100) return false;
    out->pct = (int8_t)v;
    return true;
}

void batt_at_parse(const char *at, batt_at_t *out)
{
    memset(out, 0, sizeof *out);
    out->pct = -1;
    if (!at) return;
    const char *s = skip(at);
    if ((s[0] == 'A' || s[0] == 'a') && (s[1] == 'T' || s[1] == 't')) s = skip(s + 2);
    if (*s != '+') return;
    s++;
    /* Its name, in capitals: one longer than any of these is another. */
    char name[16];
    size_t n = 0;
    while (n < sizeof name - 1 && isalpha((unsigned char)s[n])) {
        name[n] = (char)toupper((unsigned char)s[n]);
        n++;
    }
    name[n] = 0;
    s += n;
    if (isalnum((unsigned char)*s)) return;
    if      (!strcmp(name, "XAPL"))        out->cmd = BATT_AT_XAPL;
    else if (!strcmp(name, "IPHONEACCEV")) out->cmd = BATT_AT_ACCEV;
    else if (!strcmp(name, "BIEV"))        out->cmd = BATT_AT_BIEV;
    else return;
    /* A set, nothing else: a read, a test, or the bare name is not one. */
    s = skip(s);
    if (*s != '=') return;
    s = skip(s + 1);
    if (*s == '?') return;
    switch (out->cmd) {
    case BATT_AT_XAPL:  out->ok = xapl(s, out);  break;
    case BATT_AT_ACCEV: out->ok = accev(s, out); break;
    default:            out->ok = biev(s, out);  break;
    }
    if (!out->ok) out->pct = -1;
}

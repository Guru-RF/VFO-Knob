/* Reading the UberSDR's JSON where it lies, with no allocation: its messages
 * come fifty a second, a spot replay is two hundred of them, and cJSON would
 * build each as a tree of small blocks in internal RAM.
 *
 * A value is a pointer to its first character, within [s, e). */
#ifndef UBER_JSON_H
#define UBER_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The value of `key` in the object at `obj` (its '{'), at that level only;
 * NULL if it has none. */
const char *jkey(const char *obj, const char *e, const char *key);
/* Past the value at v; e if it runs off the end. */
const char *jskip(const char *v, const char *e);

/* The value as a string (unescaped as far as a dial needs), a number, a
 * bool; `def` or false when it is not one. */
bool   jstr(const char *v, const char *e, char *out, size_t cap);
double jnum(const char *v, const char *e, double def);
bool   jbool(const char *v, const char *e);

/* An array's elements: `*it` starts at the array's '['; each call gives the
 * next element's value, NULL after the last. */
const char *jnext(const char **it, const char *e);

/* Shortcuts on an object: its key's string, number or bool. */
bool   jo_str(const char *obj, const char *e, const char *key, char *out, size_t cap);
double jo_num(const char *obj, const char *e, const char *key, double def);
bool   jo_bool(const char *obj, const char *e, const char *key);

#endif /* UBER_JSON_H */

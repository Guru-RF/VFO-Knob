/* Minimal assert harness. Deliberately dependency-free so `vfo_core` can be
 * tested with nothing but cc + cmake, on any machine, with no ESP-IDF. */
#ifndef TINY_H
#define TINY_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_run = 0, t_fail = 0;
static const char *t_case = "";

#define CASE(n) do { t_case = (n); } while (0)

#define CHECK(cond)                                                          \
    do {                                                                     \
        t_run++;                                                             \
        if (!(cond)) {                                                       \
            t_fail++;                                                        \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n",                         \
                    t_case, __FILE__, __LINE__, #cond);                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(got, want)                                                  \
    do {                                                                     \
        t_run++;                                                             \
        long long g_ = (long long)(got), w_ = (long long)(want);             \
        if (g_ != w_) {                                                      \
            t_fail++;                                                        \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n  got  %lld\n  want %lld\n",\
                    t_case, __FILE__, __LINE__, #got, g_, w_);               \
        }                                                                    \
    } while (0)

#define CHECK_STR(got, want)                                                 \
    do {                                                                     \
        t_run++;                                                             \
        if (strcmp((got), (want)) != 0) {                                    \
            t_fail++;                                                        \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n  got  \"%s\"\n  want \"%s\"\n",\
                    t_case, __FILE__, __LINE__, #got, (got), (want));        \
        }                                                                    \
    } while (0)

#define T_MAIN(body)                                                         \
    int main(void) {                                                         \
        body;                                                                \
        printf("%s: %d checks, %d failed\n", __FILE__, t_run, t_fail);       \
        return t_fail ? 1 : 0;                                               \
    }

#endif /* TINY_H */

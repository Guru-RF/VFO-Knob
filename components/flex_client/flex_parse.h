/* The FlexRadio API's text, parsed: what the radio says about its memories
 * and its antennas, and what it broadcasts on the LAN to be found. Pure C --
 * no ESP-IDF -- so test/host takes it exactly as the knob does.
 *
 * The API's values carry no spaces: a space in a name is sent as 0x7F, and
 * comes back here as a space. */
#ifndef FLEX_PARSE_H
#define FLEX_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The value of `key` in a space-separated key=value list, NUL-terminated in
 * `out`. The key must start a word: "tx" is not "rit_tx". */
bool flex_kv(const char *line, const char *key, char *out, size_t cap);

/* "14.100000" MHz to Hz, without float: 24 bits of mantissa would round a
 * 54 MHz frequency to several Hz. */
bool flex_kv_mhz(const char *line, const char *key, int64_t *hz);

/* --- a radio's discovery broadcast ------------------------------------------
 * Once a second every radio on the LAN broadcasts, to UDP 4992, a VITA-49
 * packet -- extension data, FlexRadio's OUI, packet class 0xFFFF -- whose
 * payload is key=value text: model=FLEX-6600 serial=... nickname=... ip=...
 * port=4992 status=Available, and who is on it. Bare text, as some
 * gateways send it, is taken too. */
typedef struct {
    char     serial[24];
    char     model[16];
    char     nickname[24];
    char     callsign[16];
    char     ip[16];             /* "" where it said none: its sender's, then */
    char     version[24];
    char     status[16];         /* "Available", "In_Use" ... */
    char     who[32];            /* the stations on it, or whoever uses it */
    uint16_t port;               /* 4992 where it said none */
} flex_disc_t;

/* False for anything not a radio's: another kind of VITA-49 packet, or no
 * serial number. */
bool flex_disc_parse(const uint8_t *b, size_t n, flex_disc_t *out);

/* --- the memory channels ------------------------------------------------------
 * `sub memories all` brings one status per memory -- "memory 3 owner=ON6URE
 * group= freq=145.600000 name=ON0ORA mode=FM repeater=DOWN
 * repeater_offset=0.600000 tone_mode=CTCSS_TX tone_value=79.7
 * rx_filter_low=-8000 rx_filter_high=8000 ..." -- then another as one
 * changes, with what changed, and "memory 3 removed" when one goes. */
typedef struct {
    uint16_t idx;                /* the radio's number for it */
    int64_t  hz;
    int32_t  offset_hz;          /* the repeater's shift, Hz */
    int32_t  lo, hi;             /* its receive filter; both 0: the mode's own */
    uint16_t tone_dhz;           /* the CTCSS tone, 0.1 Hz */
    bool     tone_on;            /* ...sent */
    int8_t   duplex;             /* -1 DOWN, 0 SIMPLEX, +1 UP */
    char     mode[8];            /* lower case, as the editors name them */
    char     name[17];
} flex_mem_t;

/* "memory <n> ...": n, or -1 for a status that is not a memory's. */
int  flex_mem_index(const char *body);

/* What the status says, into `m` -- the memory as it was, or zeroes for a
 * new one: a key it does not carry leaves that field alone. *removed when
 * the radio says the memory is gone. */
void flex_mem_parse(const char *body, flex_mem_t *m, bool *removed);

/* The memories kept in order of frequency, then of their numbers: where the
 * one numbered idx is (-1 none); one put in -- in place of the one with its
 * number -- returning where it went (-1: no room); one taken out; the one
 * nearest a frequency; one on it, within tol Hz (-1 none). */
int  flex_mem_find(const flex_mem_t *list, int n, uint16_t idx);
int  flex_mem_put(flex_mem_t *list, int *n, int cap, const flex_mem_t *m);
bool flex_mem_drop(flex_mem_t *list, int *n, uint16_t idx);
int  flex_mem_nearest(const flex_mem_t *list, int n, int64_t hz);
int  flex_mem_on(const flex_mem_t *list, int n, int64_t hz, int32_t tol);

/* --- lists of names, as a slice gives its antennas: "ANT1,ANT2,RX_A" ---------
 * How many, and the place of one (-1 if it is not there). */
int  flex_list_count(const char *list);
int  flex_list_find(const char *list, const char *name);

#endif /* FLEX_PARSE_H */

/* A device's battery, as its hands-free link reports it: AT commands the
 * stack does not know, which it hands this chip as they came, less their
 * "AT" (hfp.c). Plain C, with nothing of the stack's: test/host tests it.
 *
 * Apple's way, which most headsets and speakers made for phones take:
 *
 *   AT+XAPL=<vendor>-<product>-<version>,<features>
 *       what the device is, and what it reports (BATT_XAPL_*). Answered
 *       "+XAPL=iPhone,<ours>" and OK, as an iPhone answers it -- ours the
 *       battery alone, as Android's.
 *   AT+IPHONEACCEV=<n>,<key>,<value>,...
 *       its news, n pairs of them: key 1 its battery, 0-9 for 10-100 %.
 *
 * And the hands-free profile's own (1.7): AT+BIEV=2,<0-100>, its battery
 * indicator. The stack never offers a headset its indicators (hfp.c), so a
 * device sends that only unasked. */
#ifndef BATT_H
#define BATT_H

#include <stdbool.h>
#include <stdint.h>

/* AT+XAPL's features: what the device reports, and, in the answer, what
 * this chip takes of it. */
enum {
    BATT_XAPL_BATTERY = 2,     /* its battery (AT+IPHONEACCEV's key 1) */
    BATT_XAPL_DOCK    = 4,     /* whether it is docked or powered (key 2) */
    BATT_XAPL_SIRI    = 8,     /* Siri's state */
    BATT_XAPL_NR      = 16,    /* its noise reduction's */
};
/* This chip's answer: the battery, nothing else. */
#define BATT_XAPL_ANSWER "+XAPL=iPhone,2"

enum { BATT_AT_OTHER = 0, BATT_AT_XAPL, BATT_AT_ACCEV, BATT_AT_BIEV };

typedef struct {
    uint8_t cmd;               /* BATT_AT_*: OTHER for any other command */
    bool    ok;                /* one of the three, well formed: answered OK, XAPL with ours
                                  first. Not: ERROR, as anything the stack does not know */
    int8_t  pct;               /* the charge it says, 0-100 %; -1 none in it, or one out of range */
    int     features;          /* XAPL's: BATT_XAPL_* */
    char    id[24];            /* XAPL's: <vendor>-<product>-<version>, as it says, cut to fit */
} batt_at_t;

/* One command, as the stack hands it over -- its "AT" there or not, its
 * letters in either case, spaces around its fields. Never NULL out; `at`
 * may be. */
void batt_at_parse(const char *at, batt_at_t *out);

#endif /* BATT_H */

/* TCI v2.0 line parser — pure C, no ESP-IDF, no allocation.
 *
 * AetherSDR's TCI server (src/core/TciProtocol.cpp) speaks semicolon-terminated
 * ASCII, one command per WebSocket text frame. The same strings serve as both
 * commands and notifications.
 *
 * Note there is no error frame in TCI (TciServer.cpp:2124). A rejected command
 * comes back as an authoritative value, not as an error, which is why callers
 * must reconcile rather than wait for a NACK.
 */
#ifndef VFO_TCI_PARSE_H
#define VFO_TCI_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    TCI_NONE = 0,
    TCI_UNKNOWN,            /* well-formed but not in our vocabulary */

    /* greeting */
    TCI_PROTOCOL,           /* s0 = "ExpertSDR3,1.5"            */
    TCI_DEVICE,             /* s0                               */
    TCI_READY,
    TCI_TRX_COUNT,          /* i0                               */
    TCI_MODULATIONS_LIST,   /* s0 (truncated)                   */
    TCI_RECEIVE_ONLY,       /* b0                               */
    TCI_CHANNELS_COUNT,     /* i0                               */
    TCI_VFO_LIMITS,         /* ignored: hardcoded server-side   */
    TCI_IF_LIMITS,          /* ignored: hardcoded server-side   */

    /* state */
    TCI_VFO,                /* trx, channel, hz                 */
    TCI_TRX,                /* trx, b0  -- PTT                  */
    TCI_MODULATION,         /* trx, s0                          */
    TCI_RX_FILTER_BAND,     /* trx, i0 = lo, i1 = hi            */
    TCI_RIT_OFFSET,         /* trx, i0                          */
    TCI_RIT_ENABLE,         /* trx, b0                          */
    TCI_XIT_OFFSET,         /* trx, i0                          */
    TCI_XIT_ENABLE,         /* trx, b0                          */
    TCI_SPLIT_ENABLE,       /* trx, b0                          */
    TCI_LOCK,               /* trx, b0                          */
    TCI_TX_ENABLE,          /* trx, b0                          */
    TCI_DRIVE,              /* trx, i0                          */
    TCI_TUNE,               /* trx, b0                          */
    TCI_ACTIVE_SLICE,       /* i0 = index, s0 = display letter   */

    /* Audio stream format, advertised in the greeting. v1 stores and ignores
     * these so that v2's RX audio work does not have to touch the parser. */
    TCI_AUDIO_SAMPLERATE,      /* i0 */
    TCI_AUDIO_SAMPLE_TYPE,     /* s0 */
    TCI_AUDIO_CHANNELS,        /* i0 */
    TCI_AUDIO_SAMPLES,         /* i0 */
    TCI_TX_AUDIO_BUFFERING,    /* i0 */

    /* telemetry */
    TCI_RX_SMETER,          /* trx, i0 = dBm (int, truncated)   */
    TCI_RX_CHANNEL_SENSORS, /* trx, channel, f0 = dBm           */
    TCI_TX_SENSORS,         /* f0 mic, f1 fwd_w, f2 peak_w,
                               f3 swr, f4 alc_dbfs              */

    /* recognised and deliberately discarded (see tci_is_ignorable) */
    TCI_DDS,                /* arrives with every accepted tune */
    TCI_IGNORED,
} tci_kind_t;

#define TCI_STR_MAX 32

typedef struct {
    tci_kind_t kind;
    int32_t    trx;         /* -1 when the command carries no trx */
    int32_t    channel;     /* -1 when absent                     */
    int64_t    hz;
    int32_t    i0, i1;
    float      f0, f1, f2, f3, f4;
    bool       b0;
    char       s0[TCI_STR_MAX];
} tci_fact_t;

/* Parse one command. `line` need not be NUL-terminated within `len`, and may
 * or may not include the trailing ';'. Leading/trailing whitespace is ignored.
 * Returns false only for empty input; unrecognised-but-well-formed input
 * yields TCI_UNKNOWN so the caller can count it (see milestone M15, which
 * requires zero TCI_UNKNOWN over a full session).
 *
 * Never allocates, never reads past `len`, and is safe on arbitrary bytes. */
bool tci_parse(const char *line, size_t len, tci_fact_t *out);

/* True for commands we recognise but deliberately do not act on. Keeping these
 * distinct from TCI_UNKNOWN is what makes the M15 "zero unparsed" check useful. */
bool tci_is_ignorable(tci_kind_t k);

const char *tci_kind_name(tci_kind_t k);

#endif /* VFO_TCI_PARSE_H */

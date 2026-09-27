#include "tci_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp: <string.h> only exposes it under _DEFAULT_SOURCE */

#define LINE_MAX 200
#define ARG_MAX  8

static const struct { const char *name; tci_kind_t kind; } NAMES[] = {
    { "vfo",                 TCI_VFO                 },
    { "trx",                 TCI_TRX                 },
    { "modulation",          TCI_MODULATION          },
    { "rx_filter_band",      TCI_RX_FILTER_BAND      },
    { "rit_offset",          TCI_RIT_OFFSET          },
    { "rit_enable",          TCI_RIT_ENABLE          },
    { "xit_offset",          TCI_XIT_OFFSET          },
    { "xit_enable",          TCI_XIT_ENABLE          },
    { "split_enable",        TCI_SPLIT_ENABLE        },
    { "lock",                TCI_LOCK                },
    { "tx_enable",           TCI_TX_ENABLE           },
    { "drive",               TCI_DRIVE               },
    { "tune",                TCI_TUNE                },
    { "active_slice",        TCI_ACTIVE_SLICE        },
    { "rx_smeter",           TCI_RX_SMETER           },
    { "rx_channel_sensors",  TCI_RX_CHANNEL_SENSORS  },
    { "tx_sensors",          TCI_TX_SENSORS          },
    { "ready",               TCI_READY               },
    { "protocol",            TCI_PROTOCOL            },
    { "device",              TCI_DEVICE              },
    { "trx_count",           TCI_TRX_COUNT           },
    { "modulations_list",    TCI_MODULATIONS_LIST    },
    { "receive_only",        TCI_RECEIVE_ONLY        },
    /* Audio stream format from the greeting -- stored for v2, unused in v1. */
    { "audio_samplerate",         TCI_AUDIO_SAMPLERATE   },
    { "audio_stream_sample_type", TCI_AUDIO_SAMPLE_TYPE  },
    { "audio_stream_channels",    TCI_AUDIO_CHANNELS     },
    { "audio_stream_samples",     TCI_AUDIO_SAMPLES      },
    { "tx_stream_audio_buffering",TCI_TX_AUDIO_BUFFERING },
    { "channels_count",      TCI_CHANNELS_COUNT      },
    { "vfo_limits",          TCI_VFO_LIMITS          },
    { "if_limits",           TCI_IF_LIMITS           },
    { "dds",                 TCI_DDS                 },
    /* Recognised, never acted on. Listing them keeps TCI_UNKNOWN meaningful:
     * milestone M15 requires zero TCI_UNKNOWN over a full session. */
    { "start",               TCI_IGNORED             },
    { "stop",                TCI_IGNORED             },
    { "volume",              TCI_IGNORED             },
    { "mute",                TCI_IGNORED             },
    { "rx_mute",             TCI_IGNORED             },
    { "mic_level",           TCI_IGNORED             },
    { "tune_drive",          TCI_IGNORED             },
    { "rx_enable",           TCI_IGNORED             },
    { "sql_enable",          TCI_IGNORED             },
    { "sql_level",           TCI_IGNORED             },
    { "rx_volume",           TCI_IGNORED             },
    { "rx_balance",          TCI_IGNORED             },
    { "agc_mode",            TCI_IGNORED             },
    { "agc_gain",            TCI_IGNORED             },
    { "rx_nb_enable",        TCI_IGNORED             },
    { "rx_nr_enable",        TCI_IGNORED             },
    { "rx_anf_enable",       TCI_IGNORED             },
    { "rx_nf_enable",        TCI_IGNORED             },
    { "rx_apf_enable",       TCI_IGNORED             },
    { "rx_dse_enable",       TCI_IGNORED             },
    { "rx_anc_enable",       TCI_IGNORED             },
    { "rx_bin_enable",       TCI_IGNORED             },
    { "rx_channel_enable",   TCI_IGNORED             },
    { "rx_nb_param",         TCI_IGNORED             },
    { "rx_play",             TCI_IGNORED             },
    { "rx_record",           TCI_IGNORED             },
    { "mon_enable",          TCI_IGNORED             },
    { "mon_volume",          TCI_IGNORED             },
    { "tx_frequency",        TCI_IGNORED             },
    { "tx_gain",             TCI_IGNORED             },
    { "cw_macros_delay",     TCI_IGNORED             },
    { "cw_macros_stop",      TCI_IGNORED             },
    { "cw_terminal",         TCI_IGNORED             },
    { "callsign",            TCI_IGNORED             },
    { "cw_macros_speed",     TCI_IGNORED             },
    { "cw_keyer_speed",      TCI_IGNORED             },
    { "spot",                TCI_IGNORED             },
    { "spot_clear",          TCI_IGNORED             },
    { "spot_delete",         TCI_IGNORED             },
    { "iq_start",            TCI_IGNORED             },
    { "iq_stop",             TCI_IGNORED             },
    { "audio_start",         TCI_IGNORED             },
    { "audio_stop",          TCI_IGNORED             },
    { "iq_samplerate",       TCI_IGNORED             },
    { "rx_sensors_enable",   TCI_IGNORED             },
    { "tx_sensors_enable",   TCI_IGNORED             },
    { "set_in_focus",        TCI_IGNORED             },
    { "vfo_lock",            TCI_IGNORED             },
    { "keyer",               TCI_IGNORED             },
    { "cw_msg",              TCI_IGNORED             },
};

static bool arg_bool(const char *s, bool *out)
{
    if (!s) return false;
    /* AetherSDR is strict about true/false; we accept either case. */
    if (strcasecmp(s, "true")  == 0) { *out = true;  return true; }
    if (strcasecmp(s, "false") == 0) { *out = false; return true; }
    return false;
}

static int64_t arg_i64(char *const argv[], int argc, int n, int64_t def)
{
    if (n >= argc || !argv[n] || argv[n][0] == '\0') return def;
    char *end = NULL;
    long long v = strtoll(argv[n], &end, 10);
    if (end == argv[n]) return def;
    return (int64_t)v;
}

static float arg_f(char *const argv[], int argc, int n, float def)
{
    if (n >= argc || !argv[n] || argv[n][0] == '\0') return def;
    char *end = NULL;
    float v = strtof(argv[n], &end);
    if (end == argv[n]) return def;
    return v;
}

static int split_args(char *s, char *argv[], int max)
{
    int n = 0;
    if (!s || *s == '\0') return 0;
    argv[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == ',') { *p = '\0'; argv[n++] = p + 1; }
    }
    return n;
}

bool tci_is_ignorable(tci_kind_t k)
{
    return k == TCI_IGNORED || k == TCI_DDS ||
           k == TCI_VFO_LIMITS || k == TCI_IF_LIMITS;
}

bool tci_parse(const char *line, size_t len, tci_fact_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->kind    = TCI_NONE;
    out->trx     = -1;
    out->channel = -1;

    if (!line) return false;

    /* Trim whitespace and the terminator. */
    while (len && (unsigned char)line[0] <= ' ')        { line++; len--; }
    while (len && ((unsigned char)line[len - 1] <= ' ' ||
                   line[len - 1] == ';'))               { len--; }
    if (len == 0) return false;
    if (len > LINE_MAX - 1) len = LINE_MAX - 1;

    char buf[LINE_MAX];
    memcpy(buf, line, len);
    buf[len] = '\0';

    char *colon = memchr(buf, ':', len);
    size_t nlen = colon ? (size_t)(colon - buf) : len;
    for (size_t i = 0; i < nlen; i++)
        buf[i] = (char)tolower((unsigned char)buf[i]);

    char *rest = colon ? colon + 1 : buf + len; /* "" when no args */
    if (colon) *colon = '\0';

    tci_kind_t kind = TCI_UNKNOWN;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        if (strcmp(buf, NAMES[i].name) == 0) { kind = NAMES[i].kind; break; }
    }
    out->kind = kind;
    if (kind == TCI_UNKNOWN || kind == TCI_IGNORED || kind == TCI_READY)
        return true;

    /* Free-text payloads must not be split on commas. */
    if (kind == TCI_PROTOCOL || kind == TCI_DEVICE || kind == TCI_MODULATIONS_LIST) {
        strncpy(out->s0, rest, TCI_STR_MAX - 1);
        out->s0[TCI_STR_MAX - 1] = '\0';
        return true;
    }

    char *argv[ARG_MAX] = { 0 };
    int argc = split_args(rest, argv, ARG_MAX);

    switch (kind) {
    case TCI_VFO:
        out->trx     = (int32_t)arg_i64(argv, argc, 0, -1);
        out->channel = (int32_t)arg_i64(argv, argc, 1, -1);
        out->hz      =          arg_i64(argv, argc, 2, 0);
        break;

    case TCI_TRX:
    case TCI_RIT_ENABLE:
    case TCI_XIT_ENABLE:
    case TCI_SPLIT_ENABLE:
    case TCI_LOCK:
    case TCI_TX_ENABLE:
    case TCI_TUNE:
        out->trx = (int32_t)arg_i64(argv, argc, 0, -1);
        if (argc < 2 || !arg_bool(argv[1], &out->b0)) {
            /* TciProtocol drops a malformed state silently; mirror that. */
            out->kind = TCI_UNKNOWN;
            return true;
        }
        break;

    case TCI_MODULATION:
        out->trx = (int32_t)arg_i64(argv, argc, 0, -1);
        if (argc > 1 && argv[1]) {
            strncpy(out->s0, argv[1], TCI_STR_MAX - 1);
            out->s0[TCI_STR_MAX - 1] = '\0';
            for (char *p = out->s0; *p; p++)
                *p = (char)tolower((unsigned char)*p);
        }
        break;

    case TCI_RX_FILTER_BAND:
    case TCI_VFO_LIMITS:
    case TCI_IF_LIMITS:
        out->trx = (int32_t)arg_i64(argv, argc, 0, -1);
        out->i0  = (int32_t)arg_i64(argv, argc, 1, 0);
        out->i1  = (int32_t)arg_i64(argv, argc, 2, 0);
        break;

    case TCI_RIT_OFFSET:
    case TCI_XIT_OFFSET:
    case TCI_DRIVE:
    case TCI_RX_SMETER:
        out->trx = (int32_t)arg_i64(argv, argc, 0, -1);
        out->i0  = (int32_t)arg_i64(argv, argc, 1, 0);
        break;

    case TCI_RX_CHANNEL_SENSORS:
        out->trx     = (int32_t)arg_i64(argv, argc, 0, -1);
        out->channel = (int32_t)arg_i64(argv, argc, 1, -1);
        out->f0      =          arg_f  (argv, argc, 2, -200.0f);
        break;

    case TCI_TX_SENSORS:
        /* trx is hardcoded 0 server-side (TciServer.cpp:3699). Field 2 is
         * "peak" but carries the same cached value as fwd; never render it.
         * Field 5 (alc_dbfs) is an AetherSDR extension. */
        out->trx = (int32_t)arg_i64(argv, argc, 0, 0);
        out->f0  =          arg_f  (argv, argc, 1, 0.0f);  /* mic dBm   */
        out->f1  =          arg_f  (argv, argc, 2, 0.0f);  /* fwd watts */
        out->f2  =          arg_f  (argv, argc, 3, 0.0f);  /* peak      */
        out->f3  =          arg_f  (argv, argc, 4, 1.0f);  /* swr       */
        out->f4  =          arg_f  (argv, argc, 5, 0.0f);  /* alc dBFS  */
        break;

    case TCI_TRX_COUNT:
    case TCI_CHANNELS_COUNT:
    case TCI_AUDIO_SAMPLERATE:
    case TCI_AUDIO_CHANNELS:
    case TCI_AUDIO_SAMPLES:
    case TCI_TX_AUDIO_BUFFERING:
        out->i0 = (int32_t)arg_i64(argv, argc, 0, 0);
        break;

    case TCI_ACTIVE_SLICE:
        /* Real form is "active_slice:0,A;" -- index AND display letter. The
         * knob can only FOLLOW focus (the server ignores SETs and
         * set_in_focus is a stub), so the letter is a display, not a control. */
        out->i0 = (int32_t)arg_i64(argv, argc, 0, 0);
        if (argc > 1 && argv[1]) {
            strncpy(out->s0, argv[1], TCI_STR_MAX - 1);
            out->s0[TCI_STR_MAX - 1] = '\0';
        }
        break;

    case TCI_AUDIO_SAMPLE_TYPE:
        if (argc > 0 && argv[0]) {
            strncpy(out->s0, argv[0], TCI_STR_MAX - 1);
            out->s0[TCI_STR_MAX - 1] = '\0';
        }
        break;

    case TCI_RECEIVE_ONLY:
        arg_bool(argc > 0 ? argv[0] : NULL, &out->b0);
        break;

    case TCI_DDS:
        out->trx = (int32_t)arg_i64(argv, argc, 0, -1);
        out->hz  =          arg_i64(argv, argc, 1, 0);
        break;

    default:
        break;
    }
    return true;
}

const char *tci_kind_name(tci_kind_t k)
{
    switch (k) {
    case TCI_NONE:                return "none";
    case TCI_UNKNOWN:             return "unknown";
    case TCI_PROTOCOL:            return "protocol";
    case TCI_DEVICE:              return "device";
    case TCI_READY:               return "ready";
    case TCI_TRX_COUNT:           return "trx_count";
    case TCI_MODULATIONS_LIST:    return "modulations_list";
    case TCI_RECEIVE_ONLY:        return "receive_only";
    case TCI_AUDIO_SAMPLERATE:    return "audio_samplerate";
    case TCI_AUDIO_SAMPLE_TYPE:   return "audio_stream_sample_type";
    case TCI_AUDIO_CHANNELS:      return "audio_stream_channels";
    case TCI_AUDIO_SAMPLES:       return "audio_stream_samples";
    case TCI_TX_AUDIO_BUFFERING:  return "tx_stream_audio_buffering";
    case TCI_CHANNELS_COUNT:      return "channels_count";
    case TCI_VFO_LIMITS:          return "vfo_limits";
    case TCI_IF_LIMITS:           return "if_limits";
    case TCI_VFO:                 return "vfo";
    case TCI_TRX:                 return "trx";
    case TCI_MODULATION:          return "modulation";
    case TCI_RX_FILTER_BAND:      return "rx_filter_band";
    case TCI_RIT_OFFSET:          return "rit_offset";
    case TCI_RIT_ENABLE:          return "rit_enable";
    case TCI_XIT_OFFSET:          return "xit_offset";
    case TCI_XIT_ENABLE:          return "xit_enable";
    case TCI_SPLIT_ENABLE:        return "split_enable";
    case TCI_LOCK:                return "lock";
    case TCI_TX_ENABLE:           return "tx_enable";
    case TCI_DRIVE:               return "drive";
    case TCI_TUNE:                return "tune";
    case TCI_ACTIVE_SLICE:        return "active_slice";
    case TCI_RX_SMETER:           return "rx_smeter";
    case TCI_RX_CHANNEL_SENSORS:  return "rx_channel_sensors";
    case TCI_TX_SENSORS:          return "tx_sensors";
    case TCI_DDS:                 return "dds";
    case TCI_IGNORED:             return "ignored";
    }
    return "?";
}

/* The Elecraft K4's remote protocol, pure C: what comes off the wire and what
 * goes on it, with nothing of ESP-IDF in it -- so test/host/ takes it as the
 * knob takes it, bytes off a network that may be anything.
 *
 * Written from the K4 Programmer's Reference (rev. D12) and from K4remote's
 * notes on the K4/0 server's framing; none of K4remote's code is used.
 *
 * Every byte the K4 sends -- CAT text, audio, panadapter -- is wrapped in one
 * envelope, so a single TCP read may hold any mix of them:
 *
 *   [FE FD FC FB] [payload length, 4 bytes big-endian] [payload] [FB FC FD FE]
 *
 * The payload's first byte says what it is: 0x00 CAT (then two more bytes,
 * then ASCII commands ending in ';'), 0x01 audio, 0x02 and 0x03 the
 * panadapter, which this knob never asks for and skips.
 */
#ifndef K4_PARSE_H
#define K4_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define K4_PAY_CAT   0x00
#define K4_PAY_AUDIO 0x01

/* --- the envelope ------------------------------------------------------ */

/* A payload of more than this is skipped, not kept: a panadapter frame is
 * several kB, and nothing the knob reads is anywhere near it. */
#define K4_FRAME_MAX 2048

typedef void (*k4_frame_cb)(void *ctx, const uint8_t *payload, size_t len);

typedef struct {
    uint8_t  state;
    uint8_t  m;               /* bytes of the marker matched so far */
    uint32_t len, got;
    uint8_t  buf[K4_FRAME_MAX];
    uint32_t bad;             /* frames whose end marker was not there */
    uint32_t skipped;         /* frames too long to keep */
} k4_framer_t;

void k4_framer_init(k4_framer_t *f);

/* Feed bytes as they come, in pieces of any size. cb is called once for each
 * whole frame, with its payload (valid until cb returns). A frame that does
 * not end where its length says is dropped, and the framer looks for the next
 * start marker -- it never stops. */
void k4_framer_push(k4_framer_t *f, const uint8_t *data, size_t n, k4_frame_cb cb, void *ctx);

/* A CAT command ("FA00014074000;") as a whole frame into out; its length, or
 * 0 if it does not fit. */
size_t k4_encode_cat(uint8_t *out, size_t cap, const char *cmd);

/* --- audio ------------------------------------------------------------- */

/* [0x01][version][seq][mode][frame samples, LE16][rate code] then the data.
 * The K4 sends 12 kHz stereo Opus (left Main, right Sub) and takes 12 kHz
 * mono; the mode byte is the EM command's. */
#define K4_AUDIO_HDR 7
#define K4_EM_OPUS_INT   2
#define K4_EM_OPUS_FLOAT 3

typedef struct {
    uint8_t        seq, mode;
    uint16_t       samples;       /* per channel */
    const uint8_t *data;
    size_t         n;
} k4_audio_t;

bool   k4_audio_parse(const uint8_t *payload, size_t len, k4_audio_t *out);
/* A transmit audio frame (the whole envelope) into out; its length, or 0. */
size_t k4_encode_audio(uint8_t *out, size_t cap, uint8_t seq, uint8_t mode,
                       uint16_t samples, const uint8_t *data, size_t n);

/* --- CAT text ---------------------------------------------------------- */

/* The commands in a CAT payload, one at a time: *pos starts at 0, each call
 * copies the next command, without its ';', into cmd and moves on. False at
 * the end. A command longer than cap is cut, not left half in the next one. */
bool k4_cat_next(const uint8_t *payload, size_t len, size_t *pos, char *cmd, size_t cap);

/* What the radio has told us. A have_* flag says the field was set. */
typedef struct {
    bool     have_f;     int64_t f_hz;          /* VFO A */
    bool     have_mode;  char    mode[8];       /* the knob's names: "usb", "cwr", "digu" */
    bool     have_bw;    int32_t bw_hz;
    bool     have_tx;    bool    tx;
    bool     have_smeter; float  smeter_dbm;    /* SMH, high resolution */
    bool     have_bars;  int     bars;          /* SM: 0-42 */
    bool     have_tm;    float   alc, fwd_w, swr; int cmp_db;
    bool     have_agc;   uint8_t agc;           /* GT: 0 off, 1 slow, 2 fast */
    bool     have_rg;    int     rg_db;         /* RG: 0 to -60 */
    bool     have_rit;   int32_t rit_hz; bool rit_on;
    bool     have_pc;    uint16_t pc; char pc_range;
    bool     have_is;    int32_t is_hz;         /* IS: the passband's centre pitch */
    bool     have_nb;    bool nb_on;            /* the noise blanker */
    bool     have_nr;    bool nr_on; uint8_t nr_level;     /* noise reduction: 0-10 */
    bool     have_na;    bool na_on;            /* the auto notch */
    bool     have_ap;    bool ap_on; uint8_t ap_width;     /* the audio peak filter: 0-2 */
    bool     have_id;    char    id[12];        /* ID: "017" is a K4 */
    bool     pong;
    int      err;        /* ER: the code of an error the radio reported; 0 none */
    char     err_text[32];
} k4_status_t;

/* Take one command (no ';') into st. True if it was one this knows. */
bool k4_apply(k4_status_t *st, const char *cmd);

/* K4 MD digit <-> the knob's mode names. NULL / 0 for what has none. */
const char *k4_mode_name(char md);
char        k4_mode_digit(const char *name);

/* GT's digit <-> the knob's AGC names ("off", "slow", "fast"). */
const char *k4_agc_name(uint8_t gt);
int         k4_agc_digit(const char *name);        /* -1 for an unknown name */

/* --- commands ---------------------------------------------------------- */

size_t k4_cmd_freq(char *out, size_t cap, int64_t hz);           /* FA...; */
size_t k4_cmd_mode(char *out, size_t cap, char md);              /* MD3; */
size_t k4_cmd_bw(char *out, size_t cap, int32_t hz);             /* BW0240; (10 Hz units) */
size_t k4_cmd_rit(char *out, size_t cap, int32_t hz);            /* RO+0100; */
size_t k4_cmd_rg(char *out, size_t cap, int db);                 /* RG-12; */

/* The password's proof on the clear port (9205) is SHA-384 of it, in
 * lowercase hex, sent raw, before anything else. 96 characters and a NUL. */
#define K4_AUTH_HEX 97
void k4_hex(char *out, const uint8_t *digest, size_t n);

#endif /* K4_PARSE_H */

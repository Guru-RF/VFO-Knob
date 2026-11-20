/* TailChaser's API, as the knob reads it (TailChaser docs/API.md):
 *
 *   GET  /api/v1/status   everything the node's status page shows
 *   GET  /api/v1/block    {"rx":false,"tx":false}
 *   POST /api/v1/block    rx=1 / tx=0 ...; a control key only (403 for a
 *                         read key); the answer is the state after it
 *
 * Plain HTTP on the status page's port (8080), the key in X-API-Key, one
 * request a connection (Connection: close), Content-Length always there.
 *
 * Nothing here talks to the network: the parsing, the S-meter and the node
 * list, for the client and the host tests alike. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TC_NODES      8         /* repeaters on the dial                       */
#define TC_HOST      64         /* host name or address                        */
#define TC_KEY       96         /* tc_ and 64 hex digits; room to spare        */
#define TC_NAME      17         /* a repeater's name on the face, 16 at most   */
#define TC_PORT    8080         /* the status page's port, TailChaser's default */
#define TC_HOLD_S    20         /* the one shown kept after it was busy, s     */

typedef struct {
    char     host[TC_HOST];
    uint16_t port;
} tc_addr_t;

/* What the face and the page show of a node. A transmit site of a split
 * repeater carries its receive site's status in remote_receiver: the
 * receiver's squelch and signal come from there. */
typedef struct {
    char     role[12];          /* simplex duplex receiver transmitter         */
    char     callsign[16];
    char     version[16];
    uint32_t uptime_s;
    uint32_t now;               /* the node's clock, Unix seconds              */
    bool     has_rx, has_tx, has_reflector, has_repeater, has_rxlink;
    bool     block_rx, block_tx;
    /* the radio: what it transmits on, and what it listens on */
    double   tx_mhz, rx_mhz;    /* 0 = not known                               */
    char     ctcss_tx[8], ctcss_rx[8];
    /* the transmitter */
    bool     tx_keyed;
    char     tx_why[24];        /* "talkgroup audio", "announcement", ...      */
    uint32_t tx_keyed_ms;
    /* the receiver: its own, or the receive site's */
    bool     rx_known;          /* there is a receiver to tell of              */
    bool     cos;               /* the squelch is open                         */
    bool     open;              /* ...and what it hears is passed on           */
    char     gate[12];          /* the gate's state: "idle", ...               */
    int      rssi, rssi_lo, rssi_hi;    /* raw; -1 = not known                 */
    float    level_db;          /* the received audio, dBFS                    */
    /* a transmit site: its link to the receive site */
    bool     rx_site_up;
    /* the talkgroup */
    bool     link_up;
    uint32_t tg, home_tg;
    char     talker[16];        /* "" = nobody                                 */
    char     reflector[40];
    /* the last overs, newest first in the node's lists */
    char     net_call[16];      /* the last one from the talkgroup             */
    uint32_t net_t, net_ms;     /* when it ended (node's clock), how long      */
    uint32_t local_t, local_ms; /* the last opening of the squelch             */
    char     local_pass[12];    /* "passed", "blocked", ...                    */
    bool     monitor;           /* /monitor/rx and /monitor/tx are on          */
} tc_status_t;

/* The status's JSON into *st; false (and why) for what is not a status. */
bool tc_parse_status(const char *json, size_t len, tc_status_t *st, char *why, size_t why_cap);

/* {"rx":..,"tx":..}: the block's state. */
bool tc_parse_block(const char *json, size_t len, bool *rx, bool *tx);

/* An error's {"ok":false,"error":"..."} -- its words, or "". */
void tc_parse_error(const char *json, size_t len, char *out, size_t cap);

/* The raw RSSI on the S scale the node calibrated, as its own status page
 * reads it: rssi_lo S1, rssi_hi S9+40, twelve equal steps between (S1..S9,
 * then 10 dB each), on to S9+60 above. "S7", "S9+20"; "" when not known.
 * Returns the dB above S1 of that step (0..108), -1 when not known. */
int tc_smeter(int rssi, int lo, int hi, char *out, size_t cap);
/* The same in dBm, unrounded, as the radios' faces scale their S-meters:
 * S1 -121, six dB an S-point to S9 -73, rssi_hi S9+40 -33, S9+60 -13; down
 * to S0 -127, which is also what an unknown reads. */
float tc_smeter_dbm(int rssi, int lo, int hi);

/* ---- the audio: ws://node:8080/monitor/tx, 16-bit mono PCM at 16 kHz in
 * binary messages of about 100 ms (TailChaser's src/monitor.h) */

/* A frame from the server, unmasked: its opcode (0 more of a message, 1 text,
 * 2 binary, 8 close, 9 ping, 10 pong), whether it ends its message, and its
 * payload, from the frame's start. Returns the frame's whole length; 0 while
 * it is not all there; -1 for what a server may not send (masked, a reserved
 * bit or opcode, a control frame of over 125 bytes or in pieces, 16 MB). */
typedef struct {
    uint8_t op;
    bool    fin;
    size_t  off, len;
} tc_ws_frame_t;
long tc_ws_frame(const uint8_t *p, size_t n, tc_ws_frame_t *f);

/* A client's frame -- a pong, a close: under 126 bytes -- masked with mask
 * into out. Its length; 0 when it does not fit. */
size_t tc_ws_client_frame(uint8_t op, const uint8_t *payload, size_t n, const uint8_t mask[4], uint8_t *out,
                          size_t cap);

/* The handshake's answer: the length of its head (the frames follow it), 0
 * while it is not all in; its status in *code, 101 to go on. */
size_t tc_ws_answer(const char *buf, size_t n, int *code);

/* The loudest sample of 16-bit PCM, dBFS; -120 for silence. */
float tc_pcm_peak_db(const int16_t *pcm, size_t n);

/* An HTTP/1.x answer: its status code and where its body starts. False for
 * what is not one, or one cut short of its Content-Length. */
bool tc_http_answer(const char *buf, size_t len, int *code, const char **body, size_t *body_len);

/* "host", "host:port", "http://host:port/..." into an address; false for
 * what is no host. */
bool tc_parse_addr(const char *s, tc_addr_t *a);

/* A repeater's name on the face, as its owner gave it on the page: printable
 * ASCII, spaces trimmed and single, cut to cap - 1. */
void tc_clean_name(const char *in, char *out, size_t cap);
/* The name kept on a settings line after its address, a tab between; "". */
void tc_line_name(const char *line, char *out, size_t cap);

/* A frequency for the face: "438.800", "145.2875" for a 12.5 kHz channel,
 * or "" for 0. */
void tc_mhz(double mhz, char *out, size_t cap);

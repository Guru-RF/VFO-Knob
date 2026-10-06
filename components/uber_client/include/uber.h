/* The ubersdr firmware's receiver, beyond what radio.h says of any radio:
 * what the receiver has, the spots and voices on the dial's band, and the
 * receiver's SSTV pictures.
 *
 * The UberSDR itself is behind radio.h like any radio -- the dial tunes it,
 * the S-meter is its signal, the AGC's place its SNR and the gain's its noise
 * filter (OFF, NR2, RN2, NR4, whichever it offers) -- and it only receives.
 * A second receiver, a KiwiSDR or a Web-888, plays beside it as a web SDR
 * does beside a radio (components/sdr_rx). */
#ifndef UBER_H
#define UBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* What the receiver says of itself (its /api/description). */
typedef struct {
    bool known;                 /* read */
    char name[48], callsign[16], location[64], version[16];
    int  max_clients, available;
    int  max_session_s;         /* listening time per session; 0 = no limit */
    bool bypassed;              /* the password lifts the limits */
    bool spots;                 /* a DX cluster or a CW skimmer */
    bool voice;                 /* its voice-activity detector */
    bool sstv;                  /* its SSTV gallery */
} uber_info_t;
void uber_info(uber_info_t *out);

/* The receiver the client is on, where the knob lists more than one: the one
 * in use, or the next in turn while that cannot be reached -- its place in
 * the list (net_prov's) and its name as the dial has it, into `name`. -1,
 * and "", with a single receiver: nothing to name. */
int  uber_receiver(char *name, size_t cap);

/* A guest's time left, in seconds, as the receiver will end the session:
 * its limit on a session, counted from the session's first socket -- on
 * through a reconnect and every turn of the dial, as the receiver counts
 * it; only LISTEN AGAIN starts it again -- or its allowance for the day,
 * where that runs out sooner. In the last minute before an idle limit
 * would end the session, that minute. -1 where no limit applies (the
 * password, a LAN address, a receiver with none), before the session's
 * first socket, and once it has ended. `why`, unless NULL: 'S' the
 * session's limit, 'D' the day's, 'I' the idle one. */
int  uber_time_left(char *why);

/* What is on the dial's band that suits its mode, nearest the dial first
 * and then in frequency order: the DX cluster's spots; in a voice mode the
 * voices the receiver hears now, named or not; in CW the skimmer's spots.
 * A voice a few hundred hertz from a spot is that spot, heard. `seq` moves
 * on whenever they change; ages are counted from the receiver's own clock. */
#define UBER_SPOTS 24
typedef struct {
    char     call[12];          /* "" for a voice nobody has named */
    uint32_t hz;
    char     mode[5];           /* as it would be tuned: "usb", "cwu" */
    char     kind;              /* 'D' the cluster, 'C' the skimmer, 'V' a voice */
    bool     heard;             /* talking now */
    int8_t   snr;               /* the skimmer's, or the voice's; 0 = none */
    uint8_t  wpm;
    uint16_t age_s;             /* 0 for a voice: it is now */
} uber_spot_t;
int  uber_spots(uber_spot_t *out, int max, uint32_t *seq);

/* A spot's frequency and mode together, as one retune. */
void uber_tune_to(uint32_t hz, const char *mode);

/* SSTV pictures from the receiver's gallery, newest first: how many (-1: it
 * has none to show), and the one to fetch (-1 once the viewer is closed and
 * the glass shows none of them), with `gen`, the viewer's request: it moves
 * on each time the viewer says "fetching...", and a new one has the picture
 * offered again, the same one too. A picture comes decoded to RGB565, fitted
 * to the viewer; `seq` moves on for each. The caller says what became of
 * each, and the buffer the glass shows is not reused while it shows it. */
int  uber_sstv_count(void);
void uber_sstv_want(int idx, uint32_t gen);
typedef struct {
    const uint16_t *px;
    uint16_t w, h;
    int      idx, n;
    char     title[24];         /* "M2  3 / 24" */
    char     caption[48];       /* "14.230 USB  19:19  5 dB" */
    bool     failed;            /* could not be had: caption says why */
    uint32_t seq;
} uber_sstv_t;
bool uber_sstv_get(uber_sstv_t *out, uint32_t after_seq);
/* Picture `seq` dealt with: `taken`, it is on the glass now (or its failure
 * said there); not, the viewer let it go -- closed, or on another one. */
void uber_sstv_shown(uint32_t seq, bool taken);

/* For the knob's page: the gallery's file names, newest first, and where the
 * receiver is ("https://name.tunnel.ubersdr.org", "http://192.168.1.50:8080"),
 * for a browser to fetch them from it directly -- each has a _thumb.jpg
 * beside it. */
int  uber_sstv_files(char (*out)[72], int max);
void uber_base_url(char *out, size_t cap);

#endif /* UBER_H */

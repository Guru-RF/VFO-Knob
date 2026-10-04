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

/* The knob turned or the glass was touched: an UberSDR with an idle timeout
 * counts only that as listening, as its own page does. */
void uber_activity(void);

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
 * receiver is ("https://name.tunnel.ubersdr.org"), for a browser to fetch
 * them from it directly -- each has a _thumb.jpg beside it. */
int  uber_sstv_files(char (*out)[72], int max);
void uber_base_url(char *out, size_t cap);

#endif /* UBER_H */

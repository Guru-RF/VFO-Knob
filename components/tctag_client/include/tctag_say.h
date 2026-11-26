/* TailChaser Tag: each new over's callsign said out loud, in a voice made by
 * ElevenLabs, on the jack and a Bluetooth speaker (tctag_say.c). Off until
 * the page turns it on and gives the owner's ElevenLabs key. */
#ifndef TCTAG_SAY_H
#define TCTAG_SAY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool on;
    bool phonetic;              /* "Oscar November Six ...", else letter by letter */
    char voice[32];             /* an ElevenLabs voice's ID */
    char key[128];              /* the owner's ElevenLabs key: on the card, never shown */
} tctag_say_cfg_t;

typedef struct {
    bool     on, phonetic, key;  /* key: whether there is one */
    char     voice[32];
    char     state[48];          /* off, ready, asking, saying, said, or what went wrong */
    uint32_t said, asked, cached;   /* since the start: said; asked ElevenLabs; from the card */
} tctag_say_info_t;

/* What the page changes: what it sends, the rest kept. */
typedef struct {
    bool        have_on, on, have_phonetic, phonetic, key_clear;
    const char *voice;           /* NULL: kept; "": ElevenLabs' George again */
    const char *key;             /* NULL or "": kept */
} tctag_say_set_t;

void        tctag_say_start(void);
void        tctag_say_new(const char *call);              /* a new over: said, when on */
bool        tctag_say_test(const char *call);             /* the page's "Say it", on or off */
void        tctag_say_info(tctag_say_info_t *out);
const char *tctag_say_set(const tctag_say_set_t *s);      /* NULL, or why not */

#endif

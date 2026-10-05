/* What a device is to the knob: a headset or a speaker (bt_link_proto.h's
 * BTL_KIND_*). From what a scan heard of it -- its class of device, and the
 * services its scan answer lists -- and the verdicts this chip came to on
 * its own or was given by the configuration page, kept in NVS for the
 * devices met last. hfp.c decides with them. */
#ifndef KIND_H
#define KIND_H

#include <stdbool.h>
#include <stdint.h>

/* A device's kind from what a scan heard of it: its class and the services
 * its scan answer lists (BTL_SVC_*). A device that lists A2DP and no
 * hands-free or headset profile is a speaker, one that lists either of those
 * and no A2DP a headset; else an audio device whose class says loudspeaker,
 * portable or car audio, set-top box, hi-fi or display with loudspeaker is a
 * speaker, and everything else -- headphones too -- a headset. */
uint8_t kind_classify(uint32_t cod, uint8_t svc);
/* The services an EIR lists, as BTL_SVC_*: KNOWN for a complete list. */
uint8_t kind_eir_services(const uint8_t *eir);
/* Words for the log: "loudspeaker", "wearable headset", ... "audio/video";
 * a device that is not audio, its major class. */
const char *kind_minor_str(uint32_t cod);

/* The verdicts, kept for up to KIND_MAX devices, newest first. Any task:
 * the table has its own lock. */
#define KIND_MAX 8
/* hfp_init(), before the remembered device is read. */
void kind_load(void);
bool kind_lookup(const uint8_t bda[6], uint8_t *kind, uint8_t *why);
/* In RAM now, to NVS at the next kind_flush(); the same record again does
 * nothing. */
void kind_store(const uint8_t bda[6], uint8_t kind, uint8_t why);
/* It has held a call's audio open: a headset for sure, whatever drops of its
 * audio come later -- hfp.c never takes it for a speaker by them. Kept with
 * its verdict (kind and why, if it has none yet); only the page changes it. */
void kind_set_held(const uint8_t bda[6], uint8_t kind, uint8_t why);
bool kind_held(const uint8_t bda[6]);
void kind_forget(const uint8_t bda[6]);
/* The main loop, while no audio is open: to NVS, if anything changed. */
void kind_flush(void);

#endif /* KIND_H */

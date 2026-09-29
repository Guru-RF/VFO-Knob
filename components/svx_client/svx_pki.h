/* The station's key, its certificate request, the certificate the reflector
 * signs, and the reflector's CA bundle: SVXConnect-CLI's pki_dir, kept in NVS.
 *
 * The rules are the CLI's, because the reflector enforces them:
 *
 *  - The key is made once and never replaced. svxreflector binds a callsign
 *    to its key, and a new key for a known callsign looks like a hijack; a
 *    renewal is a new request from the SAME key.
 *  - A certificate the reflector sends is checked before it is stored: made
 *    out to this callsign, for this key, and not older than the one it would
 *    replace. A bad one must never overwrite one that may still work.
 *  - One the reflector has refused is remembered, and left out of the next
 *    login, which is what makes the reflector ask for a new request.
 *
 * Thread-safe: the reflector task writes, the web page reads.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

typedef enum {
    PKI_NO_KEY = 0,     /* nothing yet: a request starts with a key */
    PKI_NO_CERT,        /* a key, and perhaps a request, but no certificate */
    PKI_VALID,
    PKI_NOT_YET_VALID,  /* still presented: the clocks may just disagree */
    PKI_RENEW_DUE,      /* past two thirds of its life: the reflector renews it */
    PKI_EXPIRING,       /* in its last stretch */
    PKI_EXPIRED,
    PKI_REFUSED,        /* the reflector turned it down */
    PKI_WRONG_CALL,     /* made out to another callsign, or for another key */
} pki_state_t;

typedef struct {
    pki_state_t state;
    char     cn[40];             /* the certificate's subject, "" if none */
    char     issuer[64];
    int64_t  not_before, not_after;  /* Unix time, 0 if no certificate */
    bool     have_key, have_csr, have_ca;
    bool     pending;            /* a request is out; the knob keeps asking */
    int64_t  requested;          /* when this request first went, Unix time */
} pki_info_t;

typedef enum {
    PKI_PUSH_STORED = 0,         /* a new certificate: log in again with it */
    PKI_PUSH_EMPTY,              /* no certificate in it: not signed yet */
    PKI_PUSH_SAME,               /* the one already stored */
    PKI_PUSH_REJECTED,           /* not for us, or worse than what we have */
    PKI_PUSH_FAILED,             /* could not be stored */
} pki_push_t;

esp_err_t svx_pki_init(void);

/* A snapshot for the web page and the face. `call` is the configured
 * callsign: a certificate for another one is PKI_WRONG_CALL. */
void svx_pki_info(const char *call, time_t now, pki_info_t *out);

bool svx_pki_have_key(void);

/* RSA-2048. Takes seconds, yielding as it goes; call it from a task that can
 * wait, never from the web server. Refuses to replace an existing key. */
esp_err_t svx_pki_make_key(void);

/* The certificate request for `call`, from the stored key: rebuilt when it is
 * missing or was made for another callsign or address. */
esp_err_t svx_pki_make_csr(const char *call, const char *email);

/* Copies, NUL-terminated and malloc'd; the caller frees. NULL if absent. */
char *svx_pki_csr_pem(void);
char *svx_pki_key_pem(void);
char *svx_pki_crt_pem(void);
char *svx_pki_ca_pem(void);

/* Whether to present the certificate at the next login: there is one, it is
 * for this callsign and key, it has not expired and was not refused. */
bool svx_pki_present(const char *call, time_t now);

void svx_pki_store_ca(const char *pem);

/* A MsgClientCert body, checked and, if it is better, stored. */
pki_push_t svx_pki_store_cert(const uint8_t *body, size_t len,
                              const char *call, time_t now);

/* The reflector refused the certificate we presented. */
void svx_pki_refused(void);

/* The enrolment request: on until a certificate is stored or it is cancelled.
 * Remembered across reboots, like the CLI's files. */
void svx_pki_set_pending(bool on, time_t now);
bool svx_pki_pending(void);

/* Delete the key, the request and the certificate. The web page's last
 * resort, behind a warning: the reflector knows the callsign by its key. */
void svx_pki_forget(void);

/* True once SNTP has set the clock: before that, dates cannot be judged. */
static inline bool svx_time_known(time_t now) { return now > 1700000000; }

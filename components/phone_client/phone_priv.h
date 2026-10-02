/* Inside the phone client: what phone_web.c shares with phone_client.c. */
#ifndef PHONE_PRIV_H
#define PHONE_PRIV_H

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "sip.h"

#define PHONE_FAV_MAX 40

typedef struct {
    char name[24];
    char number[24];
} phone_fav_t;

/* The account; the password never leaves the knob (`pass` is "" in a copy
 * for the page, has_pass says whether there is one). */
void      phone_account_get(sip_account_t *a, bool *has_pass);
/* Saved, and registered again. A `pass` of "" keeps the one there is. */
esp_err_t phone_account_set(const sip_account_t *a);

int       phone_favs_get(phone_fav_t *out, int max);
esp_err_t phone_favs_set(const phone_fav_t *in, int n);

/* The telephone's state, as JSON, for the page and other programs. */
size_t    phone_state_json(char *buf, size_t cap);

extern const httpd_uri_t phone_web_uris[];
extern const size_t      phone_web_uris_n;

#endif /* PHONE_PRIV_H */

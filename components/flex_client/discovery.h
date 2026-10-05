/* The FlexRadios on the LAN, found by their discovery broadcast.
 *
 * Every radio broadcasts what it is, once a second, to UDP 4992 (see
 * flex_parse.h). The knob listens from the first time anything asks which
 * radios there are -- the dial's chooser, the configuration page -- once its
 * WiFi is up, whether or not a radio is configured: a new knob finds its
 * radio by itself. The radios already in the configured list (by address)
 * are not offered again; one chosen joins that list, in use. */
#ifndef FLEX_DISCOVERY_H
#define FLEX_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "flex_parse.h"

/* Listening from now on; a no-op until there is a network, and after. */
void      disc_start(void);

/* The radios heard lately, that the dial may offer: none already in the
 * configured list, and none while that list is full. */
int       disc_lan_count(void);
bool      disc_lan_get(int i, flex_disc_t *out);
/* ...its name on the dial: its nickname, else its model -- 15 characters. */
bool      disc_lan_name(int i, char *name, size_t cap);

/* That one into the configured list -- where a new knob has none yet, in
 * place of its empty entry -- and in use from the next boot. */
esp_err_t disc_lan_use(int i);

/* What has been heard since last asked, and what has gone quiet, to the
 * log: from a task, now and then. */
void      disc_news(void);

/* The configuration page's: GET /api/flexfound, every radio heard. */
size_t    disc_web_endpoints(const httpd_uri_t **out);

#endif /* FLEX_DISCOVERY_H */

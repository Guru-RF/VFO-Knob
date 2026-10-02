/* Google Contacts: the starred ones, as the telephone's favourites. See
 * contacts.c. */
#ifndef CONTACTS_H
#define CONTACTS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

void      contacts_load(void);
/* The user's own Google OAuth client, and where its sign-in returns (the
 * relay page). A secret of "" keeps the one there is. */
esp_err_t contacts_set_client(const char *client_id, const char *secret, const char *relay);
/* Google's sign-in page for this knob, reached at `knob` (its address as the
 * browser has it). False without a client. */
bool      contacts_signin_url(const char *knob, char *out, size_t cap);
/* What the relay page brought back -- the code, or Google's error -- with
 * the sign-in's `state`: exchanged, then a sync. False if it was no sign-in
 * of this knob's, or a refused one. */
bool      contacts_set_code(const char *code, const char *state, const char *error);
void      contacts_sync(void);
void      contacts_forget(void);
/* Often, from the phone task: starts a sync that is due. */
void      contacts_tick(bool online);
size_t    contacts_state_json(char *buf, size_t cap);

#endif /* CONTACTS_H */

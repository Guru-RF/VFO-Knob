/* SvxLink protocol 2's login: the reply to the reflector's challenge.
 *
 * The digest is HMAC-SHA1 keyed with the password's own bytes -- no hashing
 * first, no salt -- over the 20 bytes of the challenge, as SvxLink's
 * MsgAuthResponse computes it (ReflectorMsg.h, calcHMAC). The reflector does
 * the same with the password in its [PASSWORDS] and compares: the password
 * never crosses the network, though everything after the login does, in the
 * clear.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVX_PASS_MAX 64             /* characters: more than any reflector's */

/* HMAC-SHA1 of `msg` keyed with `key`: 0, or -1. */
int svx_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                  uint8_t out[20]);

/* The digest for `challenge` with `password`: 0, or -1 (no password). */
int svx_auth_digest(const char *password, const uint8_t challenge[20], uint8_t out[20]);

/* A password a reflector can have: 1 to SVX_PASS_MAX printable ASCII
 * characters, no space at either end (SvxLink's configuration reader drops
 * those, so a password with one could never match). */
bool svx_pass_ok(const char *password);

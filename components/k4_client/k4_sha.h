/* The K4's password proof: SHA-384 of the password, as 96 lowercase hex
 * characters (k4_parse.h's K4_AUTH_HEX with the NUL). Apart from k4_parse.c
 * because it needs a hash: mbedTLS's on the knob, OpenSSL's on the PC. */
#ifndef K4_SHA_H
#define K4_SHA_H

void k4_sha384_hex(const char *password, char out[97]);

#endif

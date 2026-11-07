/* SHA-384 of the K4's password, in hex: see k4_sha.h. */
#include "k4_sha.h"

#include <string.h>

#include "k4_parse.h"
#include "mbedtls/sha512.h"

void k4_sha384_hex(const char *password, char out[97])
{
    uint8_t d[48];
    /* is384 = 1: SHA-384, not SHA-512. */
    mbedtls_sha512((const unsigned char *)password, strlen(password), d, 1);
    k4_hex(out, d, sizeof d);
}

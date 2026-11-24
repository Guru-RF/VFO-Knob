/* SvxLink protocol 2's login digest. See svx_auth.h. */
#include "svx_auth.h"

#include <string.h>

#include "mbedtls/md.h"

int svx_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                  uint8_t out[20])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md) return -1;
    return mbedtls_md_hmac(md, key, key_len, msg, msg_len, out) == 0 ? 0 : -1;
}

int svx_auth_digest(const char *password, const uint8_t challenge[20], uint8_t out[20])
{
    if (!password || !password[0]) return -1;
    return svx_hmac_sha1((const uint8_t *)password, strlen(password), challenge, 20, out);
}

bool svx_pass_ok(const char *p)
{
    const size_t n = p ? strlen(p) : 0;
    if (n < 1 || n > SVX_PASS_MAX) return false;
    if (p[0] == ' ' || p[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] > 0x7E) return false;
    return true;
}

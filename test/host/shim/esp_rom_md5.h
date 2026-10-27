/* Host shim: the ROM's MD5, on mbedTLS's -- as the ROM's own is. */
#ifndef SHIM_ESP_ROM_MD5_H
#define SHIM_ESP_ROM_MD5_H
#include <stdint.h>
#include "mbedtls/md5.h"
typedef mbedtls_md5_context md5_context_t;
#define ESP_ROM_MD5_DIGEST_LEN 16
static inline void esp_rom_md5_init(md5_context_t *c)
{
    mbedtls_md5_init(c);
    mbedtls_md5_starts(c);
}
static inline void esp_rom_md5_update(md5_context_t *c, const void *b, uint32_t n)
{
    mbedtls_md5_update(c, b, n);
}
static inline void esp_rom_md5_final(uint8_t *d, md5_context_t *c)
{
    mbedtls_md5_finish(c, d);
    mbedtls_md5_free(c);
}
#endif

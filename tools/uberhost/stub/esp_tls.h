/* esp_tls.h for the PC: the names the client uses. There is no TLS here --
 * esp_tls_init() counts its calls and fails -- so a receiver in the clear is
 * seen never to touch esp-tls. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
typedef struct esp_tls esp_tls_t;
typedef struct {
    esp_err_t last_error;
    int       esp_tls_error_code;
    int       esp_tls_flags;
} esp_tls_last_error_t;
typedef esp_tls_last_error_t *esp_tls_error_handle_t;
typedef struct {
    int        timeout_ms;
    bool       is_plain_tcp;
    esp_err_t (*crt_bundle_attach)(void *conf);
} esp_tls_cfg_t;
#define ESP_ERR_ESP_TLS_BASE                     0x8000
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME  (ESP_ERR_ESP_TLS_BASE + 0x01)
#define ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET     (ESP_ERR_ESP_TLS_BASE + 0x02)
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST   (ESP_ERR_ESP_TLS_BASE + 0x06)
#define ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED     (ESP_ERR_ESP_TLS_BASE + 0x1A)
#define ESP_TLS_ERR_SSL_WANT_READ                (-0x6900)
#define ESP_TLS_ERR_SSL_WANT_WRITE               (-0x6880)
extern int uh_tls_inits;
esp_tls_t *esp_tls_init(void);
int        esp_tls_conn_new_sync(const char *host, int hostlen, int port, const esp_tls_cfg_t *cfg,
                                 esp_tls_t *tls);
ssize_t    esp_tls_conn_read(esp_tls_t *tls, void *data, size_t len);
ssize_t    esp_tls_conn_write(esp_tls_t *tls, const void *data, size_t len);
int        esp_tls_conn_destroy(esp_tls_t *tls);
esp_err_t  esp_tls_get_error_handle(esp_tls_t *tls, esp_tls_error_handle_t *eh);
esp_err_t  esp_tls_get_conn_sockfd(esp_tls_t *tls, int *fd);
ssize_t    esp_tls_get_bytes_avail(esp_tls_t *tls);

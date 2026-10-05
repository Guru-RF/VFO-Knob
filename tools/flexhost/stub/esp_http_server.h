/* esp_http_server.h for the PC: the names flex_client.c's web endpoints use,
 * never served here. */
#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct httpd_req { int unused; } httpd_req_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;
typedef void *httpd_handle_t;
esp_err_t httpd_resp_send_500(httpd_req_t *r);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v);
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s);
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *msg);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t n);
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500

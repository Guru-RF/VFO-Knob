/* Host shim: the part of ESP-IDF's esp_http_server that webcfg.c uses, on a
 * plain socket and a thread of its own (shim_httpd.c) -- one request at a
 * time, as the knob's server runs them, each on a connection that closes
 * after its answer. The port is SHIM_HTTP_PORT's, not the config's 80. */
#ifndef SHIM_ESP_HTTP_SERVER_H
#define SHIM_ESP_HTTP_SERVER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

#define ESP_ERR_HTTPD_BASE           0xb000
#define ESP_ERR_HTTPD_RESULT_TRUNC   (ESP_ERR_HTTPD_BASE + 6)
#define HTTPD_SOCK_ERR_FAIL          -1
#define HTTPD_SOCK_ERR_INVALID       -2
#define HTTPD_SOCK_ERR_TIMEOUT       -3

typedef enum { HTTP_DELETE, HTTP_GET, HTTP_HEAD, HTTP_POST, HTTP_PUT } httpd_method_t;
typedef enum {
    HTTPD_500_INTERNAL_SERVER_ERROR = 0,
    HTTPD_501_METHOD_NOT_IMPLEMENTED,
    HTTPD_505_VERSION_NOT_SUPPORTED,
    HTTPD_400_BAD_REQUEST,
    HTTPD_401_UNAUTHORIZED,
    HTTPD_403_FORBIDDEN,
    HTTPD_404_NOT_FOUND,
    HTTPD_405_METHOD_NOT_ALLOWED,
    HTTPD_408_REQ_TIMEOUT,
    HTTPD_411_LENGTH_REQUIRED,
    HTTPD_414_URI_TOO_LONG,
    HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE,
    HTTPD_ERR_CODE_MAX
} httpd_err_code_t;

#define HTTPD_200 "200 OK"
#define HTTPD_400 "400 Bad Request"
#define HTTPD_404 "404 Not Found"
#define HTTPD_500 "500 Internal Server Error"

typedef void *httpd_handle_t;
typedef struct httpd_req {
    httpd_handle_t handle;
    int            method;
    char           uri[512];
    size_t         content_len;
    void          *aux;
    void          *user_ctx;
} httpd_req_t;

typedef struct httpd_uri {
    const char     *uri;
    httpd_method_t  method;
    esp_err_t     (*handler)(httpd_req_t *r);
    void           *user_ctx;
} httpd_uri_t;

typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *req, httpd_err_code_t error);

typedef struct {
    unsigned task_priority;
    size_t   stack_size;
    int      core_id;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool     lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { .task_priority = 5, .stack_size = 4096, .core_id = 0, .server_port = 80, \
                                 .ctrl_port = 32768, .max_open_sockets = 7, .max_uri_handlers = 8,        \
                                 .max_resp_headers = 8, .backlog_conn = 5, .lru_purge_enable = false,     \
                                 .recv_wait_timeout = 5, .send_wait_timeout = 5 }

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error,
                                     httpd_err_handler_func_t handler_fn);
int       httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
int       httpd_req_to_sockfd(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);
esp_err_t httpd_resp_send_custom_err(httpd_req_t *req, const char *status, const char *msg);
#define HTTPD_RESP_USE_STRLEN -1
static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *str)
{
    return httpd_resp_send(r, str, str ? HTTPD_RESP_USE_STRLEN : 0);
}
static inline esp_err_t httpd_resp_send_404(httpd_req_t *r) { return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, NULL); }
static inline esp_err_t httpd_resp_send_500(httpd_req_t *r) { return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); }
#endif

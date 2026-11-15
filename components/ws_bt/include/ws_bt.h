/* A WebSocket client over a radio's Bluetooth serial link (bt_link.h's
 * bt_link_radio_*): the openx6bt firmware's TCI, to an X6100 or X6200 running
 * OpenX6X00, over RFCOMM from the knob's second chip instead of TCP.
 *
 * The part of esp_websocket_client that tci_client uses, the same way: the
 * events come on the client's own task -- CONNECTED once the upgrade is
 * answered, DATA for each frame or piece of one (pongs and the server's close
 * among them), DISCONNECTED when the link or the handshake fails or the link
 * drops, CLOSED when a close is answered, and FINISH as the task ends, after
 * any of them or after a stop. A stop or a close from the handler itself
 * takes effect when the handler returns.
 *
 * One client at a time: the second chip carries one radio's link. */
#ifndef WS_BT_H
#define WS_BT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* OpenX6X00's TCI service over Bluetooth: its record's class UUID, most
 * significant byte first, and its channel, found through SDP -- this only if
 * SDP finds none. */
#define WS_BT_OPENX6X00_UUID                                                                                  \
    { 0x1d, 0x21, 0xee, 0xa1, 0x57, 0xbf, 0x4f, 0x53, 0x9a, 0xd6, 0x72, 0x1d, 0x62, 0x4f, 0x2e, 0x67 }
#define WS_BT_OPENX6X00_SCN 22

typedef enum {
    WS_BT_EVENT_CONNECTED,
    WS_BT_EVENT_DATA,
    WS_BT_EVENT_DISCONNECTED,
    WS_BT_EVENT_CLOSED,
    WS_BT_EVENT_FINISH,
} ws_bt_event_id_t;

/* DATA: a frame's opcode, and its payload's piece from payload_offset on.
 * The names are esp_websocket_event_data_t's. */
typedef struct {
    const char *data_ptr;
    int         data_len;
    uint8_t     op_code;
    int         payload_len;
    int         payload_offset;
} ws_bt_event_data_t;

/* esp_event_handler_t's shape: base is NULL. */
typedef void (*ws_bt_handler_t)(void *arg, const char *base, int32_t id, void *data);

typedef struct {
    uint8_t     bda[6];               /* the radio */
    uint8_t     scn;                  /* its channel if SDP finds none: 0 none */
    const char *host;                 /* the upgrade's Host: anything the server takes */
    const char *path;                 /* "/" if NULL */
    const char *user_agent;           /* none if NULL */
    int         connect_timeout_ms;   /* the link, pairing included, and the upgrade's answer */
    int         ping_interval_sec;    /* a ping this often: 0 none */
    int         pingpong_timeout_sec; /* no pong this long after a ping: the link is lost. 0 never */
    int         task_prio, task_stack, task_core;
} ws_bt_config_t;

typedef struct ws_bt *ws_bt_handle_t;

ws_bt_handle_t ws_bt_init(const ws_bt_config_t *cfg);
esp_err_t      ws_bt_register_events(ws_bt_handle_t c, ws_bt_handler_t cb, void *arg);
/* Its task, which links, upgrades, and runs until the link ends. ESP_FAIL
 * while it still runs. */
esp_err_t      ws_bt_start(ws_bt_handle_t c);
/* Ends it now, without a close frame; waits for the task. Idempotent. */
esp_err_t      ws_bt_stop(ws_bt_handle_t c);
/* A close frame, its answer waited for up to timeout, then as ws_bt_stop. */
esp_err_t      ws_bt_close(ws_bt_handle_t c, TickType_t timeout);
esp_err_t      ws_bt_destroy(ws_bt_handle_t c);
/* One whole frame, or nothing: len, or -1 when it is not connected or the
 * link has no room for it within timeout. */
int            ws_bt_send_text(ws_bt_handle_t c, const char *data, int len, TickType_t timeout);
int            ws_bt_send_bin(ws_bt_handle_t c, const char *data, int len, TickType_t timeout);
/* A frame of len bytes would go at once. */
bool           ws_bt_has_room(ws_bt_handle_t c, size_t len);
bool           ws_bt_is_connected(ws_bt_handle_t c);
/* Why the last link ended, or failed, in words: "" none yet. */
const char    *ws_bt_why(ws_bt_handle_t c);

#endif /* WS_BT_H */

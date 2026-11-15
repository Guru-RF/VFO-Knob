/* A radio's serial link: an RFCOMM (SPP) link from this chip to a service of
 * the radio's, its bytes carried to and from the knob over the UART
 * (bt_link_proto.h's CMD_RADIO, EVT_RADIO, RS_DATA and RS_ACK). The knob's
 * firmware for an OpenX6X00 radio over Bluetooth speaks TCI through it. */
#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stdint.h>

/* After hfp_init(): Bluetooth is up. */
void radio_init(void);
/* The knob's CMD_RADIO, RS_DATA and RS_ACK. */
void radio_on_frame(uint8_t type, const uint8_t *p, uint16_t n);
/* The knob started afresh (its hello asks): a link it had is closed, and
 * where things are is news to it. */
void radio_knob_hello(void);
/* Radio mode is on: no headset nor speaker is called. */
bool radio_mode(void);
/* The radio a link is asked to: pairing with it is ours to accept, and asks
 * for General Bonding (radio.c's __wrap_bta_dm_co_io_req). */
bool radio_owns(const uint8_t *bda);
/* A pairing ended (AUTH_CMPL): the radio's refusal drops our old key. */
void radio_paired(const uint8_t *bda, int stat);
/* BTL_SVC_RADIO if a scan answer's EIR lists the radio's service. */
uint8_t radio_eir_svc(const uint8_t *eir);

#endif /* RADIO_H */

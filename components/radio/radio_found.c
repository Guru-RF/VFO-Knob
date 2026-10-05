/* The client finds no radios of its own unless it says otherwise: see
 * radio_found_count() in radio.h. The FlexRadio client overrides these. */
#include "radio.h"

__attribute__((weak)) int radio_found_count(void) { return 0; }

__attribute__((weak)) int radio_found_lan(void) { return 0; }

__attribute__((weak)) bool radio_found_get(int i, char *name, size_t cap)
{
    (void)i;
    if (name && cap) name[0] = 0;
    return false;
}

__attribute__((weak)) const char *radio_found_via(void) { return ""; }

__attribute__((weak)) int radio_found_active(void) { return -1; }

__attribute__((weak)) esp_err_t radio_found_use(int i)
{
    return i < 0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

/* Nor has a radio a transmit antenna apart from its receive one unless its
 * client says so (n_tx_ant): the FlexRadio's does. */
__attribute__((weak)) void radio_set_tx_antenna(uint8_t ant) { (void)ant; }

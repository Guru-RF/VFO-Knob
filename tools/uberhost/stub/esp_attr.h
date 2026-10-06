/* esp_attr.h for the PC: no PSRAM, no IRAM -- and RTC memory a plain static,
 * so every run is a power-on. */
#pragma once
#define EXT_RAM_BSS_ATTR
#define IRAM_ATTR
#define RTC_NOINIT_ATTR

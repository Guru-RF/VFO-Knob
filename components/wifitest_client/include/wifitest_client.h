/* The WiFi Test firmware's own, beside its radio.h (wifitest_client.c): what
 * it measured, for the face and the page, and the page's requests. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "wifitest.h"

/* A copy of all measured so far. From any task. */
void wifitest_snapshot(wt_t *out);

/* The page's: the channels scanned now (the pings paused the second or two
 * it takes); the counts begun again from nothing; the hosts pinged again
 * from the list saved (net_prov's). */
void wifitest_scan(void);
void wifitest_recount(void);
void wifitest_hosts_changed(void);

/* What it found, as text to paste into a report: into buf, with its length. */
size_t wifitest_report(char *buf, size_t n);

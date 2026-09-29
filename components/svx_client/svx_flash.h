/* NVS from the reflector task.
 *
 * The reflector task's stack is in PSRAM: internal RAM is what the WiFi
 * driver sends from, and with the 12 kB this task used to take out of it,
 * sends failed -- first the certificate login's large packets, and in the end
 * every one. But flash may only be read or written from a stack in internal
 * RAM, because the cache is off while it happens (IDF asserts it). Every NVS
 * access in this client therefore goes through here: run directly on an
 * internal stack, or on a short-lived helper task that has one. They are
 * rare -- at start, on enrolment, when a certificate arrives, once a day for
 * the talkgroup names.
 */
#pragma once

void svx_flash_safe(void (*fn)(void *), void *arg);

/* SPDX-License-Identifier: MIT
 * SVXConnect-CLI — Copyright (c) 2026 Joeri Van Dooren
 *
 * The knob's stand-in for the CLI's logger: the same four calls, written to
 * ESP_LOG under the tag "svx", so the modules shared with the CLI compile
 * unchanged. There is no log file and no level switch here; the knob's log is
 * the one on port 3333.
 */
#ifndef SVX_LOG_H
#define SVX_LOG_H

#include <stdarg.h>

void log_err (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_dbg (const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif

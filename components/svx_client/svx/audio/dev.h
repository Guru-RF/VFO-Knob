/* SPDX-License-Identifier: MIT
 * SVXConnect-CLI — Copyright (c) 2026 Joeri Van Dooren
 *
 * The audio format every part of the client agrees on. On the knob there is
 * no device layer to go with it: the speaker and the microphone are the
 * knob's own (components/audio), and both already run at this rate in the
 * svxconnect firmware.
 */
#ifndef SVX_DEV_H
#define SVX_DEV_H

#define SVX_RATE  16000     /* SvxLink's own rate: no resampling anywhere */
#define SVX_FRAME   320     /* 20 ms, one Opus packet                      */

#endif

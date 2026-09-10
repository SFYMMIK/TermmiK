/*
 * TermmiK
 * Copyright (C) 2026 SfymmiK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SOUND_H
#define SOUND_H

// Key sound effects (ALSA). All functions are safe no-ops when the sound
// feature is disabled at build time (DISABLE_SOUND) or misconfigured.
void sound_init(void);      // load the configured sample, open the device
void sound_play_key(void);  // play the key sound (call on user keypress)
void sound_cleanup(void);

#endif

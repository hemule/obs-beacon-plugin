/*
Beacon
Copyright (C) 2026 hemule <hemule@mayflower.work>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

/*
 * Thin C bridge between the C module entry point (plugin-main.c) and the C++
 * diagnostics components. Keeping obs_module_load/unload in C avoids fighting
 * the C linkage of the OBS module exports.
 */

#ifdef __cplusplus
extern "C" {
#endif

// Create and start the diagnostics components (called from obs_module_load).
void beacon_load(void);

// Stop and tear down the diagnostics components (called from obs_module_unload).
void beacon_unload(void);

#ifdef __cplusplus
}
#endif

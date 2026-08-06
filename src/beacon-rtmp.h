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
 * Registers the Beacon RTMP output (a vendored copy of OBS's stock rtmp_output,
 * id "beacon_rtmp_output") and the Beacon streaming service. The service's
 * get_output_type points OBS at our output, so selecting the Beacon service
 * routes streaming through it — the hook point for custom AMF connect data.
 *
 * Phase 0: no AMF injection yet — this only proves the vendored output builds
 * and streams identically to stock.
 */

#ifdef __cplusplus
extern "C" {
#endif

void beacon_rtmp_register(void);

#ifdef __cplusplus
}
#endif

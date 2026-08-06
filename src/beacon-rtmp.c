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

#include "beacon-rtmp.h"

#include <obs-module.h>
#include <util/bmem.h>

/* The vendored, patched OBS RTMP output (id = "beacon_rtmp_output"),
 * defined in src/rtmp-output/rtmp-stream.c. */
extern struct obs_output_info rtmp_output_info;

/* ------------------------------------------------------------------ */
/* Minimal Beacon streaming service                                    */
/* ------------------------------------------------------------------ */

struct beacon_service {
	char *server;
	char *key;
};

static const char *beacon_service_getname(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Beacon";
}

static void beacon_service_update(void *data, obs_data_t *settings)
{
	struct beacon_service *s = data;
	bfree(s->server);
	bfree(s->key);
	s->server = bstrdup(obs_data_get_string(settings, "server"));
	s->key = bstrdup(obs_data_get_string(settings, "key"));
}

static void *beacon_service_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(service);
	struct beacon_service *s = bzalloc(sizeof(struct beacon_service));
	beacon_service_update(s, settings);
	return s;
}

static void beacon_service_destroy(void *data)
{
	struct beacon_service *s = data;
	bfree(s->server);
	bfree(s->key);
	bfree(s);
}

static const char *beacon_service_connect_info(void *data, uint32_t type)
{
	struct beacon_service *s = data;
	switch (type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return s->server;
	case OBS_SERVICE_CONNECT_INFO_STREAM_KEY:
		return s->key;
	default:
		return NULL;
	}
}

static const char *beacon_service_get_url(void *data)
{
	return ((struct beacon_service *)data)->server;
}

static const char *beacon_service_get_key(void *data)
{
	return ((struct beacon_service *)data)->key;
}

static const char *beacon_service_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return "beacon_rtmp_output";
}

static const char *beacon_service_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "RTMP";
}

static obs_properties_t *beacon_service_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *ppts = obs_properties_create();
	obs_properties_add_text(ppts, "server", "Server URL", OBS_TEXT_DEFAULT);
	obs_properties_add_text(ppts, "key", "Stream Key", OBS_TEXT_PASSWORD);
	return ppts;
}

static struct obs_service_info beacon_service_info = {
	.id = "beacon_service",
	.get_name = beacon_service_getname,
	.create = beacon_service_create,
	.destroy = beacon_service_destroy,
	.update = beacon_service_update,
	.get_properties = beacon_service_properties,
	.get_url = beacon_service_get_url,
	.get_key = beacon_service_get_key,
	.get_output_type = beacon_service_output_type,
	.get_protocol = beacon_service_protocol,
	.get_connect_info = beacon_service_connect_info,
};

void beacon_rtmp_register(void)
{
	obs_register_output(&rtmp_output_info);
	obs_register_service(&beacon_service_info);
}

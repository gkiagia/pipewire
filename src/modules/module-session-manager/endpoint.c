/* PipeWire
 *
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <extensions/session-manager.h>

#include <spa/pod/filter.h>

#include "endpoint.h"
#include "client-endpoint.h"

#include <pipewire/private.h>

#define NAME "endpoint"

struct resource_data {
	struct endpoint *endpoint;
	struct spa_hook resource_listener;
	struct spa_hook object_listener;
	uint32_t n_subscribe_ids;
	uint32_t subscribe_ids[32];
};

#define pw_endpoint_resource(r,m,v,...)	\
	pw_resource_call(r,struct pw_endpoint_proxy_events,m,v,__VA_ARGS__)
#define pw_endpoint_resource_info(r,...)	\
	pw_endpoint_resource(r,info,0,__VA_ARGS__)
#define pw_endpoint_resource_param(r,...)	\
	pw_endpoint_resource(r,param,0,__VA_ARGS__)

static int endpoint_enum_params (void *object, int seq,
				uint32_t id, uint32_t start, uint32_t num,
				const struct spa_pod *filter)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint *this = data->endpoint;
	struct spa_pod *result;
	struct spa_pod *param;
	uint8_t buffer[1024];
	struct spa_pod_builder b = { 0 };
	uint32_t index;
	uint32_t next = start;
	uint32_t count = 0;

	while (true) {
		index = next++;
		if (index >= this->n_params)
			break;

		param = this->params[index];

		if (param == NULL || !spa_pod_is_object_id(param, id))
			continue;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (spa_pod_filter(&b, &result, param, filter) != 0)
			continue;

		pw_log_debug(NAME" %p: %d param %u", this, seq, index);

		pw_endpoint_resource_param(resource, seq, id, index, next, result);

		if (++count == num)
			break;
	}
	return 0;
}

static int endpoint_subscribe_params (void *object, uint32_t *ids, uint32_t n_ids)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	uint32_t i;

	n_ids = SPA_MIN(n_ids, SPA_N_ELEMENTS(data->subscribe_ids));
	data->n_subscribe_ids = n_ids;

	for (i = 0; i < n_ids; i++) {
		data->subscribe_ids[i] = ids[i];
		pw_log_debug(NAME" %p: resource %d subscribe param %u",
			data->endpoint, resource->id, ids[i]);
		endpoint_enum_params(resource, 1, ids[i], 0, UINT32_MAX, NULL);
	}
	return 0;
}

static int endpoint_set_param (void *object, uint32_t id, uint32_t flags,
				const struct spa_pod *param)
{
	struct pw_resource *resource = object;
	struct resource_data *data = pw_resource_get_user_data(resource);
	struct endpoint *this = data->endpoint;

	pw_client_endpoint_resource_set_param(this->client_ep->resource,
						id, flags, param);

	return 0;
}

static const struct pw_endpoint_proxy_methods methods = {
	PW_VERSION_ENDPOINT_PROXY_METHODS,
	.subscribe_params = endpoint_subscribe_params,
	.enum_params = endpoint_enum_params,
	.set_param = endpoint_set_param,
};

static void endpoint_notify_subscribed(struct endpoint *this,
					uint32_t index, uint32_t next)
{
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;
	struct spa_pod *param = this->params[index];
	uint32_t id;
	uint32_t i;

	if (!param || !spa_pod_is_object (param))
		return;

	id = SPA_POD_OBJECT_ID (param);

	spa_list_for_each(resource, &global->resource_list, link) {
		data = pw_resource_get_user_data(resource);
		for (i = 0; i < data->n_subscribe_ids; i++) {
			if (data->subscribe_ids[i] == id) {
				pw_endpoint_resource_param(resource, 1, id,
					index, next, param);
			}
		}
	}
}

int endpoint_update(struct endpoint *this,
			uint32_t change_mask,
			uint32_t n_params,
			const struct spa_pod **params,
			const struct pw_endpoint_info *info)
{
	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_PARAMS) {
		uint32_t i;
		size_t size = n_params * sizeof(struct spa_pod *);

		pw_log_debug(NAME" %p: update %d params", this, n_params);

		for (i = 0; i < this->n_params; i++)
			free(this->params[i]);
		this->params = realloc(this->params, size);
		if (size > 0 && !this->params) {
			this->n_params = 0;
			goto no_mem;
		}
		this->n_params = n_params;

		for (i = 0; i < this->n_params; i++) {
			this->params[i] = params[i] ? spa_pod_copy(params[i]) : NULL;
			endpoint_notify_subscribed(this, i, i+1);
		}
	}

	if (change_mask & PW_CLIENT_ENDPOINT_UPDATE_INFO) {
		struct pw_resource *resource;

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_STREAMS)
			this->info.n_streams = info->n_streams;

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION)
			this->info.session_id = info->session_id;

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS)
			pw_properties_update(this->props, info->props);

		if (info->change_mask & PW_ENDPOINT_CHANGE_MASK_PARAMS) {
			size_t size = info->n_params * sizeof(struct spa_param_info);

			this->info.params = realloc(this->info.params, size);
			if (size > 0 && !this->info.params) {
				this->info.n_params = 0;
				goto no_mem;
			}
			this->info.n_params = info->n_params;

			memcpy(this->info.params, info->params, size);
		}

		if (!this->info.name) {
			this->info.name = strdup(info->name);
			this->info.media_class = strdup(info->media_class);
			this->info.direction = info->direction;
			this->info.flags = info->flags;
		}

		this->info.change_mask = info->change_mask;
		spa_list_for_each(resource, &this->global->resource_list, link) {
			pw_endpoint_resource_info(resource, &this->info);
		}
		this->info.change_mask = 0;
	}

	return 0;

      no_mem:
	pw_log_error(NAME" can't update: no memory");
	pw_resource_error(this->client_ep->resource, -ENOMEM,
			NAME" can't update: no memory");
	return -ENOMEM;
}

static void endpoint_unbind(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = endpoint_unbind,
};

static int endpoint_bind(void *_data, struct pw_client *client,
			uint32_t permissions, uint32_t version, uint32_t id)
{
	struct endpoint *this = _data;
	struct pw_global *global = this->global;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	data->endpoint = this;
	pw_resource_add_listener(resource, &data->resource_listener,
				&resource_events, resource);
	pw_resource_add_object_listener(resource, &data->object_listener,
					&methods, resource);

	pw_log_debug(NAME" %p: bound to %d", this, resource->id);

	spa_list_append(&global->resource_list, &resource->link);

	this->info.change_mask = PW_ENDPOINT_CHANGE_MASK_ALL;
	pw_endpoint_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error(NAME" can't create resource: no memory");
	pw_resource_error(this->client_ep->resource, -ENOMEM,
			NAME" can't create resource: no memory");
	return -ENOMEM;
}

int endpoint_init(struct endpoint *this,
		struct client_endpoint *client_ep,
		struct pw_core *core,
		struct pw_properties *properties)
{
	const char *keys[] = {
		PW_KEY_FACTORY_ID,
		PW_KEY_CLIENT_ID,
		NULL
	};

	pw_log_debug(NAME" %p: new", this);

	this->client_ep = client_ep;
	this->props = properties;

	properties = pw_properties_new(NULL, NULL);
	if (!properties)
		goto no_mem;

	pw_properties_copy_keys(this->props, properties, keys);

	this->global = pw_global_new (core,
			PW_TYPE_INTERFACE_Endpoint,
			PW_VERSION_ENDPOINT_PROXY,
			properties, endpoint_bind, this);
	if (!this->global)
		goto no_mem;

	pw_properties_setf(this->props, PW_KEY_ENDPOINT_ID, "%u", this->global->id);

	this->info.version = PW_VERSION_ENDPOINT_INFO;
	this->info.id = this->global->id;
	this->info.props = &this->props->dict;

	pw_client_endpoint_resource_set_id(client_ep->resource, this->global->id);

	return pw_global_register(this->global);

      no_mem:
	pw_log_error(NAME" - can't create - out of memory");
	return -ENOMEM;
}

void endpoint_clear(struct endpoint *this)
{
	uint32_t i;

	pw_log_debug(NAME" %p: destroy", this);

	pw_global_destroy(this->global);

	for (i = 0; i < this->n_params; i++)
		free(this->params[i]);
	free(this->params);

	free(this->info.name);
	free(this->info.media_class);
	free(this->info.params);

	if (this->props)
		pw_properties_free(this->props);
}

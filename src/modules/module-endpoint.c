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

#include "config.h"

#include "module-endpoint/endpoint-impl.h"

struct pw_protocol *pw_protocol_native_ext_endpoint_init(struct pw_core *core);

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "George Kiagiadakis <george.kiagiadakis@collabora.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Allows clients to interract with session manager endpoints" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

struct factory_data {
	struct pw_factory *this;
	struct pw_properties *properties;

	struct pw_module *module;
	struct spa_hook module_listener;
};

static void *create_object(void *_data,
			   struct pw_resource *resource,
			   uint32_t type,
			   uint32_t version,
			   struct pw_properties *properties,
			   uint32_t new_id)
{
	void *result;
	struct pw_resource *endpoint_resource;
	struct pw_global *parent;
	struct pw_client *client = pw_resource_get_client(resource);

	endpoint_resource = pw_resource_new(client, new_id, PW_PERM_RWX, type, version, 0);
	if (endpoint_resource == NULL)
		goto no_mem;

	parent = pw_client_get_global(client);

	result = pw_client_endpoint_new(endpoint_resource, parent, properties);
	if (result == NULL)
		goto no_mem;

	return result;

      no_mem:
	pw_log_error("can't create endpoint");
	pw_resource_error(resource, -ENOMEM, "can't create endpoint: no memory");
	if (properties)
		pw_properties_free(properties);
	return NULL;
}

static const struct pw_factory_implementation impl_factory = {
	PW_VERSION_FACTORY_IMPLEMENTATION,
	.create_object = create_object,
};

static void module_destroy(void *data)
{
	struct factory_data *d = data;

	spa_hook_remove(&d->module_listener);

	if (d->properties)
		pw_properties_free(d->properties);

	pw_factory_destroy(d->this);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_factory *factory;
	struct factory_data *data;

	factory = pw_factory_new(core,
				 "client-endpoint",
				 PW_TYPE_INTERFACE_ClientEndpoint,
				 PW_VERSION_CLIENT_ENDPOINT,
				 NULL,
				 sizeof(*data));
	if (factory == NULL)
		return -ENOMEM;

	data = pw_factory_get_user_data(factory);
	data->this = factory;
	data->module = module;
	data->properties = properties;

	pw_log_debug("module-endpoint %p: new", module);

	pw_factory_set_implementation(factory, &impl_factory, data);
	pw_factory_register(factory, NULL, pw_module_get_global(module), NULL);

	pw_protocol_native_ext_endpoint_init(core);

	pw_module_add_listener(module, &data->module_listener, &module_events, data);
	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}

#include "server/wl_shm.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "util.h"
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#define SRV_SHM_VERSION 1

struct shm_buffer_data {
    int32_t width, height;
};

static void
shm_buffer_destroy(void *data) {
    struct shm_buffer_data *buffer_data = data;

    free(buffer_data);
}

static void
shm_buffer_size(void *data, uint32_t *width, uint32_t *height) {
    struct shm_buffer_data *buffer_data = data;

    *width = buffer_data->width;
    *height = buffer_data->height;
}

static const struct server_buffer_impl shm_buffer_impl = {
    .name = "shm",

    .destroy = shm_buffer_destroy,
    .size = shm_buffer_size,
};

static void
shm_pool_resource_destroy(struct wl_resource *resource) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    wl_shm_pool_destroy(shm_pool->remote);
    close(shm_pool->fd);
    free(shm_pool);
}

static void
shm_pool_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                       int32_t offset, int32_t width, int32_t height, int32_t stride,
                       uint32_t format) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    if (offset + (width * stride) > shm_pool->sz) {
        wl_resource_post_error(
            resource, WL_SHM_ERROR_INVALID_STRIDE,
            "create_buffer: invalid size: (%d + %dx%d, stride: %d) exceeds pool size (%d)",
            (int)offset, (int)width, (int)height, (int)stride, (int)shm_pool->sz);
        return;
    }

    uint32_t *fmt;
    bool ok = false;
    wl_array_for_each(fmt, shm_pool->formats) {
        if (*fmt == format) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT,
                               "create_buffer: invalid format %" PRIu32, format);
        return;
    }

    struct shm_buffer_data *buffer_data = calloc(1, sizeof(*buffer_data));
    if (!buffer_data) {
        wl_resource_post_no_memory(resource);
        return;
    }

    buffer_data->width = width;
    buffer_data->height = height;

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer_resource) {
        wl_resource_post_no_memory(resource);
        goto fail_resource;
    }

    struct wl_buffer *remote =
        wl_shm_pool_create_buffer(shm_pool->remote, offset, width, height, stride, format);
    if (!remote) {
        wl_resource_post_no_memory(buffer_resource);
        goto fail_remote;
    }

    struct server_buffer *buffer =
        server_buffer_create(buffer_resource, remote, &shm_buffer_impl, buffer_data);
    if (!buffer) {
        wl_resource_post_no_memory(buffer_resource);
        goto fail_buffer;
    }

    return;

fail_buffer:
    wl_buffer_destroy(remote);

fail_remote:
fail_resource:
    free(buffer_data);
}

static void
shm_pool_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
shm_pool_resize(struct wl_client *client, struct wl_resource *resource, int32_t size) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    if (size < shm_pool->sz) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
                               "cannot decrease size of wl_shm_pool (fd: %d, size: %d -> %d)",
                               (int)shm_pool->fd, (int)shm_pool->sz, (int)size);
        return;
    }

    shm_pool->sz = size;
    wl_shm_pool_resize(shm_pool->remote, size);
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void
shm_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
shm_create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t fd,
                int32_t size) {
    struct server_shm *shm = wl_resource_get_user_data(resource);

    struct server_shm_pool *shm_pool = calloc(1, sizeof(*shm_pool));
    if (!shm_pool) {
        wl_resource_post_no_memory(resource);
        return;
    }

    shm_pool->resource =
        wl_resource_create(client, &wl_shm_pool_interface, wl_resource_get_version(resource), id);
    if (!shm_pool->resource) {
        wl_resource_post_no_memory(resource);
        close(fd);
        free(shm_pool);
        return;
    }
    wl_resource_set_implementation(shm_pool->resource, &shm_pool_impl, shm_pool,
                                   shm_pool_resource_destroy);

    shm_pool->formats = shm->formats;
    shm_pool->fd = fd;
    shm_pool->sz = size;

    shm_pool->remote = wl_shm_create_pool(shm->remote, fd, size);
    if (!shm_pool->remote) {
        wl_resource_post_no_memory(shm_pool->resource);
        close(fd);
        free(shm_pool);
        return;
    }
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_SHM_VERSION);

    struct server_shm *shm = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &shm_impl, shm, shm_resource_destroy);

    wl_list_insert(&shm->objects, wl_resource_get_link(resource));
}

static void
on_shm_format(struct wl_listener *listener, void *data) {
    struct server_shm *shm = wl_container_of(listener, shm, on_shm_format);
    uint32_t *format = data;

    struct wl_resource *resource;
    wl_resource_for_each(resource, &shm->objects) {
        wl_shm_send_format(resource, *format);
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_shm *shm = wl_container_of(listener, shm, on_display_destroy);

    wl_global_destroy(shm->global);

    wl_list_remove(&shm->on_shm_format.link);
    wl_list_remove(&shm->on_display_destroy.link);

    free(shm);
}

struct server_shm *
server_shm_create(struct server *server) {
    struct server_shm *shm = calloc(1, sizeof(*shm));
    if (!shm) {
        ww_log(LOG_ERROR, "failed to allocate server_shm");
        return NULL;
    }

    shm->global =
        wl_global_create(server->display, &wl_shm_interface, SRV_SHM_VERSION, shm, on_global_bind);
    if (!shm->global) {
        ww_log(LOG_ERROR, "failed to allocate wl_shm global");
        free(shm);
        return NULL;
    }

    wl_list_init(&shm->objects);
    shm->remote = server->backend->shm;
    shm->formats = &server->backend->shm_formats;

    shm->on_shm_format.notify = on_shm_format;
    wl_signal_add(&server->backend->events.shm_format, &shm->on_shm_format);

    shm->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &shm->on_display_destroy);

    return shm;
}

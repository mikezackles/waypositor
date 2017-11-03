#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>

#include <gbm.h>
#include <xf86drm.h>

#include <wayland-client.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"

struct buffer;

struct display {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shell *shell;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    int req_dmabuf_immediate;
    struct gbm_device *dev;
    int node_fd;
};

struct buffer {
    struct wl_buffer *buffer;
    int busy;
    struct gbm_bo *bo;
    int dmabuf_fd;
};

#define NUM_BUFFERS 3

struct window {
    struct display *display;
    int width, height;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    struct buffer buffers[NUM_BUFFERS];
    struct buffer *prev_buffer;
    struct wl_callback *callback;
};

static int running = 1;

static void
redraw(void *data, struct wl_callback *callback, uint32_t time);

static void
buffer_release(void *data, struct wl_buffer */* buffer */)
{
  struct buffer *buf = static_cast<struct buffer *>(data);
  buf->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
  buffer_release
};

static void
fill_content(uint8_t *raw_data, int width, int height, int stride)
{
    int x = 0, y = 0;
    uint32_t *pix;

    for (y = 0; y < height; y++) {
        pix = (uint32_t *)(raw_data + y * stride);
        for (x = 0; x < width; x++)
            *pix++ = (0xff << 24) | ((x % 256) << 16) |
                 ((y % 256) << 8) | 0xf0;
    }
}

// Only called in non-immediate mode.
static void
create_succeeded(void *data,
         struct zwp_linux_buffer_params_v1 *params,
         struct wl_buffer *new_buffer)
{
  struct buffer *buffer = static_cast<struct buffer *>(data);

  buffer->buffer = new_buffer;
  wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);

  zwp_linux_buffer_params_v1_destroy(params);

  fprintf(stderr, "Succeed to create wl buffer from dmabuf\n");
}

// Only called in non-immediate mode.
static void
create_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
  struct buffer *buffer = static_cast<struct buffer *>(data);

  buffer->buffer = NULL;
  running = 0;

  zwp_linux_buffer_params_v1_destroy(params);

  fprintf(stderr, "Error: zwp_linux_buffer_params.create failed.\n");
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
  create_succeeded,
  create_failed
};

bool draw_content(struct buffer *buffer)
{
  // Solution1: using gbm_bo_map. But currently not implement in mesa for intel driver. Only for gallium drivers.
  if (0) {
    uint32_t stride = 0;
    void* map_info = nullptr;
    uint8_t *raw_data = static_cast<uint8_t *>(gbm_bo_map(buffer->bo, 0, 0, gbm_bo_get_width(buffer->bo),
        gbm_bo_get_height(buffer->bo),
        GBM_BO_TRANSFER_WRITE, &stride, &map_info));
    if (!map_info) {
      fprintf(stderr, "map_bo failed %p\n", (void *)raw_data);
      return false;
    }
    assert(raw_data);
    fill_content(raw_data, gbm_bo_get_width(buffer->bo), gbm_bo_get_height(buffer->bo), stride);
    gbm_bo_unmap(buffer->bo, map_info);
  }

  buffer->dmabuf_fd = gbm_bo_get_fd(buffer->bo);
  if (buffer->dmabuf_fd < 0) {
      fprintf(stderr, "error: dmabuf_fd < 0\n");
      return false;
  }

  // Solution2: generic system call 'mmap'. But bm_bo_get_fd always returns a read-only fd. So we can mmap for RO access but not for RW access.
  if (0) {
    size_t length = gbm_bo_get_width (buffer->bo) * gbm_bo_get_height(buffer->bo) ;
    void* map_data = mmap (0 /* addr */, length, PROT_WRITE, MAP_SHARED, buffer->dmabuf_fd, 0 /* offset */);

    if (map_data == MAP_FAILED) {
      fprintf(stderr, "map_bo failed\n");
      return false;
    }
    assert(map_data);
    fill_content(static_cast<uint8_t *>(map_data), gbm_bo_get_width(buffer->bo), gbm_bo_get_height(buffer->bo), gbm_bo_get_stride (buffer->bo));
    munmap(map_data, length);
  }

  // Solution3: Import the above dmabuf into an EGLImage. Then bind it to a
  // gl texture attached to a gl FBO. Then draw into it using gl.
  return true;
}

static int
create_dmabuf_buffer(struct display *display, struct buffer *buffer,
             int width, int height)
{
  struct zwp_linux_buffer_params_v1 *params;
  uint64_t modifier = 0;
  uint32_t format = GBM_FORMAT_ARGB8888;

  buffer->bo = gbm_bo_create(
      display->dev, width, height,
      format, /*GBM_BO_USE_LINEAR |*/ GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!buffer->bo) {
      fprintf(stderr, "gbm_bo_create failed\n");
      return -1;
  }

  uint32_t stride = gbm_bo_get_stride (buffer->bo);

  if (!draw_content(buffer))
    goto error;

  params = zwp_linux_dmabuf_v1_create_params(display->dmabuf);
  zwp_linux_buffer_params_v1_add(params,
                     buffer->dmabuf_fd,
                     0, /* plane_idx */
                     0, /* offset */
                     stride,
                     modifier >> 32,
                     modifier & 0xffffffff);
  if (display->req_dmabuf_immediate) {
    buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
                  width,
                  height,
                  format,
                  0 /* flags */);
    wl_buffer_add_listener(buffer->buffer, &buffer_listener, buffer);
  }
  else {
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buffer);
    zwp_linux_buffer_params_v1_create(params,
                  width,
                  height,
                  format,
                  0 /* flags */);

  }
  return 0;

error:
  gbm_bo_destroy(buffer->bo);
  return -1;
}

static void
handle_ping(void */*data*/, struct wl_shell_surface *shell_surface,
        uint32_t serial)
{
  wl_shell_surface_pong(shell_surface, serial);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
  handle_ping,
  nullptr /* handle_configure */,
  nullptr /* handle_popup_done */
};

static struct window *
create_window(struct display *display, int width, int height)
{
  struct window *window;
  int i;
  int ret;

  window = static_cast<struct window *>(calloc(1, sizeof(struct window)));
  if (!window)
    return NULL;

  window->callback = NULL;
  window->display = display;
  window->width = width;
  window->height = height;
  window->surface = wl_compositor_create_surface(display->compositor);

  window->shell_surface = wl_shell_get_shell_surface(display->shell, window->surface);

  wl_shell_surface_add_listener(window->shell_surface, &shell_surface_listener, nullptr);
  wl_shell_surface_set_toplevel(window->shell_surface);

  for (i = 0; i < NUM_BUFFERS; ++i) {
    ret = create_dmabuf_buffer(display, &window->buffers[i],
                                   width, height);
    if (ret < 0)
      return NULL;
  }

  return window;
}

static void
destroy_window(struct window *window)
{
  if (window->callback)
    wl_callback_destroy(window->callback);

  for (int i = 0; i < NUM_BUFFERS; i++) {
    if (!window->buffers[i].buffer)
      continue;

    wl_buffer_destroy(window->buffers[i].buffer);
    window->buffers[i].buffer = nullptr;
    gbm_bo_destroy(window->buffers[i].bo);
    window->buffers[i].bo = nullptr;
    close(window->buffers[i].dmabuf_fd);
  }

  wl_surface_destroy(window->surface);
  free(window);
}

static struct buffer *
window_next_buffer(struct window *window)
{
  for (int i = 0; i < NUM_BUFFERS; i++)
    if (!window->buffers[i].busy)
      return &window->buffers[i];

  return NULL;
}

static const struct wl_callback_listener frame_listener = {
    redraw
};

static void
redraw(void *data, struct wl_callback *callback, [[maybe_unused]] uint32_t time)
{
  struct window *window = static_cast<struct window *>(data);
  struct buffer *buffer;

  buffer = window_next_buffer(window);
  if (!buffer) {
    fprintf(stderr,
        !callback ? "Failed to create the first buffer.\n" :
        "All buffers busy at redraw(). Server bug?\n");
    abort();
  }

  wl_surface_attach(window->surface, buffer->buffer, 0, 0);
  wl_surface_damage(window->surface, 0, 0, window->width, window->height);

  if (callback)
    wl_callback_destroy(callback);

  window->callback = wl_surface_frame(window->surface);
  wl_callback_add_listener(window->callback, &frame_listener, window);
  wl_surface_commit(window->surface);
  buffer->busy = 1;
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
               uint32_t id, const char *interface, uint32_t /* version */)
{
  struct display *d = static_cast<struct display *>(data);

  if (strcmp(interface, "wl_compositor") == 0) {
      d->compositor =
          static_cast<struct wl_compositor *>(wl_registry_bind(registry,
                   id, &wl_compositor_interface, 1));
  } else if (strcmp(interface, "wl_shell") == 0) {
      d->shell =
          static_cast<struct wl_shell *>(wl_registry_bind(registry, id,
                   &wl_shell_interface, 1));
  } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
      int version = d->req_dmabuf_immediate ? 2 : 1;
      d->dmabuf = static_cast<struct zwp_linux_dmabuf_v1 *>(wl_registry_bind(registry,
                       id, &zwp_linux_dmabuf_v1_interface,
                       version));
  }
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  nullptr /* registry_handle_global_remove */
};

static struct display *
create_display(int is_immediate)
{
  struct display *display = static_cast<struct display *>(malloc(sizeof *display));
  if (display == NULL) {
      fprintf(stderr, "out of memory\n");
      exit(1);
  }
  display->display = wl_display_connect(NULL);
  assert(display->display);

  display->req_dmabuf_immediate = is_immediate;

  display->registry = wl_display_get_registry(display->display);
  wl_registry_add_listener(display->registry,
               &registry_listener, display);
  wl_display_roundtrip(display->display);
  if (display->dmabuf == NULL) {
      fprintf(stderr, "No zwp_linux_dmabuf global\n");
      exit(1);
  }

  wl_display_roundtrip(display->display);

  display->node_fd = open("/dev/dri/renderD128", O_RDWR);
  if (display->node_fd < 0) {
    fprintf(stderr, "Failed to open render node\n");
    exit(1);
  }

  drmVersionPtr version = drmGetVersion(display->node_fd);
  display->dev = gbm_create_device(display->node_fd);
  if (!display->dev) {
    close(display->node_fd);
    fprintf(stderr, "Error: drm device %s unsupported.\n", version->name);
    exit(1);
  }

  fprintf(stderr, "Using gbm device %s\n", version->name);

  return display;
}

static void
destroy_display(struct display *display)
{
  if (display->dev)
    gbm_device_destroy(display->dev);

  if (display->node_fd)
    close(display->node_fd);

  if (display->dmabuf)
    zwp_linux_dmabuf_v1_destroy(display->dmabuf);

  if (display->compositor)
    wl_compositor_destroy(display->compositor);

  wl_registry_destroy(display->registry);
  wl_display_flush(display->display);
  wl_display_disconnect(display->display);
  free(display);
}

static void
signal_int(int /* signum */)
{
    running = 0;
}

static int
is_import_mode_immediate(const char* c)
{
  if (!strcmp(c, "1"))
      return 1;
  else if (!strcmp(c, "0"))
      return 0;
  else
      exit(0);

  return 0;
}

int
main(int argc, char **argv)
{
  struct sigaction sigint;
  struct display *display;
  struct window *window;
  int is_immediate = 0;
  int ret = 0, i = 0;

  if (argc > 1) {
    static const char import_mode[] = "--import-immediate=";
    for (i = 1; i < argc; i++) {
      if (!strncmp(argv[i], import_mode,
             sizeof(import_mode) - 1)) {
        is_immediate = is_import_mode_immediate(argv[i]
                    + sizeof(import_mode) - 1);
      }
    }
  }

  display = create_display(is_immediate);
  window = create_window(display, 256, 256);
  if (!window)
    return 1;

  sigint.sa_handler = signal_int;
  sigemptyset(&sigint.sa_mask);
  sigint.sa_flags = SA_RESETHAND;
  sigaction(SIGINT, &sigint, NULL);

  /* Here we retrieve the linux-dmabuf objects if executed without immed,
   * or error */
  wl_display_roundtrip(display->display);

  if (!running)
    return 1;

  redraw(window, NULL, 0);

  while (running && ret != -1)
    ret = wl_display_dispatch(display->display);

  fprintf(stderr, "simple-dmabuf exiting\n");
  destroy_window(window);
  destroy_display(display);

  return 0;
}

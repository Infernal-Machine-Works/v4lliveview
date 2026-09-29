#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>

#ifndef V4LLIVEVIEW_BACKGROUND
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#include <cuse.h>
#include <linux/videodev2.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef V4LLIVEVIEW_BACKGROUND
#include <jpeglib.h>
#include <setjmp.h>
#endif

#define V4LLIVE_NAME "v4lliveview"
#define MAX_BUFFERS 32U
#ifndef V4LLIVEVIEW_PORTS
#define V4LLIVEVIEW_PORTS 1U
#endif

struct buffer {
    void *physical;
    void *virtual;
    size_t length;
    bool queued;
    bool ready;
    struct v4l2_buffer metadata;
};

struct client {
    struct buffer buffers[MAX_BUFFERS];
    unsigned int count;
    bool streaming;
    bool control_owner;
    struct port *port;
};

struct port {
    struct cuse_dev *device;
    struct client *client;
    unsigned int index;
    unsigned int slot;
};

struct application {
    int camera_fd;
    const char *camera_path;
    pthread_mutex_t camera_lock;
    pthread_mutex_t state_lock;
    pthread_cond_t state_changed;
    struct client *control_client;
    struct client *capture_client;
#ifdef V4LLIVEVIEW_SAFETY
    struct client *safety_client;
    uint64_t safety_interval_ns;
    uint64_t safety_next_ns;
#endif
    bool source_streaming;
    bool source_lost;
    bool stop;
    struct port ports[V4LLIVEVIEW_PORTS];

    uint8_t *display_yuyv;
    size_t display_size;
    unsigned int display_width;
    unsigned int display_height;
    unsigned int display_stride;
    uint32_t display_format;
    uint64_t display_generation;
    unsigned int capture_width, capture_height, capture_stride;
    uint32_t capture_format;
    uint64_t display_sequence;
    unsigned long display_drops;
    unsigned long client_drops;
};

static struct application app = {
    .camera_fd = -1,
    .camera_lock = PTHREAD_MUTEX_INITIALIZER,
    .state_lock = PTHREAD_MUTEX_INITIALIZER,
    .state_changed = PTHREAD_COND_INITIALIZER,
};

static volatile sig_atomic_t stop_requested;

#ifdef V4LLIVEVIEW_SAFETY
static uint64_t
monotonic_ns(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}
#endif

static int
xioctl(int fd, unsigned long request, void *argument)
{
    int result;

    do {
        result = ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

static void
request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static bool
is_supported_format(const struct v4l2_format *format)
{
    return format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
        (format->fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV ||
        format->fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG);
}

static bool
is_forwarded_control(uint32_t id)
{
    switch (id) {
    case V4L2_CID_EXPOSURE_AUTO:
    case V4L2_CID_EXPOSURE_ABSOLUTE:
    case V4L2_CID_EXPOSURE_AUTO_PRIORITY:
    case V4L2_CID_AUTO_WHITE_BALANCE:
    case V4L2_CID_WHITE_BALANCE_TEMPERATURE:
    case V4L2_CID_DO_WHITE_BALANCE:
        return true;
    default:
        return false;
    }
}

static const uint32_t forwarded_controls[] = {
    V4L2_CID_AUTO_WHITE_BALANCE,
    V4L2_CID_WHITE_BALANCE_TEMPERATURE,
    V4L2_CID_DO_WHITE_BALANCE,
    V4L2_CID_EXPOSURE_AUTO,
    V4L2_CID_EXPOSURE_ABSOLUTE,
    V4L2_CID_EXPOSURE_AUTO_PRIORITY,
};

static void
free_buffers_locked(struct client *client)
{
    unsigned int index;
    struct v4l2_requestbuffers release;

    for (index = 0; index < client->count; ++index) {
        if (client->buffers[index].physical != NULL)
            (void)munmap(client->buffers[index].physical,
                client->buffers[index].length);
        if (client->buffers[index].virtual != NULL)
            cuse_vmfree(client->buffers[index].virtual);
    }
    memset(client->buffers, 0, sizeof(client->buffers));
    client->count = 0;
    client->streaming = false;

    if (!client->control_owner)
        return;
    memset(&release, 0, sizeof(release));
    release.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    release.memory = V4L2_MEMORY_MMAP;
    (void)xioctl(app.camera_fd, VIDIOC_REQBUFS, &release);
}

static int
allocate_buffers(struct client *client, struct v4l2_requestbuffers *request)
{
    unsigned int index;

    if (request->memory != V4L2_MEMORY_MMAP ||
        request->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return EINVAL;
    if (request->count > MAX_BUFFERS)
        request->count = MAX_BUFFERS;

    if (!client->control_owner) {
        struct v4l2_format format;
        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        pthread_mutex_lock(&app.camera_lock);
        if (xioctl(app.camera_fd, VIDIOC_G_FMT, &format) == -1) {
            int error = errno;
            pthread_mutex_unlock(&app.camera_lock);
            return error;
        }
        pthread_mutex_unlock(&app.camera_lock);
        if (!is_supported_format(&format) || format.fmt.pix.sizeimage == 0)
            return EINVAL;
        for (unsigned int index = 0; index < request->count; ++index) {
            client->buffers[index].virtual = cuse_vmalloc(format.fmt.pix.sizeimage);
            if (client->buffers[index].virtual == NULL) {
                for (unsigned int cleanup = 0; cleanup < index; ++cleanup)
                    cuse_vmfree(client->buffers[cleanup].virtual);
                memset(client->buffers, 0, sizeof(client->buffers));
                return ENOMEM;
            }
            client->buffers[index].length = format.fmt.pix.sizeimage;
        }
        client->count = request->count;
        return 0;
    }

    pthread_mutex_lock(&app.camera_lock);
    free_buffers_locked(client);
    if (xioctl(app.camera_fd, VIDIOC_REQBUFS, request) == -1) {
        int error = errno;
        pthread_mutex_unlock(&app.camera_lock);
        return error;
    }
    if (request->count == 0 || request->count > MAX_BUFFERS) {
        pthread_mutex_unlock(&app.camera_lock);
        return EINVAL;
    }

    for (index = 0; index < request->count; ++index) {
        struct v4l2_buffer descriptor;
        void *virtual_buffer;
        void *physical_buffer;

        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        descriptor.memory = V4L2_MEMORY_MMAP;
        descriptor.index = index;
        if (xioctl(app.camera_fd, VIDIOC_QUERYBUF, &descriptor) == -1)
            goto failure;
        virtual_buffer = cuse_vmalloc(descriptor.length);
        if (virtual_buffer == NULL) {
            errno = ENOMEM;
            goto failure;
        }
        physical_buffer = mmap(NULL, descriptor.length, PROT_READ | PROT_WRITE,
            MAP_SHARED, app.camera_fd, descriptor.m.offset);
        if (physical_buffer == MAP_FAILED) {
            cuse_vmfree(virtual_buffer);
            goto failure;
        }
        client->buffers[index].physical = physical_buffer;
        client->buffers[index].virtual = virtual_buffer;
        client->buffers[index].length = descriptor.length;
    }
    client->count = request->count;
    pthread_mutex_unlock(&app.camera_lock);
    return 0;

failure:
    {
        int error = errno;
        unsigned int cleanup;
        for (cleanup = 0; cleanup < request->count; ++cleanup) {
            if (client->buffers[cleanup].physical != NULL)
                (void)munmap(client->buffers[cleanup].physical,
                    client->buffers[cleanup].length);
            if (client->buffers[cleanup].virtual != NULL)
                cuse_vmfree(client->buffers[cleanup].virtual);
        }
        memset(client->buffers, 0, sizeof(client->buffers));
        client->count = 0;
        request->count = 0;
        (void)xioctl(app.camera_fd, VIDIOC_REQBUFS, request);
        pthread_mutex_unlock(&app.camera_lock);
        return error;
    }
}

static int
start_source(struct client *client)
{
    unsigned int index;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format format;

    if (!client->control_owner || client->count == 0)
        return EINVAL;
    pthread_mutex_lock(&app.camera_lock);
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(app.camera_fd, VIDIOC_G_FMT, &format) == -1) {
        int error = errno;
        pthread_mutex_unlock(&app.camera_lock);
        return error;
    }
    if (!is_supported_format(&format) ||
        (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV && format.fmt.pix.bytesperline == 0)) {
        pthread_mutex_unlock(&app.camera_lock);
        return EINVAL;
    }
    for (index = 0; index < client->count; ++index) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (xioctl(app.camera_fd, VIDIOC_QBUF, &buffer) == -1) {
            int error = errno;
            pthread_mutex_unlock(&app.camera_lock);
            return error;
        }
    }
    if (xioctl(app.camera_fd, VIDIOC_STREAMON, &type) == -1) {
        int error = errno;
        pthread_mutex_unlock(&app.camera_lock);
        return error;
    }
    pthread_mutex_lock(&app.state_lock);
    client->streaming = true;
    app.source_streaming = true;
    app.source_lost = false;
    app.capture_width = format.fmt.pix.width;
    app.capture_height = format.fmt.pix.height;
    app.capture_stride = format.fmt.pix.bytesperline;
    app.capture_format = format.fmt.pix.pixelformat;
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
    pthread_mutex_unlock(&app.camera_lock);
    return 0;
}

static void
stop_source(struct client *client)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    pthread_mutex_lock(&app.camera_lock);
    if (app.source_streaming)
        (void)xioctl(app.camera_fd, VIDIOC_STREAMOFF, &type);
    pthread_mutex_lock(&app.state_lock);
    client->streaming = false;
    app.source_streaming = false;
    for (unsigned int index = 0; index < client->count; ++index) {
        client->buffers[index].queued = false;
        client->buffers[index].ready = false;
    }
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
    pthread_mutex_unlock(&app.camera_lock);
}

static void
copy_for_display(const uint8_t *pixels, size_t bytes, const struct v4l2_buffer *buffer)
{
    uint8_t *new_pixels;

    pthread_mutex_lock(&app.state_lock);
    if (app.capture_width == 0 || app.capture_height == 0 ||
        (buffer->flags & V4L2_BUF_FLAG_ERROR) || bytes == 0 ||
        (app.capture_format == V4L2_PIX_FMT_YUYV &&
        (app.capture_stride == 0 || bytes < (size_t)app.capture_stride * app.capture_height))) {
        pthread_mutex_unlock(&app.state_lock);
        return;
    }
    if (app.display_size != bytes) {
        new_pixels = realloc(app.display_yuyv, bytes);
        if (new_pixels == NULL) {
            ++app.display_drops;
            pthread_mutex_unlock(&app.state_lock);
            return;
        }
        app.display_yuyv = new_pixels;
        app.display_size = bytes;
    }
    memcpy(app.display_yuyv, pixels, bytes);
    app.display_sequence = buffer->sequence;
    app.display_width = app.capture_width;
    app.display_height = app.capture_height;
    app.display_stride = app.capture_stride;
    app.display_format = app.capture_format;
    ++app.display_generation;
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
}

static void
release_capture_client(struct client *client)
{
    pthread_mutex_lock(&app.state_lock);
    if (app.capture_client == client)
        app.capture_client = NULL;
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
}

#ifdef V4LLIVEVIEW_SAFETY
static bool
safety_secondary_ready_locked(struct client **client_out)
{
    struct client *client = app.ports[1].client;

    if (app.control_client != NULL || client == NULL || !client->streaming)
        return false;
    for (unsigned int index = 0; index < client->count; ++index) {
        if (client->buffers[index].queued && !client->buffers[index].ready) {
            *client_out = client;
            return true;
        }
    }
    return false;
}

static bool
deliver_frame_locked(struct client *client, const void *pixels, size_t bytes,
    const struct v4l2_buffer *source)
{
    for (unsigned int index = 0; index < client->count; ++index) {
        struct buffer *destination = &client->buffers[index];
        size_t copied;
        if (!destination->queued || destination->ready)
            continue;
        copied = bytes > destination->length ? destination->length : bytes;
        memcpy(destination->virtual, pixels, copied);
        destination->metadata = *source;
        destination->metadata.index = index;
        destination->metadata.bytesused = copied;
        if (copied != bytes)
            destination->metadata.flags |= V4L2_BUF_FLAG_ERROR;
        destination->ready = true;
        destination->queued = false;
        return true;
    }
    return false;
}

static void
safety_capture_once(void)
{
    struct client *secondary = NULL;
    struct v4l2_requestbuffers request;
    struct v4l2_buffer buffer;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    void *pixels = MAP_FAILED;
    size_t map_length = 0;
    int streaming = 0;

    pthread_mutex_lock(&app.state_lock);
    if (!safety_secondary_ready_locked(&secondary) ||
        monotonic_ns() < app.safety_next_ns) {
        pthread_mutex_unlock(&app.state_lock);
        return;
    }
    app.safety_client = secondary;
    pthread_mutex_unlock(&app.state_lock);

    memset(&request, 0, sizeof(request));
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = 1;
    pthread_mutex_lock(&app.camera_lock);
    if (xioctl(app.camera_fd, VIDIOC_REQBUFS, &request) == -1 || request.count == 0)
        goto done;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = request.type;
    buffer.memory = request.memory;
    if (xioctl(app.camera_fd, VIDIOC_QUERYBUF, &buffer) == -1)
        goto done;
    map_length = buffer.length;
    pixels = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
        app.camera_fd, buffer.m.offset);
    if (pixels == MAP_FAILED)
        goto done;
    if (xioctl(app.camera_fd, VIDIOC_QBUF, &buffer) == -1 ||
        xioctl(app.camera_fd, VIDIOC_STREAMON, &type) == -1)
        goto done;
    streaming = 1;
    {
        struct pollfd source_poll = { .fd = app.camera_fd, .events = POLLIN };
        if (poll(&source_poll, 1, 2000) <= 0)
            goto done;
    }
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = request.type;
    buffer.memory = request.memory;
    if (xioctl(app.camera_fd, VIDIOC_DQBUF, &buffer) == -1)
        goto done;
    pthread_mutex_lock(&app.state_lock);
    if (app.control_client == NULL && app.ports[1].client == secondary &&
        deliver_frame_locked(secondary, pixels, buffer.bytesused, &buffer))
        app.safety_next_ns = monotonic_ns() + app.safety_interval_ns;
    pthread_mutex_unlock(&app.state_lock);
    cuse_poll_wakeup();
done:
    if (streaming)
        (void)xioctl(app.camera_fd, VIDIOC_STREAMOFF, &type);
    if (pixels != MAP_FAILED)
        (void)munmap(pixels, map_length);
    memset(&request, 0, sizeof(request));
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    (void)xioctl(app.camera_fd, VIDIOC_REQBUFS, &request);
    pthread_mutex_unlock(&app.camera_lock);
    pthread_mutex_lock(&app.state_lock);
    if (app.safety_client == secondary)
        app.safety_client = NULL;
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
}

static void *
safety_worker(void *unused)
{
    (void)unused;
    while (!stop_requested) {
        safety_capture_once();
        {
            struct timespec interval = { .tv_sec = 0, .tv_nsec = 100000000L };
            (void)nanosleep(&interval, NULL);
        }
    }
    return NULL;
}
#endif

static void *
capture_worker(void *unused)
{
    (void)unused;
    while (!stop_requested) {
        struct pollfd source_poll;
        struct v4l2_buffer source_buffer;
        struct client *client;
        unsigned int target = MAX_BUFFERS;
        int poll_result;

        pthread_mutex_lock(&app.state_lock);
        while (!app.stop && !app.source_streaming)
            (void)pthread_cond_wait(&app.state_changed, &app.state_lock);
        if (app.stop) {
            pthread_mutex_unlock(&app.state_lock);
            break;
        }
        client = app.control_client;
        app.capture_client = client;
        pthread_mutex_unlock(&app.state_lock);
        if (client == NULL)
            continue;

        source_poll.fd = app.camera_fd;
        source_poll.events = POLLIN;
        source_poll.revents = 0;
        poll_result = poll(&source_poll, 1, 100);
        if (poll_result <= 0) {
            release_capture_client(client);
            continue;
        }

        memset(&source_buffer, 0, sizeof(source_buffer));
        source_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        source_buffer.memory = V4L2_MEMORY_MMAP;
        pthread_mutex_lock(&app.camera_lock);
        if (xioctl(app.camera_fd, VIDIOC_DQBUF, &source_buffer) == -1) {
            int error = errno;
            pthread_mutex_unlock(&app.camera_lock);
            if (error != EAGAIN) {
                pthread_mutex_lock(&app.state_lock);
                app.source_lost = true;
                app.source_streaming = false;
                pthread_mutex_unlock(&app.state_lock);
                cuse_poll_wakeup();
            }
            release_capture_client(client);
            continue;
        }
        pthread_mutex_unlock(&app.camera_lock);

        pthread_mutex_lock(&app.state_lock);
        if (source_buffer.index < client->count) {
            struct buffer *source = &client->buffers[source_buffer.index];
            for (unsigned int port_index = 0; port_index < V4LLIVEVIEW_PORTS;
                ++port_index) {
                struct client *recipient = app.ports[port_index].client;
                if (recipient == NULL || !recipient->streaming)
                    continue;
#ifdef V4LLIVEVIEW_SAFETY
                if (port_index == 1 && monotonic_ns() < app.safety_next_ns)
                    continue;
#endif
                target = MAX_BUFFERS;
                for (unsigned int index = 0; index < recipient->count; ++index) {
                    if (recipient->buffers[index].queued &&
                        !recipient->buffers[index].ready) {
                        target = index;
                        break;
                    }
                }
                if (target == MAX_BUFFERS) {
                    ++app.client_drops;
                    continue;
                }
                struct buffer *destination = &recipient->buffers[target];
                size_t bytes = source_buffer.bytesused;
                if (bytes > destination->length)
                    bytes = destination->length;
                memcpy(destination->virtual, source->physical, bytes);
                destination->metadata = source_buffer;
                destination->metadata.index = target;
                destination->metadata.bytesused = bytes;
                if (bytes != source_buffer.bytesused)
                    destination->metadata.flags |= V4L2_BUF_FLAG_ERROR;
                destination->ready = true;
                destination->queued = false;
#ifdef V4LLIVEVIEW_SAFETY
                if (port_index == 1)
                    app.safety_next_ns = monotonic_ns() + app.safety_interval_ns;
#endif
            }
        }
        pthread_mutex_unlock(&app.state_lock);

        if (source_buffer.index < client->count)
            copy_for_display(client->buffers[source_buffer.index].physical,
                source_buffer.bytesused, &source_buffer);

        pthread_mutex_lock(&app.camera_lock);
        (void)xioctl(app.camera_fd, VIDIOC_QBUF, &source_buffer);
        pthread_mutex_unlock(&app.camera_lock);
        release_capture_client(client);
        cuse_poll_wakeup();
    }
    return NULL;
}

static struct client *
current_client(struct cuse_dev *device)
{
    return cuse_dev_get_per_file_handle(device);
}

static int
copy_in(void *destination, void *source, size_t size)
{
    return cuse_copy_in(source, destination, (int)size) == 0 ? 0 : EFAULT;
}

static int
copy_out(const void *source, void *destination, size_t size)
{
    return cuse_copy_out(source, destination, (int)size) == 0 ? 0 : EFAULT;
}

static int
forward_ioctl(void *user_data, unsigned long command, size_t size,
    bool require_yuyv)
{
    uint8_t local[sizeof(struct v4l2_format)];
    int result;

    if (size > sizeof(local))
        return EINVAL;
    result = copy_in(local, user_data, size);
    if (result != 0)
        return result;
    if (require_yuyv && !is_supported_format((struct v4l2_format *)local))
        return EINVAL;
    pthread_mutex_lock(&app.camera_lock);
    result = xioctl(app.camera_fd, command, local);
    if (result == -1)
        result = errno;
    else
        result = 0;
    pthread_mutex_unlock(&app.camera_lock);
    if (result != 0)
        return result;
    if (require_yuyv && !is_supported_format((struct v4l2_format *)local))
        return EINVAL;
    return copy_out(local, user_data, size);
}

static int
enumerate_yuyv(void *user_data)
{
    struct v4l2_fmtdesc requested;
    struct v4l2_fmtdesc candidate;
    unsigned int wanted;
    unsigned int found = 0;
    int result;

    result = copy_in(&requested, user_data, sizeof(requested));
    if (result != 0)
        return result;
    if (requested.type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
        return EINVAL;
    wanted = requested.index;
    for (unsigned int source_index = 0;; ++source_index) {
        memset(&candidate, 0, sizeof(candidate));
        candidate.type = requested.type;
        candidate.index = source_index;
        pthread_mutex_lock(&app.camera_lock);
        result = xioctl(app.camera_fd, VIDIOC_ENUM_FMT, &candidate);
        if (result == -1)
            result = errno;
        pthread_mutex_unlock(&app.camera_lock);
        if (result != 0)
            return result;
        if (candidate.pixelformat != V4L2_PIX_FMT_YUYV &&
            candidate.pixelformat != V4L2_PIX_FMT_MJPEG)
            continue;
        if (found++ != wanted)
            continue;
        candidate.index = wanted;
        return copy_out(&candidate, user_data, sizeof(candidate));
    }
}

static int
enumerate_yuyv_sizes(void *user_data, unsigned long command, size_t size)
{
    union {
        struct v4l2_frmsizeenum sizes;
        struct v4l2_frmivalenum intervals;
    } storage;
    void *local = &storage;
    int result;

    if (size > sizeof(storage))
        return EINVAL;
    result = copy_in(local, user_data, size);
    if (result != 0)
        return result;
    if (((struct v4l2_frmsizeenum *)local)->pixel_format != V4L2_PIX_FMT_YUYV &&
        ((struct v4l2_frmsizeenum *)local)->pixel_format != V4L2_PIX_FMT_MJPEG)
        return EINVAL;
    pthread_mutex_lock(&app.camera_lock);
    result = xioctl(app.camera_fd, command, local);
    if (result == -1)
        result = errno;
    else
        result = 0;
    pthread_mutex_unlock(&app.camera_lock);
    if (result != 0)
        return result;
    return copy_out(local, user_data, size);
}

static int
query_forwarded_control(void *user_data, unsigned long command, size_t size)
{
    uint8_t local[sizeof(struct v4l2_query_ext_ctrl)];
    uint32_t requested;
    int result;

    result = copy_in(local, user_data, size);
    if (result != 0)
        return result;
    requested = *(uint32_t *)(void *)local;
    if ((requested & V4L2_CTRL_FLAG_NEXT_CTRL) == 0) {
        if (!is_forwarded_control(requested))
            return EINVAL;
        pthread_mutex_lock(&app.camera_lock);
        result = xioctl(app.camera_fd, command, local);
        if (result == -1)
            result = errno;
        else
            result = 0;
        pthread_mutex_unlock(&app.camera_lock);
        return result == 0 ? copy_out(local, user_data, size) : result;
    }

    requested &= ~(V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND);
    for (;;) {
        uint32_t candidate = 0;
        for (size_t index = 0; index < sizeof(forwarded_controls) /
            sizeof(forwarded_controls[0]); ++index) {
            uint32_t id = forwarded_controls[index];
            if (id > requested && (candidate == 0 || id < candidate))
                candidate = id;
        }
        if (candidate == 0)
            return EINVAL;
        *(uint32_t *)(void *)local = candidate;
        pthread_mutex_lock(&app.camera_lock);
        result = xioctl(app.camera_fd, command, local);
        if (result == -1)
            result = errno;
        else
            result = 0;
        pthread_mutex_unlock(&app.camera_lock);
        if (result == 0)
            return copy_out(local, user_data, size);
        requested = candidate;
    }
}

static int
forward_extended_controls(void *user_data, unsigned long command)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control local_controls[16];
    struct v4l2_ext_control *peer_controls;
    int result;

    result = copy_in(&controls, user_data, sizeof(controls));
    if (result != 0)
        return result;
    if (controls.count > sizeof(local_controls) / sizeof(local_controls[0]))
        return EINVAL;
    peer_controls = controls.controls;
    if (controls.count != 0) {
        result = copy_in(local_controls, controls.controls,
            controls.count * sizeof(local_controls[0]));
        if (result != 0)
            return result;
    }
    for (uint32_t index = 0; index < controls.count; ++index) {
        if (!is_forwarded_control(local_controls[index].id))
            return EINVAL;
    }
    controls.controls = local_controls;
    pthread_mutex_lock(&app.camera_lock);
    result = xioctl(app.camera_fd, command, &controls);
    if (result == -1)
        result = errno;
    else
        result = 0;
    pthread_mutex_unlock(&app.camera_lock);
    if (result != 0)
        return result;
    if (controls.count != 0) {
        result = copy_out(local_controls, peer_controls,
            controls.count * sizeof(local_controls[0]));
        if (result != 0)
            return result;
    }
    controls.controls = peer_controls;
    return copy_out(&controls, user_data, sizeof(controls));
}

static int
v4l_open_impl(struct cuse_dev *device, int flags)
{
    struct client *client;
    struct port *port;
    (void)flags;

    port = cuse_dev_get_priv0(device);
    if (port == NULL)
        return ENXIO;
    client = calloc(1, sizeof(*client));
    if (client == NULL)
        return ENOMEM;
    pthread_mutex_lock(&app.state_lock);
    if (port->client != NULL) {
        pthread_mutex_unlock(&app.state_lock);
        free(client);
        return EBUSY;
    }
    /* A fresh physical file handle prevents stale webcamd MMAP mappings
     * across released/reallocated buffer generations. Keep this handle for
     * the entire virtual capture session, never reopen it per frame. */
    if (app.control_client == NULL) {
        bool first_client = true;
        for (unsigned int index = 0; index < V4LLIVEVIEW_PORTS; ++index)
            if (app.ports[index].client != NULL)
                first_client = false;
        if (first_client) {
            if (app.camera_fd >= 0)
                (void)close(app.camera_fd);
            app.camera_fd = open(app.camera_path, O_RDWR | O_NONBLOCK);
            if (app.camera_fd < 0) {
                int error = errno;
                pthread_mutex_unlock(&app.state_lock);
                free(client);
                return error;
            }
        }
    }
    client->port = port;
#ifdef V4LLIVEVIEW_SAFETY
    /* Safety has fixed roles: slot 0 is the only camera-control endpoint. */
    if (port->slot == 0) {
        client->control_owner = true;
        app.control_client = client;
    }
#else
    if (app.control_client == NULL) {
        client->control_owner = true;
        app.control_client = client;
    }
#endif
    port->client = client;
    cuse_dev_set_per_file_handle(device, client);
    pthread_mutex_unlock(&app.state_lock);
    return 0;
}

static int
v4l_close_impl(struct cuse_dev *device, int flags)
{
    struct client *client;
    (void)flags;

    client = current_client(device);
    if (client == NULL)
        return 0;
    if (client->streaming)
        stop_source(client);
    pthread_mutex_lock(&app.state_lock);
    while (app.capture_client == client)
        (void)pthread_cond_wait(&app.state_changed, &app.state_lock);
#ifdef V4LLIVEVIEW_SAFETY
    while (app.safety_client == client)
        (void)pthread_cond_wait(&app.state_changed, &app.state_lock);
#endif
    pthread_mutex_unlock(&app.state_lock);
    pthread_mutex_lock(&app.camera_lock);
    free_buffers_locked(client);
    pthread_mutex_unlock(&app.camera_lock);
    pthread_mutex_lock(&app.state_lock);
    if (app.control_client == client)
        app.control_client = NULL;
    if (client->port != NULL && client->port->client == client)
        client->port->client = NULL;
    pthread_mutex_unlock(&app.state_lock);
    cuse_dev_set_per_file_handle(device, NULL);
    free(client);
    return 0;
}

static int
v4l_ioctl_impl(struct cuse_dev *device, int flags, unsigned long command,
    void *user_data)
{
    struct client *client;
    (void)flags;
    client = current_client(device);
    if (client == NULL)
        return ENXIO;

    switch (command) {
    case VIDIOC_QUERYCAP: {
        struct v4l2_capability capability;
        int result;
        memset(&capability, 0, sizeof(capability));
        pthread_mutex_lock(&app.camera_lock);
        result = xioctl(app.camera_fd, command, &capability);
        if (result == -1)
            result = errno;
        pthread_mutex_unlock(&app.camera_lock);
        if (result != 0)
            return result;
        (void)snprintf((char *)capability.driver, sizeof(capability.driver),
            "%s", V4LLIVE_NAME);
        (void)snprintf((char *)capability.card, sizeof(capability.card),
            "V4L live tee");
        capability.capabilities |= V4L2_CAP_VIDEO_CAPTURE |
            V4L2_CAP_STREAMING | V4L2_CAP_DEVICE_CAPS;
        capability.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        return copy_out(&capability, user_data, sizeof(capability));
    }
    case VIDIOC_G_INPUT:
        return forward_ioctl(user_data, command, sizeof(int), false);
    case VIDIOC_ENUMINPUT:
        return forward_ioctl(user_data, command, sizeof(struct v4l2_input), false);
    case VIDIOC_S_INPUT:
        if (!client->control_owner || client->streaming)
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(int), false);
    case VIDIOC_ENUM_FMT:
        return enumerate_yuyv(user_data);
    case VIDIOC_ENUM_FRAMESIZES:
        return enumerate_yuyv_sizes(user_data, command,
            sizeof(struct v4l2_frmsizeenum));
    case VIDIOC_ENUM_FRAMEINTERVALS:
        return enumerate_yuyv_sizes(user_data, command,
            sizeof(struct v4l2_frmivalenum));
    case VIDIOC_G_FMT:
    case VIDIOC_TRY_FMT:
    case VIDIOC_S_FMT:
        if (command == VIDIOC_S_FMT &&
            (!client->control_owner || client->streaming))
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(struct v4l2_format),
            command == VIDIOC_S_FMT || command == VIDIOC_TRY_FMT);
    case VIDIOC_G_PARM:
    case VIDIOC_S_PARM:
        if (command == VIDIOC_S_PARM &&
            (!client->control_owner || client->streaming))
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(struct v4l2_streamparm), false);
    case VIDIOC_CROPCAP:
        return forward_ioctl(user_data, command, sizeof(struct v4l2_cropcap), false);
    case VIDIOC_G_CROP:
    case VIDIOC_S_CROP:
        if (command == VIDIOC_S_CROP && !client->control_owner)
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(struct v4l2_crop), false);
    case VIDIOC_QUERYCTRL:
        return query_forwarded_control(user_data, command,
            sizeof(struct v4l2_queryctrl));
    case VIDIOC_QUERY_EXT_CTRL:
        return query_forwarded_control(user_data, command,
            sizeof(struct v4l2_query_ext_ctrl));
    case VIDIOC_G_CTRL:
    case VIDIOC_S_CTRL: {
        struct v4l2_control control;
        int result = copy_in(&control, user_data, sizeof(control));
        if (result != 0)
            return result;
        if (!is_forwarded_control(control.id))
            return EINVAL;
        if (command == VIDIOC_S_CTRL && !client->control_owner)
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(control), false);
    }
    case VIDIOC_G_EXT_CTRLS:
    case VIDIOC_S_EXT_CTRLS:
    case VIDIOC_TRY_EXT_CTRLS:
        if (command != VIDIOC_G_EXT_CTRLS && !client->control_owner)
            return EBUSY;
        return forward_extended_controls(user_data, command);
    case VIDIOC_REQBUFS: {
        struct v4l2_requestbuffers request;
        int result = copy_in(&request, user_data, sizeof(request));
        if (result != 0)
            return result;
        if (client->streaming)
            return EBUSY;
        if (request.count == 0) {
            pthread_mutex_lock(&app.camera_lock);
            free_buffers_locked(client);
            pthread_mutex_unlock(&app.camera_lock);
            return copy_out(&request, user_data, sizeof(request));
        }
        result = allocate_buffers(client, &request);
        if (result != 0)
            return result;
        return copy_out(&request, user_data, sizeof(request));
    }
    case VIDIOC_QUERYBUF: {
        struct v4l2_buffer buffer;
        int result = copy_in(&buffer, user_data, sizeof(buffer));
        if (result != 0)
            return result;
        if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            buffer.memory != V4L2_MEMORY_MMAP || buffer.index >= client->count)
            return EINVAL;
        buffer.length = client->buffers[buffer.index].length;
        buffer.m.offset = cuse_vmoffset(client->buffers[buffer.index].virtual);
        return copy_out(&buffer, user_data, sizeof(buffer));
    }
    case VIDIOC_QBUF: {
        struct v4l2_buffer buffer;
        int result = copy_in(&buffer, user_data, sizeof(buffer));
        if (result != 0)
            return result;
        if (buffer.type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
            buffer.memory != V4L2_MEMORY_MMAP || buffer.index >= client->count)
            return EINVAL;
        pthread_mutex_lock(&app.state_lock);
        client->buffers[buffer.index].queued = true;
        client->buffers[buffer.index].ready = false;
        pthread_mutex_unlock(&app.state_lock);
        return 0;
    }
    case VIDIOC_DQBUF: {
        struct v4l2_buffer request;
        int result = copy_in(&request, user_data, sizeof(request));
        if (result != 0)
            return result;
        pthread_mutex_lock(&app.state_lock);
        for (unsigned int index = 0; index < client->count; ++index) {
            if (client->buffers[index].ready) {
                request = client->buffers[index].metadata;
                client->buffers[index].ready = false;
                pthread_mutex_unlock(&app.state_lock);
                return copy_out(&request, user_data, sizeof(request));
            }
        }
        result = app.source_lost ? EIO : EAGAIN;
        pthread_mutex_unlock(&app.state_lock);
        return result;
    }
    case VIDIOC_STREAMON: {
        enum v4l2_buf_type type;
        int result = copy_in(&type, user_data, sizeof(type));
        if (result != 0)
            return result;
        if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
            return EINVAL;
        if (!client->control_owner) {
            client->streaming = true;
            return 0;
        }
        return start_source(client);
    }
    case VIDIOC_STREAMOFF: {
        enum v4l2_buf_type type;
        int result = copy_in(&type, user_data, sizeof(type));
        if (result != 0)
            return result;
        if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
            return EINVAL;
        if (!client->control_owner) {
            client->streaming = false;
            return 0;
        }
        stop_source(client);
        return 0;
    }
    default:
        return ENOTTY;
    }
}

static int
v4l_poll_impl(struct cuse_dev *device, int flags, int events)
{
    struct client *client;
    int result = 0;
    (void)flags;
    client = current_client(device);
    if (client == NULL)
        return POLLERR;
    pthread_mutex_lock(&app.state_lock);
    if (app.source_lost)
        result |= POLLERR;
    for (unsigned int index = 0; index < client->count; ++index) {
        if (client->buffers[index].ready) {
            result |= events & (POLLIN | POLLRDNORM);
            break;
        }
    }
    pthread_mutex_unlock(&app.state_lock);
    return result;
}

static int
errno_to_cuse(int error)
{
    switch (error) {
    case 0:
        return CUSE_ERR_NONE;
    case EBUSY:
        return CUSE_ERR_BUSY;
    case EAGAIN:
        return CUSE_ERR_WOULDBLOCK;
    case EINVAL:
    case ENOTTY:
        return CUSE_ERR_INVALID;
    case ENOMEM:
        return CUSE_ERR_NO_MEMORY;
    case EFAULT:
        return CUSE_ERR_FAULT;
    case EINTR:
        return CUSE_ERR_SIGNAL;
    case ENODEV:
    case ENXIO:
        return CUSE_ERR_NO_DEVICE;
    default:
        return CUSE_ERR_OTHER;
    }
}

static int
v4l_open(struct cuse_dev *device, int flags)
{
    return errno_to_cuse(v4l_open_impl(device, flags));
}

static int
v4l_close(struct cuse_dev *device, int flags)
{
    return errno_to_cuse(v4l_close_impl(device, flags));
}

static int
v4l_ioctl(struct cuse_dev *device, int flags, unsigned long command,
    void *user_data)
{
    return errno_to_cuse(v4l_ioctl_impl(device, flags, command, user_data));
}

static int
v4l_poll(struct cuse_dev *device, int flags, int events)
{
    int readiness = v4l_poll_impl(device, flags, events);
    int result = CUSE_POLL_NONE;

    if ((readiness & (POLLIN | POLLRDNORM)) != 0)
        result |= CUSE_POLL_READ;
    if ((readiness & POLLERR) != 0)
        result |= CUSE_POLL_ERROR;
    return result;
}

static const struct cuse_methods v4l_methods = {
    .cm_open = v4l_open,
    .cm_close = v4l_close,
    .cm_ioctl = v4l_ioctl,
    .cm_poll = v4l_poll,
};

static void *
cuse_worker(void *unused)
{
    (void)unused;
    while (!stop_requested && !app.stop) {
        int result = cuse_wait_and_process();
        if (result < 0 && !stop_requested)
            fprintf(stderr, "%s: CUSE event loop error %d\n", V4LLIVE_NAME,
                result);
    }
    return NULL;
}

#ifndef V4LLIVEVIEW_BACKGROUND
struct preview_jpeg_error {
    struct jpeg_error_mgr base;
    jmp_buf escape;
};

static void
preview_jpeg_fail(j_common_ptr decoder)
{
    struct preview_jpeg_error *error = (void *)decoder->err;
    longjmp(error->escape, 1);
}

static bool
jpeg_preview(const uint8_t *source, size_t bytes, unsigned int width,
    unsigned int height, unsigned int scale, unsigned int out_width,
    unsigned int out_height, uint32_t *destination)
{
    struct jpeg_decompress_struct decoder;
    struct preview_jpeg_error error;
    memset(&decoder, 0, sizeof(decoder));
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = preview_jpeg_fail;
    if (setjmp(error.escape)) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder, source, bytes);
    (void)jpeg_read_header(&decoder, TRUE);
    if (decoder.image_width != width || decoder.image_height != height) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    decoder.out_color_space = JCS_RGB;
    decoder.scale_num = 1;
    decoder.scale_denom = scale;
    (void)jpeg_start_decompress(&decoder);
    if (decoder.output_components != 3 || decoder.output_width != out_width ||
        decoder.output_height != out_height) {
        jpeg_destroy_decompress(&decoder);
        return false;
    }
    JSAMPARRAY row = (*decoder.mem->alloc_sarray)((j_common_ptr)&decoder,
        JPOOL_IMAGE, out_width * 3, 1);
    while (decoder.output_scanline < out_height) {
        unsigned int y = decoder.output_scanline;
        if (jpeg_read_scanlines(&decoder, row, 1) != 1) {
            jpeg_destroy_decompress(&decoder);
            return false;
        }
        for (unsigned int x = 0; x < out_width; ++x)
            destination[(size_t)y * out_width + x] =
                ((uint32_t)row[0][x * 3] << 16) |
                ((uint32_t)row[0][x * 3 + 1] << 8) | row[0][x * 3 + 2];
    }
    (void)jpeg_finish_decompress(&decoder);
    bool valid = decoder.err->num_warnings == 0;
    jpeg_destroy_decompress(&decoder);
    return valid;
}

static uint32_t
yuyv_pixel(uint8_t y, uint8_t u, uint8_t v)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    int red = (298 * c + 409 * e + 128) >> 8;
    int green = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int blue = (298 * c + 516 * d + 128) >> 8;

    if (red < 0) red = 0; else if (red > 255) red = 255;
    if (green < 0) green = 0; else if (green > 255) green = 255;
    if (blue < 0) blue = 0; else if (blue > 255) blue = 255;
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
}

static void
draw_latest(Display *display, Window window, GC graphics,
    uint8_t **scratch, size_t *scratch_size, XImage **image, bool redraw)
{
    static uint64_t last_generation;
    unsigned int width, height, stride, format, scale = 1;
    size_t bytes;
    uint64_t generation;
    uint8_t *source = NULL;

    pthread_mutex_lock(&app.state_lock);
    generation = app.display_generation;
    if (generation == 0 || generation == last_generation) {
        pthread_mutex_unlock(&app.state_lock);
        if (!redraw)
            return;
        goto paint_cached;
    }
    width = app.display_width;
    height = app.display_height;
    stride = app.display_stride;
    format = app.display_format;
    bytes = app.display_size;
    if (bytes && width && height && width <= 16384 && height <= 16384) {
        source = malloc(bytes);
        if (source != NULL)
            memcpy(source, app.display_yuyv, bytes);
    }
    pthread_mutex_unlock(&app.state_lock);
    if (source == NULL)
        goto paint_cached;

    /* Never decode or perform X11 calls while holding the acquisition lock. */
    while (scale < 8 && ((width + scale - 1) / scale > 960 ||
        (height + scale - 1) / scale > 720))
        scale *= 2;
    unsigned int out_width = (width + scale - 1) / scale;
    unsigned int out_height = (height + scale - 1) / scale;
    size_t output_size = (size_t)out_width * out_height * sizeof(uint32_t);
    uint32_t *pixels = malloc(output_size);
    bool valid = pixels != NULL;
    if (valid && format == V4L2_PIX_FMT_MJPEG) {
        valid = jpeg_preview(source, bytes, width, height, scale,
            out_width, out_height, pixels);
    } else if (valid && format == V4L2_PIX_FMT_YUYV && width % 2 == 0 &&
        stride >= width * 2 && bytes >= (size_t)stride * height) {
        for (unsigned int y = 0; y < out_height; ++y) {
            const uint8_t *row = source + (size_t)(y * scale) * stride;
            for (unsigned int x = 0; x < out_width; ++x) {
                unsigned int sx = x * scale;
                unsigned int pair = (sx / 2) * 4;
                pixels[(size_t)y * out_width + x] = yuyv_pixel(
                    row[sx * 2], row[pair + 1], row[pair + 3]);
            }
        }
    } else {
        valid = false;
    }
    free(source);
    last_generation = generation;
    if (!valid) {
        free(pixels);
        goto paint_cached;
    }

    XImage *replacement = XCreateImage(display,
        DefaultVisual(display, DefaultScreen(display)),
        DefaultDepth(display, DefaultScreen(display)), ZPixmap, 0,
        (char *)pixels, out_width, out_height, 32, 0);
    if (replacement == NULL) {
        free(pixels);
        goto paint_cached;
    }
    bool resized = *image == NULL || (*image)->width != (int)out_width ||
        (*image)->height != (int)out_height;
    if (*image != NULL) {
        (*image)->data = NULL;
        XDestroyImage(*image);
    }
    free(*scratch);
    *scratch = (uint8_t *)pixels;
    *scratch_size = output_size;
    *image = replacement;
    if (resized)
        XResizeWindow(display, window, out_width, out_height);

paint_cached:
    /* The decoded image belongs to the GUI, not the capture client's buffers.
     * Close, STREAMOFF, malformed new frames and Expose retain/repaint it. */
    if (*image != NULL) {
        XPutImage(display, window, graphics, *image, 0, 0, 0, 0,
            (*image)->width, (*image)->height);
        XFlush(display);
    }
}

static int
run_x11(void)
{
    Display *display;
    Window window;
    Atom delete_window;
    GC graphics;
    uint8_t *scratch = NULL;
    size_t scratch_size = 0;
    XImage *image = NULL;

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "%s: cannot open X display\n", V4LLIVE_NAME);
        return 1;
    }
    window = XCreateSimpleWindow(display, RootWindow(display, DefaultScreen(display)),
        0, 0, 640, 480, 0, BlackPixel(display, DefaultScreen(display)),
        BlackPixel(display, DefaultScreen(display)));
    char title[256];
    (void)snprintf(title, sizeof(title), "v4lliveview - %s", app.camera_path);
    XStoreName(display, window, title);
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    (void)XSetWMProtocols(display, window, &delete_window, 1);
    graphics = XCreateGC(display, window, 0, NULL);
    XMapWindow(display, window);

    while (!stop_requested) {
        bool redraw = false;
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == Expose || event.type == ConfigureNotify)
                redraw = true;
            if (event.type == ClientMessage &&
                (Atom)event.xclient.data.l[0] == delete_window) {
                stop_requested = 1;
                break;
            }
        }
        draw_latest(display, window, graphics, &scratch, &scratch_size, &image, redraw);
        struct timespec interval = { .tv_sec = 0, .tv_nsec = 16000000L };
        (void)nanosleep(&interval, NULL);
    }

    if (image != NULL) {
        image->data = NULL;
        XDestroyImage(image);
    }
    free(scratch);
    XFreeGC(display, graphics);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
#else
static int
run_background(void)
{
    while (!stop_requested) {
        struct timespec interval = { .tv_sec = 0, .tv_nsec = 100000000L };
        (void)nanosleep(&interval, NULL);
    }
    return 0;
}
#endif

static void
usage(const char *program)
{
#ifdef V4LLIVEVIEW_SAFETY
    fprintf(stderr, "usage: %s /dev/videoN [first_virtual_port] [interval_seconds]\n", program);
#else
    fprintf(stderr, "usage: %s /dev/videoN [first_virtual_port]\n", program);
#endif
}

int
main(int argc, char **argv)
{
    pthread_t capture_thread;
    pthread_t cuse_thread;
#ifdef V4LLIVEVIEW_SAFETY
    pthread_t safety_thread;
#endif
    struct sigaction action;
    int result;
    unsigned int port_index;
    unsigned long first_port = 0;

    if (argc < 2 || argc >
#ifdef V4LLIVEVIEW_SAFETY
        4
#else
        3
#endif
        ) {
        usage(argv[0]);
        return 64;
    }
    if (argc >= 3) {
        char *end;
        errno = 0;
        first_port = strtoul(argv[2], &end, 10);
        if (errno || end == argv[2] || *end || first_port > 65535 - V4LLIVEVIEW_PORTS) {
            usage(argv[0]);
            return 64;
        }
    }
#ifdef V4LLIVEVIEW_SAFETY
    app.safety_interval_ns = UINT64_C(10000000000);
    if (argc == 4) {
        char *end;
        unsigned long seconds;
        errno = 0;
        seconds = strtoul(argv[3], &end, 10);
        if (errno || end == argv[3] || *end || seconds == 0 ||
            seconds > UINT64_MAX / UINT64_C(1000000000)) {
            usage(argv[0]);
            return 64;
        }
        app.safety_interval_ns = (uint64_t)seconds * UINT64_C(1000000000);
    }
#endif
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGUSR1, &action, NULL);

    app.camera_path = argv[1];
    app.camera_fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (app.camera_fd == -1) {
        perror(argv[1]);
        return 1;
    }
    result = cuse_init();
    if (result != 0) {
        fprintf(stderr, "%s: CUSE is unavailable (%d); it must already be loaded\n",
            V4LLIVE_NAME, result);
        (void)close(app.camera_fd);
        return 1;
    }
    for (port_index = 0; port_index < V4LLIVEVIEW_PORTS; ++port_index) {
        char name[32];
        app.ports[port_index].index = port_index;
        app.ports[port_index].slot = port_index;
        (void)snprintf(name, sizeof(name), "video_live%u", (unsigned int)first_port + port_index);
        app.ports[port_index].device = cuse_dev_create(&v4l_methods,
            &app.ports[port_index], NULL, geteuid(), getegid(), 0660, "%s",
            name);
        if (app.ports[port_index].device == NULL) {
            fprintf(stderr, "%s: could not create /dev/%s\n", V4LLIVE_NAME,
                name);
            while (port_index > 0) {
                --port_index;
                cuse_dev_destroy(app.ports[port_index].device);
            }
            (void)cuse_uninit();
            (void)close(app.camera_fd);
            return 1;
        }
    }
    if (pthread_create(&capture_thread, NULL, capture_worker, NULL) != 0 ||
        pthread_create(&cuse_thread, NULL, cuse_worker, NULL) != 0
#ifdef V4LLIVEVIEW_SAFETY
        || pthread_create(&safety_thread, NULL, safety_worker, NULL) != 0
#endif
        ) {
        fprintf(stderr, "%s: cannot start worker threads\n", V4LLIVE_NAME);
        stop_requested = 1;
        app.stop = true;
        for (port_index = 0; port_index < V4LLIVEVIEW_PORTS; ++port_index)
            cuse_dev_destroy(app.ports[port_index].device);
        (void)cuse_uninit();
        (void)close(app.camera_fd);
        return 1;
    }
#ifdef V4LLIVEVIEW_BACKGROUND
    result = run_background();
#else
    result = run_x11();
#endif

    stop_requested = 1;
    pthread_mutex_lock(&app.state_lock);
    app.stop = true;
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
    for (port_index = 0; port_index < V4LLIVEVIEW_PORTS; ++port_index)
        cuse_dev_destroy(app.ports[port_index].device);
    (void)pthread_kill(cuse_thread, SIGUSR1);
    (void)pthread_join(capture_thread, NULL);
    (void)pthread_join(cuse_thread, NULL);
#ifdef V4LLIVEVIEW_SAFETY
    (void)pthread_join(safety_thread, NULL);
#endif
    (void)cuse_uninit();
    free(app.display_yuyv);
    (void)close(app.camera_fd);
    return result;
}

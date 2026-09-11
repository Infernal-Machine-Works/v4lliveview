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
#include <unistd.h>

#define V4LLIVE_NAME "v4lliveview"
#define V4LLIVE_NODE "video_live0"
#define MAX_BUFFERS 32U

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
};

struct application {
    int camera_fd;
    pthread_mutex_t camera_lock;
    pthread_mutex_t state_lock;
    pthread_cond_t state_changed;
    struct client *owner;
    bool source_streaming;
    bool source_lost;
    bool stop;
    struct cuse_dev *cuse_device;

    uint8_t *display_yuyv;
    size_t display_size;
    unsigned int display_width;
    unsigned int display_height;
    unsigned int display_stride;
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
is_yuyv(const struct v4l2_format *format)
{
    return format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
        format->fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV;
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

    if (client->count == 0)
        return EINVAL;
    pthread_mutex_lock(&app.camera_lock);
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
    struct v4l2_format format;
    uint8_t *new_pixels;

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    pthread_mutex_lock(&app.camera_lock);
    if (xioctl(app.camera_fd, VIDIOC_G_FMT, &format) == -1) {
        pthread_mutex_unlock(&app.camera_lock);
        return;
    }
    pthread_mutex_unlock(&app.camera_lock);
    if (!is_yuyv(&format))
        return;

    pthread_mutex_lock(&app.state_lock);
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
    app.display_width = format.fmt.pix.width;
    app.display_height = format.fmt.pix.height;
    app.display_stride = format.fmt.pix.bytesperline;
    app.display_sequence = buffer->sequence;
    pthread_cond_broadcast(&app.state_changed);
    pthread_mutex_unlock(&app.state_lock);
}

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
        client = app.owner;
        pthread_mutex_unlock(&app.state_lock);
        if (client == NULL)
            continue;

        source_poll.fd = app.camera_fd;
        source_poll.events = POLLIN;
        source_poll.revents = 0;
        poll_result = poll(&source_poll, 1, 100);
        if (poll_result <= 0)
            continue;

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
            continue;
        }
        pthread_mutex_unlock(&app.camera_lock);

        pthread_mutex_lock(&app.state_lock);
        for (unsigned int index = 0; index < client->count; ++index) {
            if (client->buffers[index].queued && !client->buffers[index].ready) {
                target = index;
                break;
            }
        }
        if (target != MAX_BUFFERS && source_buffer.index < client->count) {
            struct buffer *destination = &client->buffers[target];
            struct buffer *source = &client->buffers[source_buffer.index];
            size_t bytes = source_buffer.bytesused;
            if (bytes > destination->length)
                bytes = destination->length;
            memcpy(destination->virtual, source->physical, bytes);
            destination->metadata = source_buffer;
            destination->metadata.index = target;
            destination->metadata.bytesused = bytes;
            destination->ready = true;
            destination->queued = false;
        } else {
            ++app.client_drops;
        }
        pthread_mutex_unlock(&app.state_lock);

        if (source_buffer.index < client->count)
            copy_for_display(client->buffers[source_buffer.index].physical,
                source_buffer.bytesused, &source_buffer);

        pthread_mutex_lock(&app.camera_lock);
        (void)xioctl(app.camera_fd, VIDIOC_QBUF, &source_buffer);
        pthread_mutex_unlock(&app.camera_lock);
        cuse_poll_wakeup();
    }
    return NULL;
}

static struct client *
current_client(void)
{
    return cuse_dev_get_per_file_handle(app.cuse_device);
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
    if (require_yuyv && !is_yuyv((struct v4l2_format *)local))
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
    if (require_yuyv && !is_yuyv((struct v4l2_format *)local))
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
        if (candidate.pixelformat != V4L2_PIX_FMT_YUYV)
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
    uint8_t local[sizeof(struct v4l2_frmsizeenum)];
    int result;

    result = copy_in(local, user_data, size);
    if (result != 0)
        return result;
    if (((struct v4l2_frmsizeenum *)local)->pixel_format != V4L2_PIX_FMT_YUYV)
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
    (void)device;
    (void)flags;


    client = calloc(1, sizeof(*client));
    if (client == NULL)
        return ENOMEM;
    pthread_mutex_lock(&app.state_lock);
    if (app.owner != NULL) {
        pthread_mutex_unlock(&app.state_lock);
        free(client);
        return EBUSY;
    }
    app.owner = client;
    cuse_dev_set_per_file_handle(app.cuse_device, client);
    pthread_mutex_unlock(&app.state_lock);
    return 0;
}

static int
v4l_close_impl(struct cuse_dev *device, int flags)
{
    struct client *client;
    (void)device;
    (void)flags;

    client = current_client();
    if (client == NULL)
        return 0;
    if (client->streaming)
        stop_source(client);
    pthread_mutex_lock(&app.camera_lock);
    free_buffers_locked(client);
    pthread_mutex_unlock(&app.camera_lock);
    pthread_mutex_lock(&app.state_lock);
    if (app.owner == client)
        app.owner = NULL;
    pthread_mutex_unlock(&app.state_lock);
    cuse_dev_set_per_file_handle(app.cuse_device, NULL);
    free(client);
    return 0;
}

static int
v4l_ioctl_impl(struct cuse_dev *device, int flags, unsigned long command,
    void *user_data)
{
    struct client *client;
    (void)device;
    (void)flags;
    client = current_client();
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
        if (client->streaming && command == VIDIOC_S_FMT)
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(struct v4l2_format),
            command == VIDIOC_S_FMT || command == VIDIOC_TRY_FMT);
    case VIDIOC_G_PARM:
    case VIDIOC_S_PARM:
        if (client->streaming && command == VIDIOC_S_PARM)
            return EBUSY;
        return forward_ioctl(user_data, command, sizeof(struct v4l2_streamparm), false);
    case VIDIOC_CROPCAP:
        return forward_ioctl(user_data, command, sizeof(struct v4l2_cropcap), false);
    case VIDIOC_G_CROP:
    case VIDIOC_S_CROP:
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
        return forward_ioctl(user_data, command, sizeof(control), false);
    }
    case VIDIOC_G_EXT_CTRLS:
    case VIDIOC_S_EXT_CTRLS:
    case VIDIOC_TRY_EXT_CTRLS:
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
        return start_source(client);
    }
    case VIDIOC_STREAMOFF: {
        enum v4l2_buf_type type;
        int result = copy_in(&type, user_data, sizeof(type));
        if (result != 0)
            return result;
        if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
            return EINVAL;
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
    (void)device;
    (void)flags;
    client = current_client();
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
    uint8_t **scratch, size_t *scratch_size)
{
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    size_t source_size;
    size_t output_size;
    XImage *image;

    pthread_mutex_lock(&app.state_lock);
    width = app.display_width;
    height = app.display_height;
    stride = app.display_stride;
    source_size = app.display_size;
    if (width == 0 || height == 0 || app.display_yuyv == NULL) {
        pthread_mutex_unlock(&app.state_lock);
        return;
    }
    output_size = (size_t)width * height * sizeof(uint32_t);
    if (*scratch_size != output_size) {
        uint8_t *replacement = realloc(*scratch, output_size);
        if (replacement == NULL) {
            pthread_mutex_unlock(&app.state_lock);
            return;
        }
        *scratch = replacement;
        *scratch_size = output_size;
    }
    for (unsigned int row = 0; row < height; ++row) {
        const uint8_t *source = app.display_yuyv + (size_t)row * stride;
        uint32_t *destination = (uint32_t *)(void *)(*scratch) + (size_t)row * width;
        if ((size_t)(row + 1) * stride > source_size)
            break;
        for (unsigned int column = 0; column + 1 < width; column += 2) {
            uint8_t y0 = source[column * 2];
            uint8_t u = source[column * 2 + 1];
            uint8_t y1 = source[column * 2 + 2];
            uint8_t v = source[column * 2 + 3];
            destination[column] = yuyv_pixel(y0, u, v);
            destination[column + 1] = yuyv_pixel(y1, u, v);
        }
    }
    pthread_mutex_unlock(&app.state_lock);

    image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
        DefaultDepth(display, DefaultScreen(display)), ZPixmap, 0,
        (char *)*scratch, width, height, 32, 0);
    if (image == NULL)
        return;
    XPutImage(display, window, graphics, image, 0, 0, 0, 0, width, height);
    image->data = NULL;
    XDestroyImage(image);
    XFlush(display);
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

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "%s: cannot open X display\n", V4LLIVE_NAME);
        return 1;
    }
    window = XCreateSimpleWindow(display, RootWindow(display, DefaultScreen(display)),
        0, 0, 640, 480, 0, BlackPixel(display, DefaultScreen(display)),
        BlackPixel(display, DefaultScreen(display)));
    XStoreName(display, window, "v4lliveview");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    (void)XSetWMProtocols(display, window, &delete_window, 1);
    graphics = XCreateGC(display, window, 0, NULL);
    XMapWindow(display, window);

    while (!stop_requested) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ClientMessage &&
                (Atom)event.xclient.data.l[0] == delete_window) {
                stop_requested = 1;
                break;
            }
        }
        draw_latest(display, window, graphics, &scratch, &scratch_size);
        struct timespec interval = { .tv_sec = 0, .tv_nsec = 16000000L };
        (void)nanosleep(&interval, NULL);
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
    fprintf(stderr, "usage: %s /dev/videoN\n", program);
}

int
main(int argc, char **argv)
{
    pthread_t capture_thread;
    pthread_t cuse_thread;
    struct sigaction action;
    int result;

    if (argc != 2) {
        usage(argv[0]);
        return 64;
    }
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGUSR1, &action, NULL);

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
    app.cuse_device = cuse_dev_create(&v4l_methods, NULL, NULL, geteuid(),
        getegid(), 0660, V4LLIVE_NODE);
    if (app.cuse_device == NULL) {
        fprintf(stderr, "%s: could not create /dev/%s\n", V4LLIVE_NAME,
            V4LLIVE_NODE);
        (void)cuse_uninit();
        (void)close(app.camera_fd);
        return 1;
    }
    if (pthread_create(&capture_thread, NULL, capture_worker, NULL) != 0 ||
        pthread_create(&cuse_thread, NULL, cuse_worker, NULL) != 0) {
        fprintf(stderr, "%s: cannot start worker threads\n", V4LLIVE_NAME);
        stop_requested = 1;
        app.stop = true;
        cuse_dev_destroy(app.cuse_device);
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
    cuse_dev_destroy(app.cuse_device);
    (void)pthread_kill(cuse_thread, SIGUSR1);
    (void)pthread_join(capture_thread, NULL);
    (void)pthread_join(cuse_thread, NULL);
    (void)cuse_uninit();
    free(app.display_yuyv);
    (void)close(app.camera_fd);
    return result;
}

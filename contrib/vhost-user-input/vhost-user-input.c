/*
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "vhost-user-input.h"

static void vi_input_send(VuInput *vi, struct virtio_input_event *event)
{
    VuDev *dev = &vi->dev.parent;
    VuVirtq *vq = vu_get_queue(dev, 0);
    VuVirtqElement *elem;
    int i, len;

    /* queue up events ... */
    if (vi->qindex == vi->qsize) {
        vi->qsize++;
        vi->queue = g_realloc_n(vi->queue, vi->qsize, sizeof(vi->queue[0]));
    }
    vi->queue[vi->qindex++].event = *event;

    /* ... until we see a report sync ... */
    if (event->type != htole16(EV_SYN) ||
        event->code != htole16(SYN_REPORT)) {
        return;
    }

    /* ... then check available space ... */
    for (i = 0; i < vi->qindex; i++) {
        elem = vu_queue_pop(dev, vq, sizeof(VuVirtqElement));
        if (!elem) {
            while (--i >= 0) {
                vu_queue_unpop(dev, vq, vi->queue[i].elem, 0);
            }
            vi->qindex = 0;
            g_warning("virtio-input queue full");
            return;
        }
        vi->queue[i].elem = elem;
    }

    /* ... and finally pass them to the guest */
    for (i = 0; i < vi->qindex; i++) {
        elem = vi->queue[i].elem;
        len = iov_from_buf(elem->in_sg, elem->in_num,
                           0, &vi->queue[i].event, sizeof(virtio_input_event));
        vu_queue_push(dev, vq, elem, len);
        free(elem);
    }

    vu_queue_notify(&vi->dev.parent, vq);
    vi->qindex = 0;
}

static void
vi_evdev_watch(VuDev *dev, int condition, void *data)
{
    VuInput *vi = data;
    int fd = vi->evdevfd;

    g_debug("Got evdev condition %x", condition);

    struct virtio_input_event virtio;
    struct input_event evdev;
    int rc;

    for (;;) {
        rc = read(fd, &evdev, sizeof(evdev));
        if (rc != sizeof(evdev)) {
            break;
        }

        g_debug("input %d %d %d", evdev.type, evdev.code, evdev.value);

        virtio.type  = htole16(evdev.type);
        virtio.code  = htole16(evdev.code);
        virtio.value = htole32(evdev.value);
        vi_input_send(vi, &virtio);
    }
}


static void vi_handle_status(VuInput *vi, virtio_input_event *event)
{
    struct input_event evdev;
    struct timeval tval;
    int rc;

    if (gettimeofday(&tval, NULL)) {
        perror("vi_handle_status: gettimeofday");
        return;
    }

    evdev.input_event_sec = tval.tv_sec;
    evdev.input_event_usec = tval.tv_usec;
    evdev.type = le16toh(event->type);
    evdev.code = le16toh(event->code);
    evdev.value = le32toh(event->value);

    rc = write(vi->evdevfd, &evdev, sizeof(evdev));
    if (rc == -1) {
        perror("vi_host_handle_status: write");
    }
}

static void vi_handle_sts(VuDev *dev, int qidx)
{
    VuInput *vi = container_of(dev, VuInput, dev.parent);
    VuVirtq *vq = vu_get_queue(dev, qidx);
    virtio_input_event event;
    VuVirtqElement *elem;
    int len;

    g_debug("%s", G_STRFUNC);

    for (;;) {
        elem = vu_queue_pop(dev, vq, sizeof(VuVirtqElement));
        if (!elem) {
            break;
        }

        memset(&event, 0, sizeof(event));
        len = iov_to_buf(elem->out_sg, elem->out_num,
                         0, &event, sizeof(event));
        vi_handle_status(vi, &event);
        vu_queue_push(dev, vq, elem, len);
        free(elem);
    }

    vu_queue_notify(&vi->dev.parent, vq);
}

static void
vi_panic(VuDev *dev, const char *msg)
{
    g_critical("%s\n", msg);
    exit(EXIT_FAILURE);
}

static void
vi_queue_set_started(VuDev *dev, int qidx, bool started)
{
    VuInput *vi = container_of(dev, VuInput, dev.parent);
    VuVirtq *vq = vu_get_queue(dev, qidx);

    g_debug("queue started %d:%d", qidx, started);

    if (qidx == 1) {
        vu_set_queue_handler(dev, vq, started ? vi_handle_sts : NULL);
    }

    started = vu_queue_started(dev, vu_get_queue(dev, 0)) &&
        vu_queue_started(dev, vu_get_queue(dev, 1));

    if (started && !vi->evsrc) {
        vi->evsrc = vug_source_new(&vi->dev, vi->evdevfd,
                                   G_IO_IN, vi_evdev_watch, vi);
    }

    if (!started && vi->evsrc) {
        vug_source_destroy(vi->evsrc);
        vi->evsrc = NULL;
    }
}

static virtio_input_config *
vi_find_config(VuInput *vi, uint8_t select, uint8_t subsel)
{
    virtio_input_config *cfg;
    int i;

    for (i = 0; i < vi->config->len; i++) {
        cfg = &g_array_index(vi->config, virtio_input_config, i);
        if (select == cfg->select && subsel == cfg->subsel) {
            return cfg;
        }
    }

    return NULL;
}

static int vi_get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
    VuInput *vi = container_of(dev, VuInput, dev.parent);

    if (len > sizeof(*vi->sel_config)) {
        return -1;
    }

    if (vi->sel_config) {
        memcpy(config, vi->sel_config, len);
    } else {
        memset(config, 0, len);
    }

    return 0;
}

static int vi_set_config(VuDev *dev, const uint8_t *data,
                         uint32_t offset, uint32_t size,
                         uint32_t flags)
{
    VuInput *vi = container_of(dev, VuInput, dev.parent);
    virtio_input_config *config = (virtio_input_config *)data;

    vi->sel_config = vi_find_config(vi, config->select, config->subsel);

    return 0;
}

static const VuDevIface vuiface = {
    .queue_set_started = vi_queue_set_started,
    .get_config = vi_get_config,
    .set_config = vi_set_config,
};

static void
vi_bits_config(VuInput *vi, int type, int count)
{
    virtio_input_config bits;
    int rc, i, size = 0;

    memset(&bits, 0, sizeof(bits));
    rc = ioctl(vi->evdevfd, EVIOCGBIT(type, count / 8), bits.u.bitmap);
    if (rc < 0) {
        return;
    }

    for (i = 0; i < count / 8; i++) {
        if (bits.u.bitmap[i]) {
            size = i + 1;
        }
    }
    if (size == 0) {
        return;
    }

    bits.select = VIRTIO_INPUT_CFG_EV_BITS;
    bits.subsel = type;
    bits.size   = size;
    g_array_append_val(vi->config, bits);
}

static bool need_grab(VuInput *vi)
{
    return vi->ikind == VU_KEYBOARD || vi->ikind == VU_MOUSE;
}

static void vi_abs_config(VuInput *vi, int axis)
{
    virtio_input_config config;
    struct input_absinfo absinfo;
    int rc;

    rc = ioctl(vi->evdevfd, EVIOCGABS(axis), &absinfo);
    if (rc < 0) {
        return;
    }

    memset(&config, 0, sizeof(config));
    config.select = VIRTIO_INPUT_CFG_ABS_INFO;
    config.subsel = axis;
    config.size   = sizeof(struct virtio_input_absinfo);

    config.u.abs.min  = cpu_to_le32(absinfo.minimum);
    config.u.abs.max  = cpu_to_le32(absinfo.maximum);
    config.u.abs.fuzz = cpu_to_le32(absinfo.fuzz);
    config.u.abs.flat = cpu_to_le32(absinfo.flat);
    config.u.abs.res  = cpu_to_le32(absinfo.resolution);

    g_array_append_val(vi->config, config);
}

gpointer add_device(gpointer arg)
{
    vu_thread_data *ds = (vu_thread_data*)arg;
    VuInput *vi = &ds->vi;
    virtio_input_config id, *abs;
    struct input_id ids;
    int rc, ver, fd, axis;
    uint8_t byte;

    int evdev = ds->fd;
    char *socket_path = ds->socket_path;

    vi->evdevfd = evdev;
    if (vi->evdevfd < 0) {
        g_printerr("Failed to open evdev: %s\n", g_strerror(errno));
        return NULL;
    }

    rc = ioctl(vi->evdevfd, EVIOCGVERSION, &ver);
    if (rc < 0) {
        g_printerr("%d: is not an evdev device\n", evdev);
        return NULL;
    }

    vi->config = g_array_new(false, false, sizeof(virtio_input_config));
    memset(&id, 0, sizeof(id));
    if (ioctl(vi->evdevfd, EVIOCGNAME(sizeof(id.u.string) - 1),
              id.u.string) < 0) {
        g_printerr("Failed to get evdev name: %s\n", g_strerror(errno));
        return NULL;
    }
    id.select = VIRTIO_INPUT_CFG_ID_NAME;
    id.size = strlen(id.u.string);
    g_array_append_val(vi->config, id);

    if (ioctl(vi->evdevfd, EVIOCGID, &ids) == 0) {
        memset(&id, 0, sizeof(id));
        id.select = VIRTIO_INPUT_CFG_ID_DEVIDS;
        id.size = sizeof(struct virtio_input_devids);
        id.u.ids.bustype = cpu_to_le16(ids.bustype);
        id.u.ids.vendor  = cpu_to_le16(ids.vendor);
        id.u.ids.product = cpu_to_le16(ids.product);
        id.u.ids.version = cpu_to_le16(ids.version);
        g_array_append_val(vi->config, id);
    }

    vi_bits_config(vi, EV_KEY, KEY_CNT);
    vi_bits_config(vi, EV_REL, REL_CNT);
    vi_bits_config(vi, EV_ABS, ABS_CNT);
    vi_bits_config(vi, EV_MSC, MSC_CNT);
    vi_bits_config(vi, EV_SW,  SW_CNT);
    g_debug("config length: %u", vi->config->len);

    abs = vi_find_config(vi, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS);
    if (abs) {
        for (int i = 0; i < abs->size; i++) {
            byte = abs->u.bitmap[i];
            axis = 8 * i;
            while (byte) {
                if (byte & 1) {
                    vi_abs_config(vi, axis);
                }
                axis++;
                byte >>= 1;
            }
        }
    }

    if (need_grab(vi)) {
        rc = ioctl(vi->evdevfd, EVIOCGRAB, 1);
        if (rc < 0) {
            g_printerr("Failed to grab device\n");
            return NULL;
        }
    }

    if (socket_path) {
        int lsock = unix_listen(socket_path, &error_fatal);
        if (lsock < 0) {
            g_printerr("Failed to listen on %s.\n", socket_path);
            return NULL;
        }
        fd = accept(lsock, NULL, NULL);
        close(lsock);
    }

    if (fd == -1) {
        g_printerr("Invalid vhost-user socket.\n");
        return NULL;
    }

    if (!vug_init(&vi->dev, VHOST_USER_INPUT_MAX_QUEUES, fd, vi_panic, &vuiface)) {
        g_printerr("Failed to initialize libvhost-user-glib.\n");
        return NULL;
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return NULL;
}

void remove_device(VuInput *vi)
{
    vug_deinit(&vi->dev);
    vug_source_destroy(vi->evsrc);
    g_array_free(vi->config, TRUE);
    g_free(vi->queue);
    g_free(vi);
}
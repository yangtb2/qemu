#ifndef VHOST_USER_INPUT1
#define VHOST_USER_INPUT1
#include "qemu/osdep.h"

#include <sys/ioctl.h>

#include "qemu/iov.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "qemu/main-loop.h"
#include "libvhost-user-glib.h"
#include "standard-headers/linux/input.h"
#include "standard-headers/linux/virtio_input.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"

enum {
    VHOST_USER_INPUT_MAX_QUEUES = 2,
};

enum InputKind{
    VU_KEYBOARD,
    VU_MOUSE,
    VU_TOUCH,
};

typedef struct virtio_input_event virtio_input_event;
typedef struct virtio_input_config virtio_input_config;

typedef struct VuInput {
    VugDev dev;
    GSource *evsrc;
    int evdevfd;
    GArray *config;
    virtio_input_config *sel_config;
    struct {
        virtio_input_event event;
        VuVirtqElement *elem;
    } *queue;
    uint32_t qindex, qsize;
    enum InputKind ikind;
} VuInput;

typedef struct vu_thread_data
{
    int fd;
    char socket_path[32];
    VuInput vi;
} vu_thread_data;

gpointer add_device(gpointer arg);
void remove_device(VuInput *vi);

#endif
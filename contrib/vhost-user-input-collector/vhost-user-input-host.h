#ifndef _VHOST_USER_INPUT_HOST
#define _VHOST_USER_INPUT_HOST

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qemu/iov.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "libvhost-user-glib.h"
#include "standard-headers/linux/input.h"
#include "standard-headers/linux/virtio_input.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"

enum
{
    VHOST_USER_INPUT_MAX_QUEUES = 2,
};

enum InputKind
{
    VU_INPUT_UNSUPPORT = -1,
    VU_INPUT_KEYBOARD,
    VU_INPUT_MOUSE,
    VU_INPUT_TOUCH,
};

typedef struct virtio_input_event virtio_input_event;
typedef struct virtio_input_config virtio_input_config;

typedef struct VuInput 
{
    VugDev dev;
    GSource *evsrc;
    int evdevfd;
    gchar *evdev;
    GArray *config;
    virtio_input_config *sel_config;
    struct {
    virtio_input_event event;
    VuVirtqElement *elem;
    } *queue;
    uint32_t qindex, qsize;
    enum InputKind viKind;
    GMainLoop *dev_loop;
    bool grabed;
    guint timer_id;
    bool has_event_in_timer;
    bool touch_down;
} VuInput;

typedef struct vu_thread_data
{
    gchar *socket_path;
    VuInput vi;
} vu_thread_data;

typedef struct DisplayWindowSize
{
    /* size relative of screen, 0~1 */
    double x;
    double y;
    double w;
    double h;
    bool   fullscreen;
} DisplayWindowSize;

typedef struct GraphicConsoleState
{
    DisplayWindowSize rel_size;
    bool mouse_focous;
    bool keyboard_focous;
    bool hidden;
} GraphicConsoleState;

extern GraphicConsoleState global_gcs;
extern bool vu_input_disabled;

VuInput *vu_input_add_device(char *evdev, char *socket_path);
void vu_input_remove_device(VuInput *vi);

#endif //VHOST_USER_INPUT_HOST
#include "vhost-user-input-host.h"
#include "sys/inotify.h"

enum VuCollectorHostRequest
{
    AddDevice,
    DelDevice,
};

enum VuCollectorClientRequest
{
    WindowStatusChanged,
};

typedef struct VuCollectorSocketMsg
{
    int request;
    union
    {
        GraphicConsoleState gcs;
        char evdev[16];
    } data;
} VuCollectorSocketMsg;

typedef struct VuCollectorSrc
{
    GSource parent;
    GPollFD gfd;
} VuCollectorSrc;

typedef void (*vu_collector_watch_cb) (int fd, int cond, void *data);
#include "vhost-user-input-host.h"
#include "sys/inotify.h"

enum VuMainHostRequest
{
    AddDevice,
    DelDevice,
};

enum VuMainClientRequest
{
    WindowStatusChanged,
};

typedef struct VuMainSocketMsg
{
    int request;
    union
    {
        GraphicConsoleState gcs;
        char evdev[16];
    } data;
} VuMainSocketMsg;

typedef struct VuMaingSrc
{
    GSource parent;
    GPollFD gfd;
} VuMaingSrc;

typedef void (*vu_main_watch_cb) (int fd, int cond, void *data);
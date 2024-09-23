/*
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

// #include "qemu/osdep.h"

// #include <sys/ioctl.h>

// #include "qemu/iov.h"
// #include "qemu/bswap.h"
// #include "qemu/sockets.h"
// #include "qemu/main-loop.h"
// #include "libvhost-user-glib.h"
// #include "standard-headers/linux/input.h"
// #include "standard-headers/linux/virtio_input.h"
// #include "qapi/error.h"
// #include "qemu/main-loop.h"

#include "vhost-user-input.h"

typedef enum VuMainSocketCommand
{
    AddDevice,
    DelDevice,
    WindowChanged,
} VuMainSocketCommand;

typedef struct VuMainSocketMsg
{
    VuMainSocketCommand command;
    union
    {
        struct
        {
            double x;
            double y;
            double w;
            double h;
        } window_size;
        char evdev[32];
    } data;
} VuMainSocketMsg;

// enum {
//     VHOST_USER_INPUT_MAX_QUEUES = 2,
// };

// typedef struct virtio_input_event virtio_input_event;
// typedef struct virtio_input_config virtio_input_config;

// typedef struct VuInput {
//     VugDev dev;
//     GSource *evsrc;
//     int evdevfd;
//     GArray *config;
//     virtio_input_config *sel_config;
//     struct {
//         virtio_input_event event;
//         VuVirtqElement *elem;
//     } *queue;
//     uint32_t qindex, qsize;
// } VuInput;

static int opt_fdnum = -1;
static char *opt_socket_path;

static GOptionEntry entries[] = {
    { "fd", 'f', 0, G_OPTION_ARG_INT, &opt_fdnum,
      "Use inherited fd socket", "FDNUM" },
    { "socket-path", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket_path,
      "Use UNIX socket path", "PATH" },
    { NULL, }
};

static void vu_main_msg_read(int fd, void *data)
{
    printf("vu_main_msg_read\n");
    VuMainSocketMsg msg;
    recv(fd, &msg, sizeof(msg), 0);
    if(msg.command == WindowChanged){
        printf("window changed: %f, %f, %f, %f\n", msg.data.window_size.x, msg.data.window_size.y, msg.data.window_size.w, msg.data.window_size.h);
    }
}

typedef struct VuMaingSrc {
    GSource parent;
    GPollFD gfd;
} VuMaingSrc;

typedef void (*vu_main_watch_cb) (int fd, int cond, void *data);

static void main_socket_dispatch(int fd, int cond, void *data)
{
    printf("cond=%d\n", cond);
    if(cond == G_IO_IN) {
        vu_main_msg_read(fd, data);
    }

    if(cond & G_IO_HUP) {
        exit(EINVAL);
    }
}

static gboolean
vug_src_prepare(GSource *gsrc, gint *timeout)
{
    g_assert(timeout);

    *timeout = -1;
    return FALSE;
}

static gboolean
vug_src_check(GSource *gsrc)
{
    VuMaingSrc *src = (VuMaingSrc *)gsrc;

    g_assert(src);

    return src->gfd.revents & src->gfd.events;
}

static gboolean
vug_src_dispatch(GSource *gsrc, GSourceFunc cb, gpointer data)
{
    VuMaingSrc *src = (VuMaingSrc *)gsrc;

    g_assert(src);

    ((vu_main_watch_cb)cb)(src->gfd.fd, src->gfd.revents, data);

    return G_SOURCE_CONTINUE;
}

static GSourceFuncs vug_src_funcs = {
    vug_src_prepare,
    vug_src_check,
    vug_src_dispatch,
    NULL
};

static GSource *
main_g_source_new(int fd, GIOCondition cond,
               vu_main_watch_cb vu_cb)
{
    GSource *gsrc;
    VuMaingSrc *src;
    guint id;

    g_assert(fd >= 0);
    g_assert(vu_cb);

    gsrc = g_source_new(&vug_src_funcs, sizeof(VuMaingSrc));
    g_source_set_callback(gsrc, (GSourceFunc)vu_cb, NULL, NULL);
    src = (VuMaingSrc *)gsrc;
    src->gfd.fd = fd;
    src->gfd.events = cond;

    g_source_add_poll(gsrc, &src->gfd);
    id = g_source_attach(gsrc, g_main_context_get_thread_default());
    g_assert(id);

    return gsrc;
}

static inline void get_virtio_input_bits(uint8_t *bitmap, int fd, int type, int count)
{
    ioctl(fd, EVIOCGBIT(type, count/8), bitmap);
}

static bool is_a_mouse_device(int fd)
{
    uint8_t key[128];
    memset(key, 0, sizeof(key));
    get_virtio_input_bits(key, fd, EV_KEY, KEY_CNT);
    return key[BTN_LEFT / 8] & (1 << (BTN_LEFT % 8));
}

static bool is_a_keyboard_device(int fd)
{
    uint8_t key[128];
    memset(key, 0, sizeof(key));
    get_virtio_input_bits(key, fd, EV_KEY, KEY_CNT);
    return key[KEY_A / 8] & (1 << (KEY_A % 8));
}

static bool is_a_touch_device(int fd)
{
    uint8_t abs[128];
    memset(abs, 0, sizeof(abs));
    get_virtio_input_bits(abs, fd, EV_ABS, ABS_CNT);
    return abs[ABS_MT_SLOT / 8] & (1 << (ABS_MT_SLOT % 8));
}

static void watch_evdev_pnp(int socket)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *item;
    char path[267];
    char socket_path[272];
    int fd;
    gpointer arg;
    vu_thread_data *ds;

    VuMainSocketMsg msg = {
        .command = AddDevice,
    };

    while((item=readdir(dir)))
    {
        if(item->d_type != DT_CHR)
            continue;
        sprintf(path, "/dev/input/%s", item->d_name);
        
        fd = open(path, O_RDWR);
        if(fd < 0)
        {
            printf("can't open\n");
            continue;
        }

        if (!g_unix_set_fd_nonblocking(fd, true, NULL)) {
            g_printerr("Failed to set FD nonblocking : %s\n", g_strerror(errno));
            goto close_fd;
        }

        arg = g_malloc(sizeof(vu_thread_data));
        ds = (vu_thread_data*)arg;
        memset(ds, 0, sizeof(*ds));
        ds->fd = fd;

        if(is_a_touch_device(fd))
        {
            printf("%s is a multitouch\n", path);
            ds->vi.ikind = VU_TOUCH;
        }
        else if(is_a_mouse_device(fd))
        {
            printf("%s is a mouse\n", path);
            ds->vi.ikind = VU_MOUSE;
        }
        else if(is_a_keyboard_device(fd))
        {
            printf("%s is a keyboard\n", path);
            ds->vi.ikind = VU_KEYBOARD;
        }
        else{
            goto init_fail;
        }

        memset(msg.data.evdev, 0, sizeof(msg.data.evdev));
        memcpy(msg.data.evdev, item->d_name, strlen(item->d_name));
        printf("send add device msg : %s\n", msg.data.evdev);
        send(socket, &msg, sizeof(msg), 0);
        
        sprintf(socket_path, "/tmp/vhost-%s.sock", item->d_name);
        memcpy(ds->socket_path, socket_path, strlen(socket_path));
        g_thread_ref(g_thread_new("init_evdev", add_device, arg));
        continue;
init_fail:
        free(arg);
close_fd:
        close(fd);
    }
}

int
main(int argc, char *argv[])
{
    GMainLoop *loop = NULL;
    GError *error = NULL;
    GOptionContext *context;
    int fd;
    const char* path = "/tmp/vhost-main.sock";

    context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        exit(EXIT_FAILURE);
    }

    if (opt_socket_path) {
        int lsock = unix_listen(path, &error_fatal);
        if (lsock < 0) {
            g_printerr("Failed to listen on %s.\n", opt_socket_path);
            exit(EXIT_FAILURE);
        }
        fd = accept(lsock, NULL, NULL);
        close(lsock);
    } else {
        int lsock = unix_listen(path, &error_fatal);
        if (lsock < 0) {
            g_printerr("Failed to listen on %s.\n", opt_socket_path);
            exit(EXIT_FAILURE);
        }
        fd = accept(lsock, NULL, NULL);
        printf("accept connect , fd =%d\n", fd);
        close(lsock);
    }
    if (fd == -1) {
        g_printerr("Invalid vhost-user socket.\n");
        exit(EXIT_FAILURE);
    }
    main_g_source_new(fd, G_IO_IN, main_socket_dispatch);

    watch_evdev_pnp(fd);
    
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return 0;
}

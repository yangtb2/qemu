#include "main.h"

struct vu_input_device
{
    VuInput *vi;
    gchar* id;
    QLIST_ENTRY(vu_input_device) node;
};

static QLIST_HEAD(, vu_input_device) vi_list;
static char *opt_socket_path;
static int socket_fd, lsock;

static GOptionEntry entries[] = 
{
    { "socket-path", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket_path,
    "Use UNIX socket path", "PATH" },
    { NULL,}
};

static bool vu_input_should_disable(GraphicConsoleState *gcs)
{
    bool disable;
    disable = gcs->hidden || gcs->rel_size.fullscreen;
#ifdef __LIMBO__
    disable |= gcs->limbo_multi_window_mode;
#endif //__LIMBO
    return disable;
}

static void vu_main_msg_read(int fd, void *data)
{
    VuMainSocketMsg msg;
    GraphicConsoleState *gcs;
    struct vu_input_device *vi;
    bool input_disabled;
    int ret;

    recv(fd, &msg, sizeof(msg), 0);
    g_debug("vu_main_msg_read : command=%d\n", msg.request);

    switch (msg.request)
    {
    case WindowStatusChanged:
        global_gcs = msg.data.gcs;
        input_disabled = vu_input_should_disable(&global_gcs);
        if (input_disabled != vu_input_disabled) {
            vu_input_disabled = input_disabled;
            QLIST_FOREACH (vi, &vi_list, node) {
                if (input_disabled && vi->vi->grabed) {
                    ret = ioctl(vi->vi->evdevfd, EVIOCGRAB, 0);
                    if (ret >= 0) {
                        vi->vi->grabed = false;
                    } 
                } 
                else if (!input_disabled && !vi->vi->grabed && (vi->vi->viKind == VU_INPUT_KEYBOARD || vi->vi->viKind == VU_INPUT_MOUSE)) {
                    ret = ioctl(vi->vi->evdevfd, EVIOCGRAB, 1);
                    if (ret >= 0) {
                        vi->vi->grabed = true;
                    }
                }
            }
        }
        gcs = &global_gcs;
        g_debug("vu_input_disable : %d", vu_input_disabled);
        g_debug("window size: (%f, %f, %f, %f)\n", gcs->rel_size.x, gcs->rel_size.y, gcs->rel_size.w, gcs->rel_size.h);
        g_debug("fullscreen : %d", gcs->rel_size.fullscreen);
        g_debug("keyboard focous : %d", gcs->keyboard_focous);
        g_debug("mouse focous : %d", gcs->mouse_focous);
        g_debug("hidden : %d", gcs->hidden);
#ifdef __LIMBO__
        g_debug("limbo_multi_window_mode : %d", gcs->limbo_muulti_window_mode);
#endif //__LIMBO__
        break;

    default:
        break;
    }
}

static void main_socket_dispatch(int fd, int cond, void *data)
{
    if (cond & G_IO_HUP) {
        exit(EINVAL);
    }
    if (cond & G_IO_IN) {
        vu_main_msg_read(fd, data);
    }
}

static gboolean vug_src_prepare(GSource *gsrc, gint *timeout)
{
    g_assert(timeout);
    *timeout = -1;
    return FALSE;
}

static gboolean vug_src_check(GSource *gsrc)
{
    VuMaingSrc *src = (VuMaingSrc *)gsrc;
    g_assert(src);
    return src->gfd.revents & src->gfd.events;
}

static gboolean vug_src_dispatch(GSource *gsrc, GSourceFunc cb, gpointer data)
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

static GSource *main_g_source_new(int fd, GIOCondition cond, vu_main_watch_cb vu_cb)
{
    GSource *gsrc;
    VuMaingSrc *src;
    guint id;

    g_assert(fd >= 0);
    g_assert(vu_cb);

    gsrc = g_source_new(&vug_src_funcs, sizeof(VuMaingSrc));
    g_source_set_callback(gsrc, (GSourceFunc)vu_cb, NULL, NULL);
    src = (VuMaingSrc*)gsrc;
    src->gfd.fd = fd;
    src->gfd.events = cond;
    g_source_add_poll(gsrc, &src->gfd);
    id = g_source_attach(gsrc, g_main_context_get_thread_default());
    g_assert(id);
    return gsrc;
}

static void add_device(char *name)
{
    gchar *socket_path;
    struct vu_input_device *vid, *new_vid;
    VuMainSocketMsg msg;
    gchar *uid;
    gchar *guid;
    new_vid = g_malloc(sizeof(*new_vid));
    QLIST_FOREACH(vid, &vi_list, node) {
        if(g_strcmp0(vid->vi->evdev, name) == 0) {
            g_debug("%s is already exist\n", name);
            return;
        }
    }
    guid = g_uuid_string_random();
    while(!g_uuid_string_is_valid(guid)) {
        guid = g_uuid_string_random();
    }
    g_debug("g_uuid_string_random : %s", guid);
    uid = g_strdup_printf("%s-%s", name, guid);
    g_free(guid);
    guid = g_strndup(uid, 15);
    g_free(uid);
    g_debug("gen uid = %s\n", guid);
    socket_path = g_strdup_printf("/tmp/vhost-%s.sock", guid);
    new_vid->vi = vu_input_add_device(name, socket_path);
    if (!new_vid->vi) {
        return;
    }
    new_vid->id = guid;
    QLIST_INSERT_HEAD(&vi_list, new_vid, node);
    memcpy(msg.data.evdev, guid, strlen(guid));
    msg.request = AddDevice;
    g_debug("send add device msg : %s\n", msg.data.evdev);
    send(socket_fd, &msg, sizeof(msg), 0);
}

static void del_device(char *evdev)
{
    VuMainSocketMsg msg;
    struct vu_input_device *vid, *target = NULL;
    QLIST_FOREACH(vid,&vi_list, node) {
        if (g_strcmp0(vid->vi->evdev, evdev) == 0) {
            target = vid;
            break;
        }
    }
    if (!target) {
        g_debug("find no %s to remove\n", evdev);
        return;
    }
    QLIST_REMOVE(target, node);
    memset(msg.data.evdev, 0, sizeof(msg.data.evdev));
    memcpy(msg.data.evdev, target->id, strlen(target->id));
    msg.request = DelDevice;
    g_debug("send remove device msg : %s\n", msg.data.evdev);
    send(socket_fd, &msg,sizeof(msg), 0);
    vu_input_remove_device(target->vi);
    g_free(target->id);
    g_free(target);
}

static void evdev_notify_event_read(int fd, void *data)
{
    struct inotify_event *event;
    int rc;
    event = malloc(32);
    for(;;) {
        memset(event, 0, 32);
        rc = read(fd, event, 32);
        if (rc <= (int)sizeof(struct inotify_event)) {
            break;
        }
        g_debug("inotify_event /dev/input: mask=%d, name=%s\n", event->mask, event->name);
        if (event->mask & IN_CREATE) {
            add_device(event->name);
        }
        else if (event->mask & IN_DELETE) {
            del_device(event->name);
        }
    }
    free(event);
}

static void evdev_notify_fd_dispatch(int fd, int cond, void *data)
{
    if (cond == G_IO_IN) {
        evdev_notify_event_read(fd, data);
    }
    if (cond & G_IO_HUP) {
        exit(EINVAL);
    }
}

static void watch_evdev_pnp(void)
{
    int fd = inotify_init();
    inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_DELETE);
    g_unix_set_fd_nonblocking(fd, true, NULL);
    main_g_source_new(fd, G_IO_IN, evdev_notify_fd_dispatch);
}

static void vu_main_init(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *item;
    QLIST_INIT(&vi_list);
    while ((item=readdir(dir)))
    {
        if(item->d_type != DT_CHR)
            continue;
        add_device(item->d_name);
    }
    watch_evdev_pnp();
}

int main(int argc, char *argv[])
{
    GMainLoop *loop = NULL;
    GError *error = NULL;
    GOptionContext *context;
    GSource *gsrc;
    context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, entries, NULL);
    if(!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        exit(EXIT_FAILURE);
    }
    if(!opt_socket_path){
        g_printerr("Must specify --socket-path\n");
        exit(EXIT_FAILURE);
    }
    lsock = unix_listen(opt_socket_path, &error_fatal);
    if (lsock < 0) {
        g_printerr("Failed to listen on %s.\n", opt_socket_path);
        exit(EXIT_FAILURE);
    }
    socket_fd = accept(lsock, NULL, NULL);
    if (socket_fd == -1) {
        g_printerr("Invalid vhost-user socket.\n");
        exit(EXIT_FAILURE);
    }
    gsrc = main_g_source_new(socket_fd, G_IO_IN, main_socket_dispatch);
    vu_main_init();
    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    g_source_destroy(gsrc);
    close(lsock);
    return 0;
}
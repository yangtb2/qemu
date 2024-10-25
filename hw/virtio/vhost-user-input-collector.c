#include "qemu/osdep.h"

#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-input.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/virtio/virtio-pci.h"
#include "qom/object.h"

#include "ui/console.h"
#include "qemu/sockets.h"
#include "monitor/qdev.h"
#include "monitor/hmp.h"
#include "hw/pci/pci_bridge.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qapi-commands-qdev.h"
#include "qapi/qapi-commands-char.h"

#define TYPE_VHOST_USER_INPUT_COLLECTOR   "vhost-user-input-collector"
OBJECT_DECLARE_SIMPLE_TYPE(VHostUserInputCollector, VHOST_USER_INPUT_COLLECTOR)
#define VHOST_USER_INPUT_COLLECTOR_GET_PARENT_CLASS(obj)             \
    OBJECT_GET_PARENT_CLASS(obj, TYPE_VHOST_USER_INPUT_COLLECTOR)

struct VHostUserInputCollector {
    DeviceState parent_obj;
    int vu_socket;
    char *parent_bus_id;
    char *socket_path;
    QLIST_HEAD(, vhost_user_input) vui_list;
};

typedef struct vhost_user_input
{
    DeviceState *ds;
    QLIST_ENTRY(vhost_user_input) node;
} vhost_user_input;

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

static void add_virtio_input_host_pci(char *id, VHostUserInputCollector *vuc)
{
    DeviceState *dev;
    QDict *opt;
    gchar *args, *chardev_id;
    vhost_user_input *vui, *new_vui;

    printf("-->add_virtio_input_host_pci : %s\n", id);

    if (!id || !vuc || !vuc->parent_bus_id) {
        return;
    }

    QLIST_FOREACH(vui, &vuc->vui_list, node) {
        if (g_strcmp0(vui->ds->id, id) == 0) {
            return;
        }
    }

    chardev_id = g_strdup_printf("chardev-%s", id);
    args = g_strdup_printf("socket,path=/tmp/vhost-%s.sock,id=%s", id, chardev_id);

    opt = qdict_new();
    qdict_put_str(opt, "args", args);
    hmp_chardev_add(NULL, opt);
    g_free(args);
    qdict_unref(opt);

    opt = qdict_new();
    qdict_put_str(opt, "driver", "vhost-user-input-pci");
    qdict_put_str(opt, "bus", vuc->parent_bus_id);
    qdict_put_str(opt, "id", id);
    qdict_put_str(opt, "chardev", chardev_id);
    dev = qdev_device_add_from_qdict(opt, false, &error_warn);
    g_free(chardev_id);
    qdict_unref(opt);

    if (!dev) {
        g_warning("has not create device for %s\n", id);
        return;
    }
    new_vui = g_malloc(sizeof(*new_vui));
    new_vui->ds = dev;
    QLIST_INSERT_HEAD(&vuc->vui_list, new_vui, node);
    printf("<--add_virtio_input_host_pci\n");
}

static void del_virtio_input_host_pci(char *id, VHostUserInputCollector *vuc)
{
    vhost_user_input *vui, *target = NULL;
    printf("-->del_virtio_input_host_pci : %s\n", id);
    QLIST_FOREACH(vui, &vuc->vui_list, node) {
        if (g_strcmp0(vui->ds->id, id) == 0) {
            target = vui;
            break;
        }
    }
    if (!target) {
        return;
    }
    QLIST_REMOVE(target, node);
    g_free(target);
    qmp_device_del(id, &error_warn);
    printf("<--del_virtio_input_host_pci\n");
}

static void vu_read_msg(void* opaque)
{
    VHostUserInputCollector *vuc = VHOST_USER_INPUT_COLLECTOR(opaque);
    VuCollectorSocketMsg msg;

    recv(vuc->vu_socket, &msg, sizeof(msg), 0);
    switch (msg.request)
    {
    case AddDevice:
        add_virtio_input_host_pci(msg.data.evdev, vuc);
        break;
    case DelDevice:
        del_virtio_input_host_pci(msg.data.evdev, vuc);
        break;
    default:
        break;
    }
}

static inline bool qbus_is_full(BusState *bus)
{
    BusClass *bus_class;
    if (bus->full) {
        return true;
    }
    bus_class = BUS_GET_CLASS(bus);
    return bus_class->max_dev && bus->num_children>= bus_class->max_dev;
}

static char *vui_find_pci_bus_recursive(BusState*parent, const char *bridge_type)
{
    BusChild *kid;
    PCIBridge *bridge;
    DeviceState *dev;
    BusState *child, *ret;
    char *id;

    QTAILQ_FOREACH(kid, &parent->children, sibling) {
        dev = kid->child;
        if (object_dynamic_cast(OBJECT(dev), bridge_type)){
            bridge = PCI_BRIDGE(dev);
            ret = BUS(pci_bridge_get_sec_bus(bridge));
            if (!qbus_is_full(ret)) {
                return dev->id;
            }
        }
        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            id = vui_find_pci_bus_recursive(child, bridge_type);
            if (id) {
                return id;
            }
        }
    }
    return NULL;
}

static char *vui_find_pci_bus(const char *bridge_type)
{
    return vui_find_pci_bus_recursive(sysbus_get_default(), bridge_type);
}

static void console_state_changed(GraphicConsoleState *state, void *opaque)
{
    VHostUserInputCollector *vuc = VHOST_USER_INPUT_COLLECTOR(opaque);
    VuCollectorSocketMsg msg;

    printf("console_state_changed\n");
    msg.request = WindowStatusChanged;
    msg.data.gcs = *state;
    write(vuc->vu_socket, &msg, sizeof(msg));
}

static void vhost_user_input_collector_realize(DeviceState *dev, Error **errp)
{
    VHostUserInputCollector *vuc = VHOST_USER_INPUT_COLLECTOR(dev);
    DeviceState *bridge;
    QDict *opt;
    char *bus_id;
    QemuConsole *con;
    GraphicConsoleState state;

    bus_id = vui_find_pci_bus("pci-bridge");
    if (!bus_id) {
        bus_id = vui_find_pci_bus("pcie-pci-bridge");
        if (!bus_id) {
            opt = qdict_new();
            qdict_put_str(opt, "driver", "pcie-pci-bridge");
            qdict_put_str(opt, "id", "pcie-pci-bridge-0");
            bridge = qdev_device_add_from_qdict(opt, false, &error_fatal);
            qdict_unref(opt);
            if (!bridge) {
                return;
            }
            bus_id = bridge->id;
        }
        opt = qdict_new();
        qdict_put_str(opt, "driver", "pci-bridge");
        qdict_put_str(opt, "bus", bus_id);
        qdict_put_str(opt, "id", "pci-bridge-0");
        qdict_put_str(opt, "chassis_nr", "1");
        bridge = qdev_device_add_from_qdict(opt, false, &error_fatal);
        qdict_unref(opt);
        if (!bridge) {
            return;
        }
        bus_id = bridge->id;
    }

    vuc->parent_bus_id = bus_id;
    vuc->vu_socket = unix_connect(vuc->socket_path, NULL);
    g_assert_cmpint(vuc->vu_socket, !=, -1);
    QLIST_INIT(&vuc->vui_list);
    qemu_set_fd_handler(vuc->vu_socket, vu_read_msg, NULL, dev);

    con = qemu_console_lookup_by_index(0);
    state = graphic_console_get_state(con);
    console_state_changed(&state, vuc);
    // register_graphic_console_state_listener(con, TYPE_VHOST_USER_INPUT_COLLECTOR, console_state_changed, vuc);
}

static Property vhost_user_input_collector_properties[] = {
    DEFINE_PROP_STRING("socket_path", VHostUserInputCollector, socket_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void vhost_input_collector_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_props(dc, vhost_user_input_collector_properties);
    dc->bus_type = NULL;
    dc->realize = vhost_user_input_collector_realize;
}

static const TypeInfo vhost_input_collector_info = {
    .name = TYPE_VHOST_USER_INPUT_COLLECTOR,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(VHostUserInputCollector),
    .class_init = vhost_input_collector_class_init,
};

static void vhost_user_input_collector_register(void)
{
    type_register_static(&vhost_input_collector_info);
}

type_init(vhost_user_input_collector_register);
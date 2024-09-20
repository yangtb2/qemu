/*
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-input.h"
#include "qemu/sockets.h"
#include "qapi/qmp/qdict.h"
#include "monitor/qdev.h"
#include "hw/pci/pci_bridge.h"
#include "qapi/qapi-commands-char.h"
#include "monitor/hmp.h"

static Property vinput_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserBase, chardev),
    DEFINE_PROP_END_OF_LIST(),
};

static void vinput_realize(DeviceState *dev, Error **errp)
{
    VHostUserBase *vub = VHOST_USER_BASE(dev);
    VHostUserBaseClass *vubc = VHOST_USER_BASE_GET_CLASS(dev);

    /* Fixed for input device */
    vub->virtio_id = VIRTIO_ID_INPUT;
    vub->num_vqs = 2;
    vub->vq_size = 4;
    vub->config_size = sizeof(virtio_input_config);

    vubc->parent_realize(dev, errp);
}

static const VMStateDescription vmstate_vhost_input = {
    .name = "vhost-user-input",
    .unmigratable = 1,
};

static void vhost_input_class_init(ObjectClass *klass, void *data)
{
    VHostUserBaseClass *vubc = VHOST_USER_BASE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_vhost_input;
    device_class_set_props(dc, vinput_properties);
    device_class_set_parent_realize(dc, vinput_realize,
                                    &vubc->parent_realize);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo vhost_input_info = {
    .name          = TYPE_VHOST_USER_INPUT,
    .parent        = TYPE_VHOST_USER_BASE,
    .instance_size = sizeof(VHostUserInput),
    .class_init    = vhost_input_class_init,
};

/*--------------------------------------------------*/
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

static int vu_socket;
static char *pci_bus;

typedef struct vu_thread_data
{
    char id[32];
    char *bus;
} vu_thread_data;


static gpointer add_virtio_input_host_pci(gpointer arg)
{
    DeviceState *dev;
    QDict *opt;
    char socket_path[32];
    char args[256];
    vu_thread_data *data = (vu_thread_data*)arg;

    char *id = data->id;
    char *bus = data->bus;

    sprintf(socket_path, "/tmp/vhost-%s.sock", id);
    sprintf(args, "socket,path=%s,id=%s", socket_path, id);

    opt = qdict_new();
    qdict_put_str(opt,"args", args);
    hmp_chardev_add(NULL, opt);
    qdict_unref(opt);

    opt = qdict_new();
    qdict_put_str(opt, "driver", "vhost-user-input-pci");
    qdict_put_str(opt, "bus", bus);
    qdict_put_str(opt, "chardev", id);
    dev = qdev_device_add_from_qdict(opt, false, &error_warn);

    if(!dev)
    {
        printf("has not create device\n");
    }
    else{
        printf("finish add_virtio_input_host_pci\n");
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    qdict_unref(opt);
    return NULL;
}

static void vu_read_msg(void* opaque)
{
    gpointer arg = g_malloc(sizeof(vu_thread_data));
    vu_thread_data *data = (vu_thread_data*)arg;
    printf("vu_read_msg\n");
    VuMainSocketMsg msg;
    recv(vu_socket, &msg, sizeof(msg), 0);
    if(msg.command == AddDevice){
        printf("add device: %s\n", msg.data.evdev);
    }
    if(!msg.data.evdev || !pci_bus){
        return;
    }
    memcpy(data->id, msg.data.evdev, 32);
    data->bus = pci_bus;
    g_thread_new("vhost", add_virtio_input_host_pci, arg);
}

static inline bool qbus_is_full(BusState *bus)
{
    BusClass *bus_class;

    if (bus->full) {
        return true;
    }
    bus_class = BUS_GET_CLASS(bus);
    return bus_class->max_dev && bus->num_children >= bus_class->max_dev;
}

static char *virtio_find_pci_bus_recursive(BusState *parent, const char *bridge_type)
{
    BusChild *kid;
    PCIBridge *bridge;
    DeviceState *dev;
    BusState *child, *ret;
    char *id;

    QTAILQ_FOREACH(kid, &parent->children, sibling) {
        dev = kid->child;
        printf("id = %s\n", dev->id);
        if(object_dynamic_cast(OBJECT(dev), bridge_type)){
            bridge = PCI_BRIDGE(dev);
            ret = BUS(pci_bridge_get_sec_bus(bridge));
            if(!qbus_is_full(ret)){
                printf("return\n");
                return dev->id;
            }
        }
        QLIST_FOREACH(child, &dev->child_bus, sibling) {
            id = virtio_find_pci_bus_recursive(child, bridge_type);
            if(id)
                return id;
        }
    }

    return NULL;
}

static char *virtio_find_pci_bus(const char *bridge_type)
{
    printf("--->virtio_find_pci_bus===%s\n", bridge_type);
    return virtio_find_pci_bus_recursive(sysbus_get_default(), bridge_type);
}

static void vhost_input_main_start(Object *object)
{
    DeviceState *bridge;
    QDict *opt;

    pci_bus = virtio_find_pci_bus("pci-bridge");
    if(!pci_bus){
        pci_bus = virtio_find_pci_bus("pcie-pci-bridge");
        if(!pci_bus){
            opt = qdict_new();
            qdict_put_str(opt, "driver", "pcie-pci-bridge");
            qdict_put_str(opt, "id", "pcie-pci-bridge-0");
            bridge = qdev_device_add_from_qdict(opt, false, &error_fatal);
            qdict_unref(opt);
            if(!bridge)
                return;
            pci_bus = bridge->id;
        }
        opt = qdict_new();
        qdict_put_str(opt, "driver", "pci-bridge");
        qdict_put_str(opt, "bus", pci_bus);
        qdict_put_str(opt, "id", "pci-bridge-0");
        qdict_put_str(opt, "chassis_nr", "1");
        bridge = qdev_device_add_from_qdict(opt, false, &error_fatal);
        qdict_unref(opt);
        if(!bridge)
            return;
        pci_bus = bridge->id;
    }
    else{
        printf("found pci-bridge\n");
    }
    vu_socket = unix_connect("/tmp/vhost-main.sock", NULL);
    // g_assert_cmpint(vu_socket, !=, -1);
    qemu_set_fd_handler(vu_socket, vu_read_msg, NULL, NULL);
}

static const TypeInfo vhost_input_main_info = {
    .name          = "vhost-input-main",
    .parent        = TYPE_OBJECT,
    .instance_init    = vhost_input_main_start,
};

static void vhost_input_register_types(void)
{
    type_register_static(&vhost_input_info);
    type_register_static(&vhost_input_main_info);
}

type_init(vhost_input_register_types)

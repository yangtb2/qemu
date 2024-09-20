/*
 * Virtio input host PCI Bindings
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"

#include "hw/virtio/virtio-pci.h"
#include "hw/virtio/virtio-input.h"
#include "hw/pci/pci_bridge.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "qom/object_interfaces.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qapi-commands-qdev.h"
#include "monitor/qdev.h"
#include "migration/misc.h"
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include "standard-headers/linux/input.h"

typedef struct VirtIOInputHostPCI VirtIOInputHostPCI;

#define TYPE_VIRTIO_INPUT_HOST_PCI "virtio-input-host-pci"
DECLARE_INSTANCE_CHECKER(VirtIOInputHostPCI, VIRTIO_INPUT_HOST_PCI,
                         TYPE_VIRTIO_INPUT_HOST_PCI)

struct VirtIOInputHostPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOInputHost vdev;
};

#define TYPE_VIRTIO_INPUT_HOST_MONITOR  "virtio-input-host-monitor"
OBJECT_DECLARE_SIMPLE_TYPE(VirtIOInputHostMonitor, VIRTIO_INPUT_HOST_MONITOR)

struct virtio_input_host
{
    VirtIOInputHostPCI *vih;
    QLIST_ENTRY(virtio_input_host) node;
};

struct VirtIOInputHostMonitor{
    Object parent_obj;
    int notify_fd;
    char *bus;
    QLIST_HEAD(, virtio_input_host) vih_list;
};

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

static void virtio_host_initfn(Object *obj)
{
    VirtIOInputHostPCI *dev = VIRTIO_INPUT_HOST_PCI(obj);
    
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_INPUT_HOST);
}

static const VirtioPCIDeviceTypeInfo virtio_input_host_pci_info = {
    .generic_name  = TYPE_VIRTIO_INPUT_HOST_PCI,
    .parent        = TYPE_VIRTIO_INPUT_PCI,
    .instance_size = sizeof(VirtIOInputHostPCI),
    .instance_init = virtio_host_initfn,
};

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

static void add_virtio_input_host_pci(VirtIOInputHostMonitor *vihm, const char *evdev, const char *name)
{
    DeviceState *dev;
    struct virtio_input_host *host;
    int fd;
    QDict *opt;

    QLIST_FOREACH(host, &vihm->vih_list, node)
    {
        if(strcmp(host->vih->vdev.evdev, evdev) == 0){
            return;
        }
    }

    fd = open(evdev, O_RDWR);

    if(!is_a_touch_device(fd) && !is_a_mouse_device(fd) && !is_a_keyboard_device(fd))
        goto exit;


    opt = qdict_new();
    qdict_put_str(opt, "driver", TYPE_VIRTIO_INPUT_HOST_PCI);
    qdict_put_str(opt, "bus", vihm->bus);
    qdict_put_str(opt, "id", name);
    qdict_put_str(opt, "evdev", evdev);
    dev = qdev_device_add_from_qdict(opt, false, &error_warn);

    if(!dev)
    {
        printf("has not create device\n");
        goto exit;
    }

    close(fd);
    printf("add_virtio_input_host_pci\n");


    host = g_new0(struct virtio_input_host, 1);
    host->vih = VIRTIO_INPUT_HOST_PCI(dev);

    QLIST_INSERT_HEAD(&vihm->vih_list, host, node);
exit:
    close(fd);
    qdict_unref(opt);
}

static void del_virtio_input_host_pci(VirtIOInputHostMonitor *vihm, const char *evdev, const char *name)
{
    struct virtio_input_host *host;
    bool found;

    QLIST_FOREACH(host, &vihm->vih_list, node)
    {
        if(strcmp(host->vih->vdev.evdev, evdev) == 0){
            found = true;
            break;
        }
    }
    if(!found){
        return;
    }

    printf("del_virtio_input_host_pci:%s\n",host->vih->vdev.evdev);

    qmp_device_del(name, &error_warn);
    QLIST_REMOVE(host, node);
}

static void virtio_host_device_changed(void *opaque)
{
    VirtIOInputHostMonitor *vihm;
    struct inotify_event *event;
    int rc, rs;
    char path[256];

    printf("virtio_host_device_changed\n");
    vihm = VIRTIO_INPUT_HOST_MONITOR(opaque);
    rs = sizeof(struct inotify_event) + 256 + 1;
    event = malloc(rs);
    for(;;){
        rc = read(vihm->notify_fd, event, rs);
        printf("rc=%d\n", rc);
        if(rc <= (int)sizeof(struct inotify_event)){
            break;
        }
        sprintf(path, "/dev/input/%s", event->name);
        printf("wd=%d, mask=%x, cookie=%x, len=%d, name=%s\n", event->wd, event->mask, event->cookie, event->len, event->name);
        
        if(event->mask & IN_CREATE){
            printf("add device %s\n", path);
            add_virtio_input_host_pci(vihm, path, event->name);
        }
        else if(event->mask & IN_DELETE){
            printf("del device %s\n", path);
            del_virtio_input_host_pci(vihm, path, event->name);
        }
    }

    free(event);
}

static void start_monitor(VirtIOInputHostMonitor *vihm)
{
    int fd;
    fd = inotify_init();
    inotify_add_watch(fd, "/dev/input", IN_CREATE | IN_DELETE);
    g_unix_set_fd_nonblocking(fd, true, NULL);

    vihm->notify_fd = fd;
    qemu_set_fd_handler(fd, virtio_host_device_changed, NULL, vihm);
}

static void virtio_input_host_monitor_init(Object *obj)
{
    VirtIOInputHostMonitor *vihm = VIRTIO_INPUT_HOST_MONITOR(obj);
    DIR *dir = opendir("/dev/input");
    DeviceState *dev, *bridge;
    struct dirent *item;
    char path[267];
    int fd;
    struct virtio_input_host *input;
    char *pci_bus;
    QDict *opt;

    pci_bus = virtio_find_pci_bus("pci-bridge");
    if(!pci_bus){
        pci_bus = virtio_find_pci_bus("pcie-pci-bridge");
        if(!pci_bus){
            // bridge = virtio_device_hotplug("pcie-pci-bridge", NULL);
            // pci_bus = BUS(pci_bridge_get_sec_bus(PCI_BRIDGE(bridge)));
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

    if(!pci_bus)
        return;

    vihm->bus = pci_bus;

    QLIST_INIT(&vihm->vih_list);
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

        if(is_a_touch_device(fd))
        {
            printf("%s is a multitouch\n", path);
        }
        else if(is_a_mouse_device(fd))
        {
            printf("%s is a mouse\n", path);
        }
        else if(is_a_keyboard_device(fd))
        {
            printf("%s is a keyboard\n", path);
        }
        else{
            goto close_fd;
        }

        opt = qdict_new();
        qdict_put_str(opt, "driver", TYPE_VIRTIO_INPUT_HOST_PCI);
        qdict_put_str(opt, "bus", pci_bus);
        qdict_put_str(opt, "id", item->d_name);
        qdict_put_str(opt, "evdev", path);
        printf("qdev_device_add_from_qdict\n");
        dev = qdev_device_add_from_qdict(opt, false, &error_warn);
        qdict_unref(opt);
        if(!dev)
        {
            printf("has not create device\n");
            goto close_fd;
        }
        input = g_new0(struct virtio_input_host, 1);
        input->vih = VIRTIO_INPUT_HOST_PCI(dev);

        QLIST_INSERT_HEAD(&vihm->vih_list, input, node);
close_fd:
        close(fd);
    }
    start_monitor(vihm);
}

static const TypeInfo virtio_input_host_monitor_info = {
    .name          = TYPE_VIRTIO_INPUT_HOST_MONITOR,
    .parent        = TYPE_OBJECT,
    .instance_size = sizeof(VirtIOInputHostMonitor),
    .instance_init = virtio_input_host_monitor_init,
};

static void virtio_input_host_pci_register(void)
{
    virtio_pci_types_register(&virtio_input_host_pci_info);
    type_register_static(&virtio_input_host_monitor_info);
}

type_init(virtio_input_host_pci_register)

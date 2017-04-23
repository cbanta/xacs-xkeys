
#include "xkeys.h"
#include <libudev.h>

#define VEND_ID 0x05f3
#define PROD_ID 0x045d
#define INTERFACE_NUM 0x00




static struct udev_xkeys_device *udev_xkeys_device_new(const char *path,
	unsigned vendorId, unsigned productId)
{
	int plen = strlen(path);
	struct udev_xkeys_device *dev = calloc(1,
		sizeof(struct udev_xkeys_device) + plen+1);
	dev->path = (char*)(dev + 1);
	memcpy(dev->path, path, plen);
	dev->path[plen] = 0;
	dev->vendorId = vendorId;
	dev->productId = productId;
	return dev;
}

static void udev_xkeys_device_add(struct udev_state *state,
	struct udev_xkeys_device *dev)
{
	if( state->devices == NULL ){
		state->devices = dev;
	}else{
		struct udev_xkeys_device *p = state->devices;
		while(p->next){
			p = p->next;
		}
		p->next = dev;
	}
}

static void udev_process_add(struct udev_state *state, struct udev_device *dev)
{
	unsigned int vendorId, productId, interfaceNum;
	const char *path = udev_device_get_devnode(dev);

	// printf("ADD Device\n");
	// printf("   Node: %s\n", path);

	dev = udev_device_get_parent_with_subsystem_devtype(
		dev, "usb", "usb_interface");
	if( !dev ) return;

	interfaceNum = strtoul(udev_device_get_sysattr_value(dev,"bInterfaceNumber"), NULL, 16);
	// printf("  sysPath: %s\n", udev_device_get_syspath(dev));
	// printf("   bInterfaceNumber: %s\n", udev_device_get_sysattr_value(dev,"bInterfaceNumber"));

	dev = udev_device_get_parent_with_subsystem_devtype(
	       dev, "usb", "usb_device");
	if( !dev ) return;

	// printf("   VID/PID: %s %s\n", udev_device_get_sysattr_value(dev,"idVendor"), udev_device_get_sysattr_value(dev,"idProduct"));
	// printf("   sysPath: %s\n", udev_device_get_syspath(dev));
	// printf("  sysPath: %s\n", udev_device_get_syspath(dev));



	vendorId = strtoul(udev_device_get_sysattr_value(dev,"idVendor"), NULL, 16);
	productId = strtoul(udev_device_get_sysattr_value(dev,"idProduct"), NULL, 16);
	if( vendorId == VEND_ID && interfaceNum == INTERFACE_NUM ){
		struct udev_xkeys_device *d = udev_xkeys_device_new(path, vendorId, productId);
		udev_xkeys_device_add(state, d);
		if( !state->current ){
			state->current = d;
			state->add_cb(state->userdata, state->current);
		}
	}

}

static void udev_process_remove(struct udev_state *state,
	struct udev_device *dev)
{
	const char *path = udev_device_get_devnode(dev);

	// printf("REMOVE Device\n");
	// printf("   Node: %s\n", udev_device_get_devnode(dev));
	// printf("   Subsystem: %s\n", udev_device_get_subsystem(dev));

	struct udev_xkeys_device *p = state->devices;
	struct udev_xkeys_device *l = NULL;
	while(p){
		if( strcmp(path, p->path) == 0 ){
			if( state->current == p ){
				state->remove_cb(state->userdata, state->current);
				state->current = NULL;
			}
			if( l == NULL ){
				state->devices = p->next;
			}else{
				l->next = p->next;
			}
			free(p);
			if( state->devices ){
				state->current = state->devices;
				state->add_cb(state->userdata, state->current);
			}
			return;
		}
		l = p;
		p = p->next;
	}
}

static void udev_mon_cb(uv_poll_t *uv, int status, int events)
{
	struct udev_state *state = (struct udev_state *)uv->data;
	struct udev_device *dev;

	if( status != 0 ){
		printf("ERROR: status != 0 in udev_mon_cb: %d\n", status);
		return;
	}
	if( events & UV_READABLE ){
		dev = udev_monitor_receive_device(state->mon);
		if( dev ){
			const char *action = udev_device_get_action(dev);
			// printf("ACTION: \"%s\"\n", action);
			if( strcmp(action, "add") == 0 ){
				udev_process_add(state, dev);
			}else if( strcmp(action, "remove") == 0 ){
				udev_process_remove(state, dev);
			}else{
				printf("UNKNOW ACTION: %s", action);
			}
			udev_device_unref(dev);
		}else{
			printf("No Device from receive_device(). An error occured.\n");
		}
	}else{
		printf("ERROR: udev_mon_cb bad events\n");
	}
}


static void udev_enumerate(struct udev_state *state)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct udev_device *dev;

	enumerate = udev_enumerate_new(state->udev);
	udev_enumerate_add_match_subsystem(enumerate, "hidraw");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *path;

		path = udev_list_entry_get_name(dev_list_entry);
		dev = udev_device_new_from_syspath(state->udev, path);
		udev_process_add(state, dev);
		udev_device_unref(dev);
	}
	udev_enumerate_unref(enumerate);

}


struct udev_state *udev_init(void *userdata,
	udev_add_cb_t add_cb, udev_remove_cb_t remove_cb)
{
	int fd;
	struct udev_state *state = calloc(1,sizeof(struct udev_state));

	state->udev = udev_new();
	if (!state->udev) {
		printf("Can't create udev\n");
		exit(1);
	}

	state->userdata = userdata;
	state->add_cb = add_cb;
	state->remove_cb = remove_cb;

	state->mon = udev_monitor_new_from_netlink(state->udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(state->mon, "hidraw", NULL);
	udev_monitor_enable_receiving(state->mon);
	fd = udev_monitor_get_fd(state->mon);

	state->uv.data = (void*)state;
	uv_poll_init(uv_default_loop(), &state->uv, fd);
	uv_poll_start(&state->uv, UV_READABLE, udev_mon_cb);

	udev_enumerate(state);

	return state;

}

void udev_cleanup(struct udev_state *state){
	uv_poll_stop(&state->uv);

	udev_unref(state->udev);
}



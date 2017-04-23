

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <uv.h>


#define USB_REPORT_SIZE 80   /* max size of a single usb report */





typedef void (*async_data_cb_t)(void*, char*, int);

struct async_data {
	char *data;
	int len;

	void *userdata;
	async_data_cb_t cb;

	struct async_data *next;
};

struct mqtt_subscription {
	int mid;
	char *sub;

	void *userdata;
	async_data_cb_t cb;

	struct mqtt_subscription *next;
};


struct mosquitto;
struct mqtt_state {
	struct mosquitto *mosq;

	struct mqtt_subscription *subs;

	struct async_data *async_data;

	uv_async_t uv;
	uv_mutex_t mutex;

	bool is_connected;
};


void mqtt_add_sub(struct mqtt_state *state, const char *sub, void *userdata, async_data_cb_t cb);
struct mqtt_state *mqtt_init(const char *prefix, bool clean, const char *bind);
void mqtt_close(struct mqtt_state *state);





struct udev_xkeys_device {
	char *path;
	unsigned vendorId;
	unsigned productId;

	struct udev_xkeys_device *next;
};

typedef void (*udev_add_cb_t)(void*, struct udev_xkeys_device*);
typedef void (*udev_remove_cb_t)(void*, struct udev_xkeys_device*);

struct udev;
struct udev_monitor;
struct udev_state{
	uv_poll_t uv;

	struct udev *udev;
	struct udev_monitor *mon;

	struct udev_xkeys_device *devices;
	struct udev_xkeys_device *current;

	void *userdata;
	udev_add_cb_t add_cb;
	udev_remove_cb_t remove_cb;
};

struct udev_state *udev_init(void *userdata, udev_add_cb_t add_cb, udev_remove_cb_t remove_cb);
void udev_cleanup(struct udev_state *state);






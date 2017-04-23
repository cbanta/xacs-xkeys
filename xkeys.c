
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <execinfo.h>
#include <signal.h>

#include "xkeys.h"

#define REPORT_SIZE 80   /* max size of a single usb report */



typedef struct xkeys_state {
	struct mqtt_state *mqtt;
	struct udev_state *udev;
	uv_device_t *hidraw;
	uint8_t lastz;
	int got_lastz;

	const char *hidraw_open_pending_close;
	bool hidraw_closing;
} xkeys_state_t;

typedef struct write_data {
	uv_write_t req;
	uv_buf_t bufs[1];
} write_data_t;


void hidraw_write(uv_write_t *req, int status);


void print_buf(char *data, int len)
{
	int i;
	// printf("\r");
	for (i = 0; i < len; i++) {
		printf("%02hhx ", data[i]);
		if ((i+1) % 8 == 0)
			printf("  ");
		// if ((i+1) % 16 == 0)
			// printf("\n");
	}
	// printf("\n");
}

void handler(int sig) {
  void *array[10];
  size_t size;

  fflush(stdout);

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}















#define ADDCHAR(pos, poslen, c) \
	if(poslen > 0){pos[0] = c; pos+=1; poslen-=1;}
#define ADDFLAG(pos, poslen, d, mask, id, fmt) do {\
	int val = d & mask; \
	if( val ){ \
		int chunk = snprintf(pos, poslen, fmt, id); \
		pos += chunk; poslen -= chunk; \
	} \
	} while (0)
#define ADDVAL(pos, poslen, fmt, ...) do {\
	int chunk = snprintf(pos, poslen, fmt, __VA_ARGS__); \
	pos += chunk; poslen -= chunk; \
	} while (0)

void parseSplat(xkeys_state_t *state, uint8_t *data){
	char buf[512];
	char *pos = buf;
	int poslen = 1024;
	int chunk = 0;
	int c,r;
	int zrel = 0;


	if( state->got_lastz ){
		zrel = data[16] - state->lastz;
		if( zrel > 128 ) zrel -= 256;
		if( zrel < -128 ) zrel += 256;
	}else{
		state->got_lastz = 1;
	}
	state->lastz = (uint8_t)data[16];

	ADDVAL(pos, poslen, "J:%d,%d,%d;",
		(int8_t)data[14], (int8_t)data[15], zrel
	);


	ADDCHAR(pos, poslen, 'B');
	ADDCHAR(pos, poslen, ':');
	for(c=0; c<10; c++){
		for(r=0; r<8;r++){
			ADDFLAG(pos, poslen, data[2+c], 1 << r, (c*8) + r, "%u ");
		}
	}
	if( (pos-1)[0] == ' ' ){
		pos -= 1; poslen += 1;
	}else{
		pos -= 3; poslen += 3;
	}

	if( data[1] & 1 ){
		ADDCHAR(pos, poslen, ';');
		ADDCHAR(pos, poslen, 'P');
	}
	ADDCHAR(pos, poslen, 0);

	printf("  DATA %s\n", buf);

	mosquitto_publish(state->mqtt->mosq, NULL, "xacs/xkeys/events", strlen(buf), buf, 1, false);
}










void parseCmd(void *userdata, char *msg, int msglen){
	struct xkeys_state *state = (struct xkeys_state *) userdata;
	char report[36];
	char argv[8][8];
	int argc = 0;

	memset(report, 0, 36);

	// printf("cmd %3s\n", msg);
	if( msglen >= 3 && strncmp(msg, "sys", 3) == 0 ){
		// sys [r... &| g...]
		// printf("...sys\n");
		uint8_t leds = 0;
		argc = sscanf(msg,"sys %8s %8s", argv[0], argv[1]);
		while( argc > 0 ){
			// printf(".....arg %s\n", argv[argc-1]);
			if( argv[argc-1][0] == 'r' ){
				leds |= (1 << 7);
			}
			if( argv[argc-1][0] == 'g' ){
				leds |= (1 << 6);
			}
			argc--;
		}
		report[1] = 186;
		report[2] = leds;
	}else if( msglen >= 4 && strncmp(msg, "isys", 4) == 0 ){
		// isys [r...|g...] val(off,on,flash|blink)

		uint8_t idx = 0;
		uint8_t val = 0;
		argc = sscanf(msg,"isys %8s %8s", argv[0], argv[1]);
		if( argc > 0 ){
			if( argv[0][0] == 'r' ){
				idx = 7;
			}
			if( argv[0][0] == 'g' ){
				idx = 6;
			}
		}
		if( argc > 1 ){
			if(argv[1][0] == 'o' && argv[1][1] == 'n'){
				val = 1;
			}else if(argv[1][0] == 'o' && argv[1][1] == 'f'){
				val = 0;
			}else if(argv[1][0] == 'f' || argv[1][0] == 'b'){
				val = 2;
			}
		}
		if( idx ){
			report[1] = 179;
			report[2] = idx;
			report[3] = val;
		}
	}else if( msglen >= 4 && strncmp(msg, "poll", 4) == 0 ){
		report[1] = 177;
	// }else if( msglen >= 6 && strncmp(msg, "reboot", 6) == 0 ){
		// report[1] = 238;
	}else if( msglen >= 3 && strncmp(msg, "frq", 3) == 0 ){
		uint8_t frq = 0;
		argc = sscanf(msg,"frq %hhu", &frq);
		if( argc == 1 && frq > 0 ){
			report[1] = 180;
			report[2] = frq;
		}
	}else if( msglen >= 4 && strncmp(msg, "int ", 4) == 0 ){
		// int blue(0-255) red(0-255)
		uint8_t bank1 = 0;
		uint8_t bank2 = 0;
		argc = sscanf(msg,"int %hhu %hhu", &bank1, &bank2);
		if( argc == 2 ){
			report[1] = 187;
			report[2] = bank1;
			report[3] = bank2;
		}
	}else if( msglen > 3 && strncmp(msg, "set", 3) == 0 ){
		// red btnid(0-79) val(off,on,flash|blink)
		// printf("...set\n");
		uint8_t led = -1;
		uint8_t val = 0;
		uint8_t bank = 0;
		argc = sscanf(msg,"set %hhu %8s %8s", &led, argv[0], argv[1]);
		if( argc > 1 ){
			if(argv[0][0] == 'r'){
				bank = 80;
			}else if(argv[0][0] == 'b'){
				bank = 0;
			}
		}
		if( argc > 2 ){
			if(argv[1][0] == 'o' && argv[1][1] == 'n'){
				val = 1;
			}else if(argv[1][0] == 'o' && argv[1][1] == 'f'){
				val = 0;
			}else if(argv[1][0] == 'f' || argv[1][0] == 'b'){
				val = 2;
			}
		}
		if( led >= 0 && led < 80 ){
			if( val > 2 ) val = 1;
			if( val < 0 ) val = 0;
			report[1] = 181;
			report[2] = bank + led;
			report[3] = val;
		}
	}
	if( report[1] ){
		// printf("\nREPORT ");
		// print_buf(report, 36);
		// printf("\n");
		// fflush(stdout);
		write_data_t *d = (write_data_t*) calloc(1, sizeof(write_data_t) + sizeof(report));
		d->req.data = d;
		d->bufs[0].base = (void*)(d + 1);
		d->bufs[0].len = sizeof(report);
		memcpy(d->bufs[0].base, report, 36);
		if( state->hidraw && !state->hidraw_closing )
			uv_write(&d->req, (uv_stream_t*)state->hidraw, d->bufs, 1, hidraw_write);
	}
}






void hidraw_alloc(uv_handle_t *handle, size_t size, uv_buf_t *buf){
	// printf("alloc %d\n", size);
	buf->base = malloc(REPORT_SIZE);
	buf->len = REPORT_SIZE;
}
void hidraw_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf){
	xkeys_state_t *state = (xkeys_state_t*)stream->data;
	if( buf->len >= 32 ){
		parseSplat(state, buf->base);
	}
	// printf( "READ FROM HIDRAW %d\n", buf->len);
	// print_buf(buf->base, buf->len < 32 ? buf->len : 32);
	// printf("\n");
	if( buf->base ){
		free(buf->base);
	}
}
void hidraw_write(uv_write_t *req, int status){
	// printf("WRITE DONE\n");
	free(req->data);
}


static void hidraw_open(struct xkeys_state *state, const char *path)
{
	if( state->hidraw ){
		state->hidraw_open_pending_close = path;
		return;
	}
	printf("Opening %s\n", path);
	uv_device_t *hidraw = calloc(1, sizeof(uv_device_t));
	hidraw->data = (void*)state;
	state->hidraw = hidraw;
	uv_device_init(uv_default_loop(), hidraw, path, O_RDWR);
	// uv_stream_set_blocking(&hidraw, 1);
	uv_read_start((uv_stream_t*)hidraw, hidraw_alloc, hidraw_read);
}

static void hidraw_close_finished(uv_handle_t *uv)
{
	xkeys_state_t *state = (xkeys_state_t*)uv->data;
	uv_device_t *hidraw = state->hidraw;
	state->hidraw = NULL;
	state->hidraw_closing = false;

	uv_unref((uv_handle_t*)hidraw);
	free(hidraw);
	if( state->hidraw_open_pending_close ){
		const char *path = state->hidraw_open_pending_close;
		state->hidraw_open_pending_close = NULL;
		hidraw_open(state, path);
	}
}
static void hidraw_close(struct xkeys_state *state)
{
	if( state->hidraw_closing == true ) return;
	state->hidraw_closing = true;

	printf("Closing hidraw device\n");
	uv_read_stop((uv_stream_t*)state->hidraw);
	uv_close((uv_handle_t*)state->hidraw, hidraw_close_finished);
}


static void udev_add_cb(void *userdata, struct udev_xkeys_device *dev)
{
	struct xkeys_state *state = (struct xkeys_state*) userdata;
	printf("[ADD CB] New Device %s [%04x:%04x]\n", dev->path, dev->vendorId, dev->productId);

	if( state->hidraw ){
		hidraw_close(state);
	}
	hidraw_open(state, dev->path);
	mosquitto_publish(state->mqtt->mosq, NULL, "xacs/xkeys/events/dev", 3, "NEW", 1, false);
}
static void udev_remove_cb(void *userdata, struct udev_xkeys_device *dev)
{
	struct xkeys_state *state = (struct xkeys_state*) userdata;
	printf("[REMOVE CB] Device Gone %s [%04x:%04x]\n", dev->path, dev->vendorId, dev->productId);

	hidraw_close(state);
	mosquitto_publish(state->mqtt->mosq, NULL, "xacs/xkeys/events/dev", 4, "GONE", 1, false);
}



int main(int argc, char **argv)
{
	signal(SIGSEGV, handler);

	xkeys_state_t state;
	memset(&state, 0, sizeof(state));

	state.mqtt = mqtt_init("xkeys", true, "::");
	mqtt_add_sub(state.mqtt, "xacs/xkeys/cmd", &state, parseCmd);

	state.udev = udev_init(&state, udev_add_cb, udev_remove_cb);

	int ret = uv_run(uv_default_loop(), UV_RUN_DEFAULT);
	mqtt_close(state.mqtt);
	udev_cleanup(state.udev);
	return ret;
}



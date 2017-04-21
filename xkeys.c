/***************************************************
 libtool --mode=link gcc -o test1 -Ivendor/xkeys/piehid -Ivendor/libuv/include vendor/xkeys/piehid/hid-hidraw.c vendor/xkeys/piehid/PieHid32.c $(pkg-config --libs --cflags libusb-1.0) vendor/libuv/libuv.la -lpthread  -lmosquitto -ludev test1.c -static-libtool-libs
 libtool --mode=link gcc -o test1 -Ivendor/libuv/include  vendor/libuv/libuv.la -lpthread  -lmosquitto  test1.c -static-libtool-libs
***************************************************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <uv.h>
#include <mosquitto.h>

#define REPORT_SIZE 80   /* max size of a single usb report */


typedef struct async_data {
	char *data;
	int len;
	struct async_data *next;
} async_data_t;

typedef struct xkeys_state {
	struct mosquitto *mosq;
	uv_device_t *hidraw;
	uint8_t lastz;
	int got_lastz;

	uv_async_t async;
	async_data_t *adata;
	uv_mutex_t adata_m;
} xkeys_state_t;

typedef struct write_data {
	uv_write_t req;
	uv_buf_t bufs[1];
} write_data_t;


uv_loop_t *loop;

const char *mqtt_broker_host = "localhost";
const int mqtt_broker_port = 1883;
const int mqtt_keepalive = 10;


void mqtt_cb_msg(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg);
void mqtt_cb_connect(struct mosquitto *mosq, void *userdata, int result);
void mqtt_cb_subscribe(struct mosquitto *mosq, void *userdata, int mid, int qos_count, const int *granted_qos);
void mqtt_cb_disconnect(struct mosquitto *mosq, void *userdat, int rc);
void mqtt_cb_log(struct mosquitto *mosq, void *userdata, int level, const char *str);

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

void async_data_add(xkeys_state_t *state, const char *data, const int len){
	async_data_t *n = (async_data_t*) malloc(sizeof(async_data_t) + len);
	n->len = len;
	n->data = (char*)(n + 1);
	n->next = NULL;
	memcpy(n->data, data, len);
	uv_mutex_lock(&state->adata_m);
	if( state->adata == NULL ){
		state->adata = n;
	}else{
		async_data_t *p = state->adata;
		while( p->next != NULL ){
			p = p->next;
		}
		p->next = n;
	}
	uv_mutex_unlock(&state->adata_m);
}

async_data_t * async_data_get(xkeys_state_t *state){
	uv_mutex_lock(&state->adata_m);
	async_data_t *d = state->adata;
	if( d ){
		state->adata = d->next;
		d->next = NULL;
	}
	uv_mutex_unlock(&state->adata_m);
	return d;
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

	// printf("\n[BUF]%s\n", buf);

	mosquitto_publish(state->mosq, NULL, "xacs/xkeys/events", strlen(buf), buf, 1, false);
}










void parseCmd(xkeys_state_t *state, char *msg, int msglen){
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
		write_data_t *d = (write_data_t*) malloc(sizeof(write_data_t) + sizeof(report));
		d->req.data = d;
		d->bufs[0].base = (void*)(d + 1);
		d->bufs[0].len = sizeof(report);
		memcpy(d->bufs[0].base, report, 36);
		uv_write(&d->req, (uv_stream_t*)state->hidraw, d->bufs, 1, hidraw_write);
	}
}




































void mosq_thread_read(uv_async_t *uv){
	// printf("mosq_thread_read\n");
	xkeys_state_t *state = (xkeys_state_t*) uv->data;
	async_data_t *d = async_data_get(state);
	while( d ){
		parseCmd(state, d->data, d->len);
		free(d);
		d = async_data_get(state);
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

int main(int argc, char **argv){

	xkeys_state_t state;
	memset(&state, 0, sizeof(state));

	loop = uv_default_loop();

	char *hiddev;
	if( argc > 1 ){
		hiddev = argv[1];
	}else{
		hiddev = "/dev/hidraw0";
	}
	printf("Opening %s\n", hiddev);


    char clientid[24];
    mosquitto_lib_init();
    memset(clientid, 0, 24);
    snprintf(clientid, 23, "xkeys_%d", getpid());
    state.mosq = mosquitto_new(clientid, true, (void*)&state);
    if(!state.mosq){
        fprintf(stderr, "Error: Out of memory.\n");
        return -1;
    }
    mosquitto_connect_callback_set(state.mosq, mqtt_cb_connect);
    mosquitto_message_callback_set(state.mosq, mqtt_cb_msg);
    // mosquitto_subscribe_callback_set(state.mosq, mqtt_cb_subscribe);
    // mosquitto_disconnect_callback_set(state.mosq, mqtt_cb_disconnect);
    // mosquitto_log_callback_set(state.mosq, mqtt_cb_log);

    // int running = 1;
    while(1) {
        if(mosquitto_connect_bind(state.mosq, mqtt_broker_host, mqtt_broker_port, mqtt_keepalive, "::")){
            printf("Unable to connect, host: %s, port: %d\n",
                   mqtt_broker_host, mqtt_broker_port);
            sleep(2);
            continue;
        }
        break;
    }




    uv_device_t hidraw;
    hidraw.data = (void*)&state;
    state.hidraw = &hidraw;
    uv_device_init(loop, &hidraw, hiddev, O_RDWR);
    uv_read_start((uv_stream_t*)&hidraw, hidraw_alloc, hidraw_read);


    state.async.data = (void*)&state;
    uv_mutex_init(&state.adata_m);
    uv_async_init(loop, &state.async, mosq_thread_read);
    mosquitto_loop_start(state.mosq);


	int ret = uv_run(loop, UV_RUN_DEFAULT);
	mosquitto_destroy(state.mosq);
	mosquitto_lib_cleanup();
	return ret;
}


/* Called when a message arrives to the subscribed topic,
 */
void mqtt_cb_msg(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg){
	xkeys_state_t *state = (xkeys_state_t*)userdata;
	int i;

	if( strcmp(msg->topic, "xacs/xkeys/cmd") == 0 ){
		// printf("\n[CMD]%s\n", msg->payload);
		async_data_add(state, msg->payload, msg->payloadlen);
		uv_async_send(&state->async);
	// }else{
	// 	printf("\nReceived msg on topic: %s\n", msg->topic);
	// 	if(msg->payload != NULL){
	// 	    printf("Payload: %s\n", (char *) msg->payload);
	// 	}
	}
}

void mqtt_cb_connect(struct mosquitto *mosq, void *userdata, int result){
    if(!result){
        mosquitto_subscribe(mosq, NULL, "xacs/xkeys/cmd/#", 1);
    }
    else {
        printf("\nMQTT subscribe failed\n");
    }
}

// void mqtt_cb_subscribe(struct mosquitto *mosq, void *userdata, int mid,
//                         int qos_count, const int *granted_qos)
// {
// 	int i;
//     printf("\nSubscribed (mid: %d): %d\n", mid, granted_qos[0]);
//     for(i=1; i<qos_count; i++){
//         printf("\t %d", granted_qos[i]);
//     }
// }

// void
// mqtt_cb_disconnect(struct mosquitto *mosq, void *userdat, int rc)
// {
//     printf("\nMQTT disconnect, error: %d: %s\n",rc, mosquitto_strerror(rc));
// }

// void
// mqtt_cb_log(struct mosquitto *mosq, void *userdata,
//                   int level, const char *str)
// {
//     switch(level){
//         case MOSQ_LOG_DEBUG:
//             // printf("DBG: %s\n",str);
//             break;
//         case MOSQ_LOG_INFO:
//         case MOSQ_LOG_NOTICE:
//             printf("INF: %s\n",str);
//             break;
//         case MOSQ_LOG_WARNING:
//             printf("WRN: %s\n",str);
//             break;
//         case MOSQ_LOG_ERR:
//             printf("ERR: %s\n",str);
//             break;
//         default:
//             printf("Unknown MOSQ loglevel!");
//     }
// }

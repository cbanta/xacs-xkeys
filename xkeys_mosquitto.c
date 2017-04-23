

#include "xkeys.h"
#include <mosquitto.h>


const char *mqtt_broker_host = "localhost";
const int mqtt_broker_port = 1883;
const int mqtt_keepalive = 10;





static void async_data_add(
	struct mqtt_state *state, const char *data, const int len,
	async_data_cb_t cb, void *userdata)
{
	// printf("ASYNC: Adding (%d) %.*s\n", len, len, data);
	struct async_data *n = (struct async_data*) calloc(1, sizeof(struct async_data) + len+1);
	n->len = len+1;
	n->data = (char*)(n + 1);
	n->cb = cb;
	n->userdata = userdata;
	memcpy(n->data, data, len);

	uv_mutex_lock(&state->mutex);
	if( state->async_data == NULL ){
		state->async_data = n;
	}else{
		struct async_data *p = state->async_data;
		while( p->next != NULL ){
			p = p->next;
		}
		p->next = n;
	}
	uv_mutex_unlock(&state->mutex);
}

static struct async_data * async_data_get(struct mqtt_state *state){
	// printf("ASYNC: Popping data\n");
	uv_mutex_lock(&state->mutex);
	struct async_data *d = state->async_data;
	if( d ){
		state->async_data = d->next;
		d->next = NULL;
	}
	uv_mutex_unlock(&state->mutex);
	return d;
}



static void mosq_thread_read(uv_async_t *uv)
{
	struct mqtt_state *state = (struct mqtt_state *)uv->data;
	// printf("mosq_thread_read\n");
	struct async_data *d = async_data_get(state);
	while( d ){
		d->cb(d->userdata, d->data, d->len);
		free(d);
		d = async_data_get(state);
	}
}


/* Called when a message arrives to the subscribed topic,
 */
static void mqtt_cb_msg(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg)
{
	struct mqtt_state *state = (struct mqtt_state *)userdata;
	// printf("MQTT TOPIC %s MSG(%d): %.*s\n", msg->topic, msg->payloadlen, msg->payloadlen, msg->payload);
	struct mqtt_subscription *s = state->subs;
	while(s){
		if( strcmp(msg->topic, s->sub) == 0 ){
			async_data_add(state, msg->payload, msg->payloadlen, s->cb, s->userdata);
			uv_async_send(&state->uv);
			return;
		}
		s = s->next;
	}
}

static void mqtt_cb_connect(struct mosquitto *mosq, void *userdata, int result)
{
	struct mqtt_state *state = (struct mqtt_state *)userdata;
	// printf("MQTT Connect\n");
    if(!result){
    	state->is_connected = true;
    	struct mqtt_subscription *s = state->subs;
    	while(s){
    		mosquitto_subscribe(mosq, &s->mid, s->sub, 1);
    		s = s->next;
    	}
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

static void
mqtt_cb_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
	struct mqtt_state *state = (struct mqtt_state *)userdata;
	state->is_connected = false;
    printf("MQTT disconnect, error: %d: %s\n",rc, mosquitto_strerror(rc));
}

static void
mqtt_cb_log(struct mosquitto *mosq, void *userdata,
                  int level, const char *str)
{
    switch(level){
        case MOSQ_LOG_DEBUG:
            printf("DBG: %s\n",str);
            break;
        case MOSQ_LOG_INFO:
        case MOSQ_LOG_NOTICE:
            printf("INF: %s\n",str);
            break;
        case MOSQ_LOG_WARNING:
            printf("WRN: %s\n",str);
            break;
        case MOSQ_LOG_ERR:
            printf("ERR: %s\n",str);
            break;
        default:
            printf("Unknown MOSQ loglevel!");
    }
}


void mqtt_add_sub(struct mqtt_state *state, const char *sub, void *userdata, async_data_cb_t cb)
{
	int sublen = strlen(sub);
	struct mqtt_subscription *n = calloc(1, sizeof(struct mqtt_subscription) + sublen+1);
	n->sub = (char*)(n+1);
	memcpy(n->sub, sub, sublen);
	n->userdata = userdata;
	n->cb = cb;
	n->next = NULL;

	struct mqtt_subscription *s = state->subs;
	if( !s ){
		state->subs = n;
	}else{
		while(s){
			s = s->next;
		}
		s->next = n;
	}

	if( state->is_connected ){
		mosquitto_subscribe(state->mosq, &n->mid, n->sub, 1);
	}
}


struct mqtt_state *mqtt_init(const char *prefix, bool clean, const char *bind)
{
	struct mqtt_state *state;

	state = calloc(1, sizeof(struct mqtt_state));
	if( !state ) return NULL;

    char clientid[24];

    mosquitto_lib_init();
    memset(clientid, 0, 24);
    snprintf(clientid, 23, "%s_%d", prefix, getpid());
    state->mosq = mosquitto_new(clientid, clean, (void*)state);
    if(!state->mosq){
        fprintf(stderr, "Error: Out of memory.\n");
        return NULL;
    }
    mosquitto_connect_callback_set(state->mosq, mqtt_cb_connect);
    mosquitto_message_callback_set(state->mosq, mqtt_cb_msg);
    // mosquitto_subscribe_callback_set(state->mosq, mqtt_cb_subscribe);
    mosquitto_disconnect_callback_set(state->mosq, mqtt_cb_disconnect);
    // mosquitto_log_callback_set(state->mosq, mqtt_cb_log);

    mosquitto_connect_bind_async(
    	state->mosq, mqtt_broker_host, mqtt_broker_port, mqtt_keepalive, "::"
    );

    state->uv.data = (void*)state;
    uv_mutex_init(&state->mutex);
    uv_async_init(uv_default_loop(), &state->uv, mosq_thread_read);
    mosquitto_loop_start(state->mosq);

	return state;
}

void mqtt_close(struct mqtt_state *state)
{
	mosquitto_loop_stop(state->mosq, false);
	mosquitto_destroy(state->mosq);
	mosquitto_lib_cleanup();
	uv_close((uv_handle_t *)&state->uv, NULL);
	uv_mutex_destroy(&state->mutex);
	struct async_data *d = state->async_data;
	while( d ){
		struct async_data *dd = d;
		d = d->next;
		free(dd);
	}
	free(state);
}

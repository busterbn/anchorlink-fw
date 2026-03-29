/*
 * MQTT transport module using raw Zephyr MQTT client API.
 * Handles connection, LWT, publishing state, subscribing to commands,
 * and parsing incoming commands.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/random/random.h>
#include <cJSON.h>

#include <zephyr/shell/shell.h>

#include "client_id.h"
#include "message_channel.h"

LOG_MODULE_REGISTER(transport, CONFIG_MQTT_SAMPLE_TRANSPORT_LOG_LEVEL);

ZBUS_SUBSCRIBER_DEFINE(transport, CONFIG_MQTT_SAMPLE_TRANSPORT_MESSAGE_QUEUE_SIZE);

/* Forward declarations */
static const struct smf_state state[];
static void connect_work_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

K_THREAD_STACK_DEFINE(stack_area, CONFIG_MQTT_SAMPLE_TRANSPORT_WORKQUEUE_STACK_SIZE);

static struct k_work_q transport_queue;

/* Internal states */
enum module_state { MQTT_CONNECTED, MQTT_DISCONNECTED };

/* MQTT client and buffers */
static struct mqtt_client client;
static uint8_t rx_buffer[CONFIG_MQTT_SAMPLE_TRANSPORT_RX_TX_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_SAMPLE_TRANSPORT_RX_TX_BUFFER_SIZE];

/* Broker address */
static struct sockaddr_storage broker_addr;

/* TLS config */
static sec_tag_t sec_tags[] = { CONFIG_MQTT_SAMPLE_TRANSPORT_SEC_TAG };

/* Client ID */
static char client_id[CONFIG_MQTT_SAMPLE_TRANSPORT_CLIENT_ID_BUFFER_SIZE];

/* Topics */
static char state_topic[sizeof(client_id) + sizeof("/state")];
static char cmd_topic[sizeof(client_id) + sizeof("/cmd")];
static char status_topic[sizeof(client_id) + sizeof("/status")];

/* LWT data — must be static since mqtt_client holds pointers */
static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_message;
static uint8_t will_payload[] = "offline";

/* Username/password — static storage for mqtt_client pointers */
static struct mqtt_utf8 username;
static struct mqtt_utf8 password;

/* Poll thread */
static struct pollfd fds[1];
static int nfds;
static bool mqtt_connected;

/* Buffer for incoming publish payload */
static char cmd_payload_buf[256];

/*
 * Data usage tracking.
 *
 * MQTT packet overhead (bytes per message, excluding TLS):
 *   PUBLISH:   2 + topic_len + 2 (topic length field) + payload_len
 *              + 2 (packet id, QoS 1+)
 *   PINGREQ:   2
 *   PINGRESP:  2
 *   PUBACK:    4
 *   SUBACK:    4-5
 *   SUBSCRIBE: 2 + 2 + topic_len + 2 + 1
 *
 * We track: app payload bytes, estimated MQTT framing, and keepalive pings.
 */
static uint32_t app_tx;
static uint32_t app_rx;
static uint32_t mqtt_overhead_tx;
static uint32_t mqtt_overhead_rx;
static uint32_t ping_count;

static int cmd_data_stats(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t total_tx = app_tx + mqtt_overhead_tx;
	uint32_t total_rx = app_rx + mqtt_overhead_rx;

	shell_print(sh, "--- Data usage since last reset ---");
	shell_print(sh, "App payload:    TX %u bytes  RX %u bytes", app_tx, app_rx);
	shell_print(sh, "MQTT overhead:  TX %u bytes  RX %u bytes", mqtt_overhead_tx, mqtt_overhead_rx);
	shell_print(sh, "MQTT total:     TX %u bytes  RX %u bytes", total_tx, total_rx);
	shell_print(sh, "Keepalives:     %u pings (%u bytes TX + %u bytes RX)",
		    ping_count, ping_count * 2, ping_count * 2);

	return 0;
}

static int cmd_data_reset(const struct shell *sh, size_t argc, char **argv)
{
	app_tx = 0;
	app_rx = 0;
	mqtt_overhead_tx = 0;
	mqtt_overhead_rx = 0;
	ping_count = 0;

	shell_print(sh, "Counters reset");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(data_cmds,
	SHELL_CMD(stats, NULL, "Show data usage since last reset", cmd_data_stats),
	SHELL_CMD(reset, NULL, "Reset data usage counters", cmd_data_reset),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(data, &data_cmds, "Data usage commands", NULL);

enum transport_event_type {
	CONNECTED,
	DISCONNECTED,
};

struct transport_event {
	enum transport_event_type type;
};

ZBUS_CHAN_DEFINE(TRANSPORT_PRIVATE_CHANNEL,
		 struct transport_event,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(transport),
		 ZBUS_MSG_INIT(0)
);

static struct s_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	enum network_status status;
	struct payload payload;
} s_obj;

/* Build topics from client_id (IMEI) */
static int topics_build(void)
{
	int len;

	len = snprintk(state_topic, sizeof(state_topic), "%s/state", client_id);
	if (len < 0 || len >= sizeof(state_topic)) {
		return -EMSGSIZE;
	}

	len = snprintk(cmd_topic, sizeof(cmd_topic), "%s/cmd", client_id);
	if (len < 0 || len >= sizeof(cmd_topic)) {
		return -EMSGSIZE;
	}

	len = snprintk(status_topic, sizeof(status_topic), "%s/status", client_id);
	if (len < 0 || len >= sizeof(status_topic)) {
		return -EMSGSIZE;
	}

	return 0;
}

/* Serialize payload to JSON string, returns length or negative error */
static int payload_to_json(const struct payload *p, char *buf, size_t buf_size)
{
	/* snprintk doesn't support %f, so format floats as fixed-point integers */
	int voltage_int = (int)p->voltage;
	int voltage_frac = (int)((p->voltage - voltage_int) * 100);
	if (voltage_frac < 0) { voltage_frac = -voltage_frac; }

	int power_int = (int)p->power_w;
	int power_frac = (int)((p->power_w - power_int) * 10);
	if (power_frac < 0) { power_frac = -power_frac; }

	return snprintk(buf, buf_size,
		"{\"voltage\":%d.%02d,\"power_w\":%d.%d,"
		"\"relays\":[%s,%s,%s,%s,%s],\"ts\":%lld}",
		voltage_int, voltage_frac,
		power_int, power_frac,
		p->relays[0] ? "true" : "false",
		p->relays[1] ? "true" : "false",
		p->relays[2] ? "true" : "false",
		p->relays[3] ? "true" : "false",
		p->relays[4] ? "true" : "false",
		p->ts);
}

/* Publish a message on a given topic */
static int mqtt_publish_msg(const char *topic, const uint8_t *data, size_t len,
			    enum mqtt_qos qos, bool retain)
{
	struct mqtt_publish_param param = {
		.message.topic.topic.utf8 = (uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.topic.qos = qos,
		.message.payload.data = (uint8_t *)data,
		.message.payload.len = len,
		.message_id = (qos > MQTT_QOS_0_AT_MOST_ONCE) ? sys_rand32_get() : 0,
		.retain_flag = retain ? 1 : 0,
	};

	int err = mqtt_publish(&client, &param);
	if (err) {
		LOG_WRN("mqtt_publish failed: %d", err);
	} else {
		app_tx += len;
		/* MQTT PUBLISH overhead: 2 fixed header + 2 topic len + topic + (2 pkt id if qos>0) */
		mqtt_overhead_tx += 2 + 2 + strlen(topic) + (qos > MQTT_QOS_0_AT_MOST_ONCE ? 2 : 0);
		LOG_INF("Published on %s (%u bytes)", topic, len);
	}
	return err;
}

/* Publish state payload as JSON */
static void publish_state(struct payload *p)
{
	char json_buf[256];
	int len = payload_to_json(p, json_buf, sizeof(json_buf));

	if (len > 0 && len < sizeof(json_buf)) {
		mqtt_publish_msg(state_topic, (uint8_t *)json_buf, len,
				 MQTT_QOS_0_AT_MOST_ONCE, true);
	}
}

/* Publish online status */
static void publish_online(void)
{
	mqtt_publish_msg(status_topic, (uint8_t *)"online", 6,
			 MQTT_QOS_1_AT_LEAST_ONCE, true);
}

/* Subscribe to command topic */
static void subscribe_cmd(void)
{
	struct mqtt_topic topics[] = {
		{
			.topic.utf8 = (uint8_t *)cmd_topic,
			.topic.size = strlen(cmd_topic),
			.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		},
	};
	struct mqtt_subscription_list list = {
		.list = topics,
		.list_count = ARRAY_SIZE(topics),
		.message_id = sys_rand32_get(),
	};

	LOG_INF("Subscribing to: %s", cmd_topic);
	int err = mqtt_subscribe(&client, &list);
	if (err) {
		LOG_ERR("mqtt_subscribe failed: %d", err);
	} else {
		/* SUBSCRIBE: 2 fixed + 2 msg id + 2 topic len + topic + 1 qos */
		mqtt_overhead_tx += 2 + 2 + 2 + strlen(cmd_topic) + 1;
	}
}

/* Parse and dispatch incoming command */
static void handle_command(const char *data, size_t len)
{
	cJSON *root = cJSON_ParseWithLength(data, len);
	if (!root) {
		LOG_WRN("Failed to parse command JSON");
		return;
	}

	cJSON *action = cJSON_GetObjectItem(root, "action");
	if (!cJSON_IsString(action)) {
		LOG_WRN("Missing or invalid 'action' field");
		cJSON_Delete(root);
		return;
	}

	struct command cmd = { 0 };

	if (strcmp(action->valuestring, "set_relay") == 0) {
		cJSON *relay = cJSON_GetObjectItem(root, "relay");
		cJSON *state = cJSON_GetObjectItem(root, "state");

		if (!cJSON_IsNumber(relay) || !cJSON_IsBool(state)) {
			LOG_WRN("Invalid set_relay command");
			cJSON_Delete(root);
			return;
		}

		cmd.action = CMD_SET_RELAY;
		cmd.relay = relay->valueint - 1; /* 1-indexed to 0-indexed */
		cmd.state = cJSON_IsTrue(state);

	} else if (strcmp(action->valuestring, "start_stream") == 0) {
		cmd.action = CMD_START_STREAM;

	} else if (strcmp(action->valuestring, "stop_stream") == 0) {
		cmd.action = CMD_STOP_STREAM;

	} else {
		LOG_WRN("Unknown action: %s", action->valuestring);
		cJSON_Delete(root);
		return;
	}

	cJSON_Delete(root);

	int err = zbus_chan_pub(&CMD_CHAN, &cmd, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish command: %d", err);
	}
}

/* MQTT event handler */
static void mqtt_evt_handler(struct mqtt_client *const cli,
			     const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK: {
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed: %d", evt->result);
			break;
		}

		struct transport_event event = { .type = CONNECTED };
		zbus_chan_pub(&TRANSPORT_PRIVATE_CHANNEL, &event, K_SECONDS(1));
		break;
	}

	case MQTT_EVT_DISCONNECT: {
		struct transport_event event = { .type = DISCONNECTED };
		zbus_chan_pub(&TRANSPORT_PRIVATE_CHANNEL, &event, K_SECONDS(1));
		break;
	}

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *pub = &evt->param.publish;
		size_t payload_len = pub->message.payload.len;

		if (payload_len >= sizeof(cmd_payload_buf)) {
			LOG_WRN("Command payload too large: %zu", payload_len);
			/* Read and discard */
			mqtt_read_publish_payload_blocking(cli, cmd_payload_buf,
							   sizeof(cmd_payload_buf) - 1);
			break;
		}

		int bytes = mqtt_read_publish_payload_blocking(cli, cmd_payload_buf, payload_len);
		if (bytes < 0) {
			LOG_ERR("Failed to read publish payload: %d", bytes);
			break;
		}
		cmd_payload_buf[bytes] = '\0';

		app_rx += bytes;
		/* MQTT PUBLISH overhead: 2 fixed + 2 topic len + topic + (2 pkt id if qos>0) */
		mqtt_overhead_rx += 2 + 2 + pub->message.topic.topic.size
			+ (pub->message.topic.qos > MQTT_QOS_0_AT_MOST_ONCE ? 2 : 0);
		LOG_INF("Received command (%d bytes): %s", bytes, cmd_payload_buf);
		handle_command(cmd_payload_buf, bytes);

		/* Send PUBACK for QoS 1 */
		if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = pub->message_id,
			};
			mqtt_publish_qos1_ack(cli, &ack);
		}
		break;
	}

	case MQTT_EVT_SUBACK:
		mqtt_overhead_rx += 5; /* SUBACK: 2 fixed + 2 msg id + 1 return code */
		LOG_INF("Subscription acknowledged, id: %d", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PUBACK:
		mqtt_overhead_rx += 4; /* PUBACK: 2 fixed + 2 msg id */
		LOG_DBG("PUBACK id: %d", evt->param.puback.message_id);
		break;

	case MQTT_EVT_PINGRESP:
		ping_count++;
		mqtt_overhead_tx += 2; /* PINGREQ */
		mqtt_overhead_rx += 2; /* PINGRESP */
		LOG_DBG("PINGRESP");
		break;

	default:
		break;
	}
}

/* Resolve broker hostname */
static int broker_resolve(void)
{
	struct addrinfo *result;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};

	int err = getaddrinfo(CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_HOSTNAME, NULL, &hints, &result);
	if (err) {
		LOG_ERR("getaddrinfo failed: %d", err);
		return -EFAULT;
	}

	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker_addr;
	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_PORT);
	broker4->sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;

	freeaddrinfo(result);

	char addr_str[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &broker4->sin_addr, addr_str, sizeof(addr_str));
	LOG_INF("Broker resolved: %s:%d", addr_str, CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_PORT);

	return 0;
}

/* Initialize and configure the MQTT client */
static int mqtt_client_setup(void)
{
	mqtt_client_init(&client);

	/* Broker address */
	client.broker = &broker_addr;

	/* Event handler */
	client.evt_cb = mqtt_evt_handler;

	/* Client ID */
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen(client_id);

	/* Username & password */
	username.utf8 = (uint8_t *)CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_USERNAME;
	username.size = strlen(CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_USERNAME);
	client.user_name = &username;

	password.utf8 = (uint8_t *)CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_PASSWORD;
	password.size = strlen(CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_PASSWORD);
	client.password = &password;

	/* Buffers */
	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

	/* Protocol version */
	client.protocol_version = MQTT_VERSION_3_1_1;

	/* Clean session */
	client.clean_session = IS_ENABLED(CONFIG_MQTT_CLEAN_SESSION) ? 1 : 0;

	/* LWT */
	will_topic.topic.utf8 = (uint8_t *)status_topic;
	will_topic.topic.size = strlen(status_topic);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	client.will_topic = &will_topic;

	will_message.utf8 = will_payload;
	will_message.size = sizeof(will_payload) - 1;
	client.will_message = &will_message;
	client.will_retain = 1;

	/* TLS */
	client.transport.type = MQTT_TRANSPORT_SECURE;
	struct mqtt_sec_config *tls = &client.transport.tls.config;
	tls->peer_verify = TLS_PEER_VERIFY_NONE;
	tls->sec_tag_list = sec_tags;
	tls->sec_tag_count = ARRAY_SIZE(sec_tags);
	tls->hostname = CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_HOSTNAME;

	return 0;
}

static void prepare_fds(void)
{
	fds[0].fd = client.transport.tls.sock;
	fds[0].events = POLLIN;
	nfds = 1;
}

/* Poll loop for MQTT — runs in its own thread */
static void mqtt_poll_loop(void)
{
	while (mqtt_connected) {
		int timeout = mqtt_keepalive_time_left(&client);
		int ret = poll(fds, nfds, (timeout > 0) ? timeout : 1000);

		if (ret > 0 && (fds[0].revents & POLLIN)) {
			mqtt_input(&client);
		}

		mqtt_live(&client);
	}
}

static K_THREAD_STACK_DEFINE(poll_stack, 2048);
static struct k_thread poll_thread;

static void poll_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	mqtt_poll_loop();
}

/* Connect work */
static void connect_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = client_id_get(client_id, sizeof(client_id));
	if (err) {
		LOG_ERR("client_id_get failed: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = topics_build();
	if (err) {
		LOG_ERR("topics_build failed: %d", err);
		SEND_FATAL_ERROR();
		return;
	}

	err = broker_resolve();
	if (err) {
		LOG_ERR("broker_resolve failed: %d", err);
		k_work_reschedule_for_queue(&transport_queue, &connect_work,
			K_SECONDS(CONFIG_MQTT_SAMPLE_TRANSPORT_RECONNECTION_TIMEOUT_SECONDS));
		return;
	}

	mqtt_client_setup();

	err = mqtt_connect(&client);
	if (err) {
		LOG_ERR("mqtt_connect failed: %d", err);
		k_work_reschedule_for_queue(&transport_queue, &connect_work,
			K_SECONDS(CONFIG_MQTT_SAMPLE_TRANSPORT_RECONNECTION_TIMEOUT_SECONDS));
		return;
	}

	/* Start poll thread */
	mqtt_connected = true;
	prepare_fds();
	k_thread_create(&poll_thread, poll_stack, K_THREAD_STACK_SIZEOF(poll_stack),
			poll_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
}

/* SMF state handlers */

static void disconnected_entry(void *o)
{
	struct s_object *user_object = o;

	mqtt_connected = false;
	nfds = 0;

	if (user_object->status == NETWORK_CONNECTED) {
		k_work_reschedule_for_queue(&transport_queue, &connect_work, K_NO_WAIT);
	}
}

static enum smf_state_result disconnected_run(void *o)
{
	struct s_object *user_object = o;

	if ((user_object->status == NETWORK_DISCONNECTED) && (user_object->chan == &NETWORK_CHAN)) {
		k_work_cancel_delayable(&connect_work);
	}

	if ((user_object->status == NETWORK_CONNECTED) && (user_object->chan == &NETWORK_CHAN)) {
		k_work_reschedule_for_queue(&transport_queue, &connect_work, K_SECONDS(5));
	}

	return SMF_EVENT_HANDLED;
}

static void connected_entry(void *o)
{
	ARG_UNUSED(o);

	LOG_INF("Connected to MQTT broker");
	LOG_INF("Hostname: %s", CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_HOSTNAME);
	LOG_INF("Client ID: %s", client_id);

	k_work_cancel_delayable(&connect_work);

	subscribe_cmd();
	publish_online();
}

static enum smf_state_result connected_run(void *o)
{
	struct s_object *user_object = o;

	if ((user_object->status == NETWORK_DISCONNECTED) && (user_object->chan == &NETWORK_CHAN)) {
		mqtt_connected = false;
		mqtt_disconnect(&client, NULL);
		return SMF_EVENT_HANDLED;
	}

	if (user_object->chan == &PAYLOAD_CHAN) {
		publish_state(&user_object->payload);
	}

	return SMF_EVENT_HANDLED;
}

static void connected_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_INF("Disconnected from MQTT broker");
}

static const struct smf_state state[] = {
	[MQTT_DISCONNECTED] = SMF_CREATE_STATE(disconnected_entry, disconnected_run, NULL,
					       NULL, NULL),
	[MQTT_CONNECTED] = SMF_CREATE_STATE(connected_entry, connected_run, connected_exit,
					    NULL, NULL),
};

static void transport_task(void)
{
	int err;
	const struct zbus_channel *chan;
	enum network_status status;
	struct payload payload;

	k_work_queue_init(&transport_queue);
	k_work_queue_start(&transport_queue, stack_area,
			   K_THREAD_STACK_SIZEOF(stack_area),
			   K_HIGHEST_APPLICATION_THREAD_PRIO, NULL);

	smf_set_initial(SMF_CTX(&s_obj), &state[MQTT_DISCONNECTED]);

	while (!zbus_sub_wait(&transport, &chan, K_FOREVER)) {

		s_obj.chan = chan;

		if (&NETWORK_CHAN == chan) {
			err = zbus_chan_read(&NETWORK_CHAN, &status, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_read error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}
			s_obj.status = status;
			smf_run_state(SMF_CTX(&s_obj));
		}

		if (&PAYLOAD_CHAN == chan) {
			err = zbus_chan_read(&PAYLOAD_CHAN, &payload, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_read error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}
			s_obj.payload = payload;
			smf_run_state(SMF_CTX(&s_obj));
		}

		if (&TRANSPORT_PRIVATE_CHANNEL == chan) {
			struct transport_event event;
			err = zbus_chan_read(&TRANSPORT_PRIVATE_CHANNEL, &event, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_read error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}

			switch (event.type) {
			case CONNECTED:
				smf_set_state(SMF_CTX(&s_obj), &state[MQTT_CONNECTED]);
				break;
			case DISCONNECTED:
				smf_set_state(SMF_CTX(&s_obj), &state[MQTT_DISCONNECTED]);
				break;
			}
		}
	}
}

K_THREAD_DEFINE(transport_task_id,
		CONFIG_MQTT_SAMPLE_TRANSPORT_THREAD_STACK_SIZE,
		transport_task, NULL, NULL, NULL, 3, 0, 0);

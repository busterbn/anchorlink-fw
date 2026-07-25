/*
 * MQTT transport module using raw Zephyr MQTT client API.
 *
 * All payloads are JSON. Topics:
 *   {imei}/relay1, {imei}/relay2          : retained relay state {"state":0|1}
 *   {imei}/bat                            : both batteries {"bat1":X.XX,"bat2":X.XX}
 *   {imei}/gps                            : on-demand GPS fix {"lat":..,"lon":..}
 *   {imei}/anchor-alarm                   : anchor alarm {"distance":<m>}
 *   {imei}/cmd/#                          : incoming commands {"cmd":"rel1"|"rel2"|
 *                                           "bat"|"gps"|"reboot"|"fota_update"|
 *                                           "anchor-alarm"} (+ "radius" for anchor)
 *   {imei}/pair                           : pairing request {"status":"ready"} (BTN0 long press)
 *   {imei}/status                         : {"status":"online"|"offline"} (LWT)
 *   {imei}/fw                             : firmware version {"version":"X.Y.Z"} (retained)
 *   {imei}/fota                           : {"status":"updating"} on FOTA start
 */

#include <zephyr/kernel.h>
#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/random/random.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/data/json.h>

#include <zephyr/shell/shell.h>

#include "client_id.h"
#include "message_channel.h"

LOG_MODULE_REGISTER(transport, CONFIG_MQTT_SAMPLE_TRANSPORT_LOG_LEVEL);

ZBUS_SUBSCRIBER_DEFINE(transport, CONFIG_MQTT_SAMPLE_TRANSPORT_MESSAGE_QUEUE_SIZE);

static const struct smf_state state[];
static void connect_work_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(connect_work, connect_work_fn);

K_THREAD_STACK_DEFINE(stack_area, CONFIG_MQTT_SAMPLE_TRANSPORT_WORKQUEUE_STACK_SIZE);

static struct k_work_q transport_queue;

enum module_state { MQTT_CONNECTED, MQTT_DISCONNECTED };

static struct mqtt_client client;
static uint8_t rx_buffer[CONFIG_MQTT_SAMPLE_TRANSPORT_RX_TX_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_SAMPLE_TRANSPORT_RX_TX_BUFFER_SIZE];

static struct sockaddr_storage broker_addr;

static sec_tag_t sec_tags[] = { CONFIG_MQTT_SAMPLE_TRANSPORT_SEC_TAG };

static char client_id[CONFIG_MQTT_SAMPLE_TRANSPORT_CLIENT_ID_BUFFER_SIZE];

/* Topic buffers */
static char relay1_topic[sizeof(client_id) + sizeof("/relay1")];
static char relay2_topic[sizeof(client_id) + sizeof("/relay2")];
static char bat_topic[sizeof(client_id) + sizeof("/bat")];
static char cmd_sub_topic[sizeof(client_id) + sizeof("/cmd/#")];
static char status_topic[sizeof(client_id) + sizeof("/status")];
static char gps_topic[sizeof(client_id) + sizeof("/gps")];
static char anchor_alarm_topic[sizeof(client_id) + sizeof("/anchor-alarm")];
static char pair_topic[sizeof(client_id) + sizeof("/pair")];
static char fw_topic[sizeof(client_id) + sizeof("/fw")];
static char fota_topic[sizeof(client_id) + sizeof("/fota")];

static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_message;
static uint8_t will_payload[] = "{\"status\":\"offline\"}";

static struct mqtt_utf8 username;
static struct mqtt_utf8 password;

static struct pollfd fds[1];
static int nfds;
static bool mqtt_connected;

static char cmd_payload_buf[64];

/* Data usage counters */
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
	struct publish_event pub;
} s_obj;

static int topics_build(void)
{
	if (snprintk(relay1_topic, sizeof(relay1_topic), "%s/relay1", client_id) >= sizeof(relay1_topic) ||
	    snprintk(relay2_topic, sizeof(relay2_topic), "%s/relay2", client_id) >= sizeof(relay2_topic) ||
	    snprintk(bat_topic,    sizeof(bat_topic),    "%s/bat",    client_id) >= sizeof(bat_topic) ||
	    snprintk(cmd_sub_topic, sizeof(cmd_sub_topic), "%s/cmd/#", client_id) >= sizeof(cmd_sub_topic) ||
	    snprintk(status_topic, sizeof(status_topic), "%s/status", client_id) >= sizeof(status_topic) ||
	    snprintk(gps_topic, sizeof(gps_topic), "%s/gps", client_id) >= sizeof(gps_topic) ||
	    snprintk(anchor_alarm_topic, sizeof(anchor_alarm_topic), "%s/anchor-alarm", client_id) >= sizeof(anchor_alarm_topic) ||
	    snprintk(pair_topic, sizeof(pair_topic), "%s/pair", client_id) >= sizeof(pair_topic) ||
	    snprintk(fw_topic, sizeof(fw_topic), "%s/fw", client_id) >= sizeof(fw_topic) ||
	    snprintk(fota_topic, sizeof(fota_topic), "%s/fota", client_id) >= sizeof(fota_topic)) {
		return -EMSGSIZE;
	}
	return 0;
}

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
		mqtt_overhead_tx += 2 + 2 + strlen(topic) + (qos > MQTT_QOS_0_AT_MOST_ONCE ? 2 : 0);
		LOG_INF("Published on %s: \"%.*s\" (%u bytes)",
			topic, (int)len, (const char *)data, (unsigned)len);
	}
	return err;
}

static void publish_relay(uint8_t idx, bool state)
{
	const char *topic = (idx == 0) ? relay1_topic : relay2_topic;
	char payload[16];
	int len = snprintk(payload, sizeof(payload), "{\"state\":%d}", state ? 1 : 0);
	mqtt_publish_msg(topic, (const uint8_t *)payload, len,
			 MQTT_QOS_1_AT_LEAST_ONCE, true);
}

/* Format a voltage as a bare JSON number "X.XX" without using %f. */
static int format_voltage(char *buf, size_t size, float v)
{
	int int_part = (int)v;
	int frac_part = (int)((v - int_part) * 100);
	if (frac_part < 0) { frac_part = -frac_part; }
	return snprintk(buf, size, "%d.%02d", int_part, frac_part);
}

static void publish_battery(float bat1_v, float bat2_v)
{
	char b1[8];
	char b2[8];
	char payload[48];

	format_voltage(b1, sizeof(b1), bat1_v);
	format_voltage(b2, sizeof(b2), bat2_v);
	int len = snprintk(payload, sizeof(payload),
			   "{\"bat1\":%s,\"bat2\":%s}", b1, b2);
	if (len > 0 && len < sizeof(payload)) {
		mqtt_publish_msg(bat_topic, (uint8_t *)payload, len,
				 MQTT_QOS_0_AT_MOST_ONCE, false);
	}
}

static void publish_online(void)
{
	const char *payload = "{\"status\":\"online\"}";
	mqtt_publish_msg(status_topic, (const uint8_t *)payload, strlen(payload),
			 MQTT_QOS_1_AT_LEAST_ONCE, true);
}

static void publish_fw_version(void)
{
	char payload[48];
	int len = snprintk(payload, sizeof(payload), "{\"version\":\"%s\"}",
			   CONFIG_MEMFAULT_NCS_FW_VERSION);
	mqtt_publish_msg(fw_topic, (const uint8_t *)payload, len,
			 MQTT_QOS_1_AT_LEAST_ONCE, true);
}

static void publish_fota_status(void)
{
	const char *payload = "{\"status\":\"updating\"}";
	mqtt_publish_msg(fota_topic, (const uint8_t *)payload, strlen(payload),
			 MQTT_QOS_1_AT_LEAST_ONCE, false);
}

/* Format a signed coordinate with 6 decimal places without using %f.
 * Latitude is in [-90,90], longitude in [-180,180], so int32_t is plenty.
 */
static int format_coord(char *buf, size_t size, double v)
{
	bool negative = (v < 0);
	if (negative) {
		v = -v;
	}
	int32_t scaled = (int32_t)(v * 1000000.0 + 0.5);
	int32_t int_part = scaled / 1000000;
	int32_t frac_part = scaled % 1000000;
	return snprintk(buf, size, "%s%d.%06d",
			negative ? "-" : "",
			(int)int_part, (int)frac_part);
}

static void publish_gps(double lat, double lon)
{
	char buf[48];
	char latbuf[16];
	char lonbuf[16];

	format_coord(latbuf, sizeof(latbuf), lat);
	format_coord(lonbuf, sizeof(lonbuf), lon);
	int len = snprintk(buf, sizeof(buf), "{\"lat\":%s,\"lon\":%s}", latbuf, lonbuf);
	if (len > 0 && len < sizeof(buf)) {
		mqtt_publish_msg(gps_topic, (uint8_t *)buf, len,
				 MQTT_QOS_0_AT_MOST_ONCE, false);
	}
}

static void publish_anchor_alarm_msg(uint32_t dist_m)
{
	char buf[28];
	int len = snprintk(buf, sizeof(buf), "{\"distance\":%u}", dist_m);
	if (len > 0 && len < sizeof(buf)) {
		mqtt_publish_msg(anchor_alarm_topic, (uint8_t *)buf, len,
				 MQTT_QOS_1_AT_LEAST_ONCE, false);
	}
}

static void publish_pair(void)
{
	const char *payload = "{\"status\":\"ready\"}";
	mqtt_publish_msg(pair_topic, (const uint8_t *)payload, strlen(payload),
			 MQTT_QOS_1_AT_LEAST_ONCE, false);
}

static void subscribe_cmd(void)
{
	struct mqtt_topic topics[] = {
		{
			.topic.utf8 = (uint8_t *)cmd_sub_topic,
			.topic.size = strlen(cmd_sub_topic),
			.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		},
	};
	struct mqtt_subscription_list list = {
		.list = topics,
		.list_count = ARRAY_SIZE(topics),
		.message_id = sys_rand32_get(),
	};

	LOG_INF("Subscribing to: %s", cmd_sub_topic);
	int err = mqtt_subscribe(&client, &list);
	if (err) {
		LOG_ERR("mqtt_subscribe failed: %d", err);
	} else {
		mqtt_overhead_tx += 2 + 2 + 2 + strlen(cmd_sub_topic) + 1;
	}
}

static void dispatch_command(struct command *cmd)
{
	int err = zbus_chan_pub(&CMD_CHAN, cmd, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish command: %d", err);
	}
}

/* Inbound command JSON, e.g. {"cmd":"rel1"} or
 * {"cmd":"anchor-alarm","radius":25}. `radius` is only used by anchor-alarm. */
struct cmd_json {
	const char *cmd;
	int32_t radius;
};

static const struct json_obj_descr cmd_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct cmd_json, cmd, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct cmd_json, radius, JSON_TOK_NUMBER),
};

/* Parse and dispatch a command. json_obj_parse() tokenizes the buffer in place,
 * so `data` must be writable (it is — the shared cmd_payload_buf). */
static void handle_cmd_payload(char *data, size_t len)
{
	struct cmd_json parsed = { .cmd = NULL, .radius = -1 };
	int ret = json_obj_parse(data, len, cmd_descr, ARRAY_SIZE(cmd_descr), &parsed);
	if (ret < 0 || !(ret & BIT(0)) || parsed.cmd == NULL) {
		LOG_WRN("Bad command JSON (ret %d)", ret);
		return;
	}

	struct command cmd = { 0 };
	const char *c = parsed.cmd;

	if (strcmp(c, "rel1") == 0) {
		cmd.action = CMD_TOGGLE_RELAY;
		cmd.relay = 0;
		dispatch_command(&cmd);
	} else if (strcmp(c, "rel2") == 0) {
		cmd.action = CMD_TOGGLE_RELAY;
		cmd.relay = 1;
		dispatch_command(&cmd);
	} else if (strcmp(c, "bat") == 0) {
		cmd.action = CMD_REPORT_BAT;
		dispatch_command(&cmd);
	} else if (strcmp(c, "gps") == 0) {
		cmd.action = CMD_REQUEST_GPS;
		dispatch_command(&cmd);
	} else if (strcmp(c, "fota_update") == 0) {
		cmd.action = CMD_FOTA_UPDATE;
		dispatch_command(&cmd);
	} else if (strcmp(c, "reboot") == 0) {
		cmd.action = CMD_REBOOT;
		dispatch_command(&cmd);
	} else if (strcmp(c, "anchor-alarm") == 0) {
		if (!(ret & BIT(1)) || parsed.radius < 0) {
			LOG_WRN("anchor-alarm: missing/negative radius");
			return;
		}
		cmd.action = CMD_SET_ANCHOR_ALARM;
		cmd.distance_m = (uint32_t)parsed.radius;
		dispatch_command(&cmd);
	} else {
		LOG_WRN("Unknown command: %s", c);
	}
}

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
		mqtt_overhead_rx += 2 + 2 + pub->message.topic.topic.size
			+ (pub->message.topic.qos > MQTT_QOS_0_AT_MOST_ONCE ? 2 : 0);
		LOG_INF("Received command (%d bytes): %s", bytes, cmd_payload_buf);
		handle_cmd_payload(cmd_payload_buf, bytes);

		if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			struct mqtt_puback_param ack = {
				.message_id = pub->message_id,
			};
			mqtt_publish_qos1_ack(cli, &ack);
		}
		break;
	}

	case MQTT_EVT_SUBACK:
		mqtt_overhead_rx += 5;
		LOG_INF("Subscription acknowledged, id: %d", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PUBACK:
		mqtt_overhead_rx += 4;
		LOG_DBG("PUBACK id: %d", evt->param.puback.message_id);
		break;

	case MQTT_EVT_PINGRESP:
		ping_count++;
		mqtt_overhead_tx += 2;
		mqtt_overhead_rx += 2;
		LOG_DBG("PINGRESP");
		break;

	default:
		break;
	}
}

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

static int mqtt_client_setup(void)
{
	mqtt_client_init(&client);

	client.broker = &broker_addr;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (uint8_t *)client_id;
	client.client_id.size = strlen(client_id);

	/* Use the device IMEI (same as client_id) as the username, so a whole
	 * fleet can share one firmware image yet authenticate as distinct users. */
	username.utf8 = (uint8_t *)client_id;
	username.size = strlen(client_id);
	client.user_name = &username;

	password.utf8 = (uint8_t *)CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_PASSWORD;
	password.size = strlen(CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_PASSWORD);
	client.password = &password;

	client.rx_buf = rx_buffer;
	client.rx_buf_size = sizeof(rx_buffer);
	client.tx_buf = tx_buffer;
	client.tx_buf_size = sizeof(tx_buffer);

	client.protocol_version = MQTT_VERSION_3_1_1;
	client.clean_session = IS_ENABLED(CONFIG_MQTT_CLEAN_SESSION) ? 1 : 0;

	will_topic.topic.utf8 = (uint8_t *)status_topic;
	will_topic.topic.size = strlen(status_topic);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	client.will_topic = &will_topic;

	will_message.utf8 = will_payload;
	will_message.size = sizeof(will_payload) - 1;
	client.will_message = &will_message;
	client.will_retain = 1;

	client.transport.type = MQTT_TRANSPORT_SECURE;
	struct mqtt_sec_config *tls = &client.transport.tls.config;
	tls->peer_verify = TLS_PEER_VERIFY_NONE;
	tls->sec_tag_list = sec_tags;
	tls->sec_tag_count = ARRAY_SIZE(sec_tags);
	tls->hostname = CONFIG_MQTT_SAMPLE_TRANSPORT_BROKER_HOSTNAME;
	/* Enable TLS session resumption. The modem caches the TLS session and
	 * resumes it on reconnect to the same host, skipping the full handshake
	 * (server cert exchange). On the constrained cellular link this turns a
	 * multi-KB, multi-second handshake into a tiny one, so reconnects are far
	 * less likely to hit the broker's ~60s connect timeout. Maps to the modem
	 * TLS_SESSION_CACHE / NRF_SO_SEC_SESSION_CACHE socket option. */
	tls->session_cache = TLS_SESSION_CACHE_ENABLED;

	return 0;
}

static void prepare_fds(void)
{
	fds[0].fd = client.transport.tls.sock;
	fds[0].events = POLLIN;
	nfds = 1;
}

static void mqtt_poll_loop(void)
{
	/* If the broker hasn't sent us anything (not even a PINGRESP) for
	 * 1.5x the keepalive interval, the connection is silently dead, e.g.
	 * the carrier NAT mapping expired. Treat it as a disconnect so we
	 * reconnect instead of spinning forever as "connected".
	 */
	const int64_t dead_after_ms = (int64_t)CONFIG_MQTT_KEEPALIVE * 1500;
	int64_t last_rx = k_uptime_get();

	while (mqtt_connected) {
		int timeout = mqtt_keepalive_time_left(&client);
		int ret = poll(fds, nfds, (timeout > 0) ? timeout : 1000);

		if (ret < 0) {
			LOG_ERR("poll() error: %d", errno);
			break;
		}
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			LOG_WRN("Socket error (revents 0x%x), dropping connection",
				fds[0].revents);
			break;
		}
		if ((ret > 0) && (fds[0].revents & POLLIN)) {
			if (mqtt_input(&client) != 0) {
				LOG_WRN("mqtt_input failed, dropping connection");
				break;
			}
			last_rx = k_uptime_get();
		}

		int err = mqtt_live(&client);
		if (err != 0 && err != -EAGAIN) {
			LOG_WRN("mqtt_live error: %d, dropping connection", err);
			break;
		}

		if ((k_uptime_get() - last_rx) > dead_after_ms) {
			LOG_WRN("No data from broker for >%lld ms, connection dead",
				dead_after_ms);
			break;
		}
	}
}

static K_THREAD_STACK_DEFINE(poll_stack, 2048);
static struct k_thread poll_thread;

static void poll_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	mqtt_poll_loop();

	/* If we left the loop while still flagged "connected", it was an error
	 * we detected (dead socket / NAT timeout), not an orderly shutdown.
	 * Force the connection down and tell the state machine to reconnect.
	 */
	if (mqtt_connected) {
		mqtt_connected = false;
		mqtt_abort(&client);
		struct transport_event event = { .type = DISCONNECTED };
		zbus_chan_pub(&TRANSPORT_PRIVATE_CHANNEL, &event, K_SECONDS(1));
	}
}

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

	mqtt_connected = true;
	prepare_fds();
	k_thread_create(&poll_thread, poll_stack, K_THREAD_STACK_SIZEOF(poll_stack),
			poll_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);
}

/* Send the current relay state for both relays (called on connect). */
extern bool relay_get(uint8_t idx);
static void publish_initial_relay_states(void)
{
	publish_relay(0, relay_get(0));
	publish_relay(1, relay_get(1));
}

static void publish_connection_status(enum connection_status status)
{
	zbus_chan_pub(&CONNECTION_CHAN, &status, K_SECONDS(1));
}

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

	/* A successful MQTT connect proves this image is healthy. Confirm it so
	 * MCUboot keeps it permanently; otherwise the next reboot reverts to the
	 * previous image. An image that can't connect is intentionally left
	 * unconfirmed so it gets rolled back. */
	if (!boot_is_img_confirmed()) {
		int err = boot_write_img_confirmed();
		if (err) {
			LOG_ERR("Failed to confirm image: %d", err);
		} else {
			LOG_INF("Image confirmed");
		}
	}

	publish_connection_status(CONNECTION_UP);

	k_work_cancel_delayable(&connect_work);

	subscribe_cmd();
	publish_online();
	publish_fw_version();
	publish_initial_relay_states();
}

static enum smf_state_result connected_run(void *o)
{
	struct s_object *user_object = o;

	if ((user_object->status == NETWORK_DISCONNECTED) && (user_object->chan == &NETWORK_CHAN)) {
		mqtt_connected = false;
		mqtt_disconnect(&client, NULL);
		return SMF_EVENT_HANDLED;
	}

	if (user_object->chan == &PUBLISH_CHAN) {
		switch (user_object->pub.type) {
		case PUB_RELAY_STATE:
			publish_relay(user_object->pub.relay, user_object->pub.state);
			break;
		case PUB_BAT_REPORT:
			publish_battery(user_object->pub.bat1_v, user_object->pub.bat2_v);
			break;
		case PUB_GPS:
			publish_gps(user_object->pub.latitude, user_object->pub.longitude);
			break;
		case PUB_ANCHOR_ALARM:
			publish_anchor_alarm_msg(user_object->pub.distance_m);
			publish_gps(user_object->pub.latitude, user_object->pub.longitude);
			break;
		case PUB_PAIR:
			publish_pair();
			break;
		case PUB_FOTA_STATUS:
			publish_fota_status();
			break;
		}
	}

	return SMF_EVENT_HANDLED;
}

static void connected_exit(void *o)
{
	ARG_UNUSED(o);
	LOG_INF("Disconnected from MQTT broker");
	publish_connection_status(CONNECTION_DOWN);
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
	struct publish_event pub;

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

		if (&PUBLISH_CHAN == chan) {
			err = zbus_chan_read(&PUBLISH_CHAN, &pub, K_SECONDS(1));
			if (err) {
				LOG_ERR("zbus_chan_read error: %d", err);
				SEND_FATAL_ERROR();
				return;
			}
			s_obj.pub = pub;
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

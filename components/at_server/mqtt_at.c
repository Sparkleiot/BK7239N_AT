#include <components/log.h>
#include <components/netif.h>
#include <modules/wifi.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <os/os.h>
#include <os/str.h>

#include "atsvr_unite.h"
#include "mqtt_at.h"
#include "iot_export_mqtt.h"
#include "lite-system.h"

#define TAG "AT_MQTT"

#define AT_MQTT_LINK_ID                 0
#define AT_MQTT_HOST_MAX_LEN            127
#define AT_MQTT_CLIENT_ID_MAX_LEN       127
#define AT_MQTT_USERNAME_MAX_LEN        127
#define AT_MQTT_PASSWORD_MAX_LEN        127
#define AT_MQTT_TOPIC_MAX_LEN           127
#define AT_MQTT_SUB_TOPIC_MAX           8
#define AT_MQTT_IOBUF_LEN               2048
#define AT_MQTT_EVENT_BUF_LEN           512
#define AT_MQTT_DEFAULT_KEEPALIVE_S     60
#define AT_MQTT_PRODUCT_KEY             "sparkleiot"
#define AT_MQTT_PUBRAW_MAX_LEN          (AT_MQTT_IOBUF_LEN - 256)

typedef struct {
    uint8_t used;
    uint8_t qos;
    char topic[AT_MQTT_TOPIC_MAX_LEN + 1];
} at_mqtt_sub_topic_t;

typedef struct {
    uint8_t configured;
    uint8_t connected;
    uint8_t clean_session;
    uint8_t suppress_disconnect_event;
    uint32_t keepalive_s;
    uint16_t port;
    char host[AT_MQTT_HOST_MAX_LEN + 1];
    char client_id[AT_MQTT_CLIENT_ID_MAX_LEN + 1];
    char username[AT_MQTT_USERNAME_MAX_LEN + 1];
    char password[AT_MQTT_PASSWORD_MAX_LEN + 1];
    char *read_buf;
    char *write_buf;
    char *raw_payload;
    void *handle;
    beken_mutex_t lock;
    uint8_t lock_inited;
    uint8_t raw_publish_pending;
    uint8_t raw_qos;
    uint8_t raw_retain;
    uint32_t raw_expected_len;
    uint32_t raw_received_len;
    char raw_topic[AT_MQTT_TOPIC_MAX_LEN + 1];
    at_mqtt_sub_topic_t subs[AT_MQTT_SUB_TOPIC_MAX];
} at_mqtt_session_t;

static at_mqtt_session_t s_mqtt_session = {
    .configured = 0,
    .connected = 0,
    .clean_session = 1,
    .suppress_disconnect_event = 0,
    .keepalive_s = AT_MQTT_DEFAULT_KEEPALIVE_S,
    .port = 0,
    .host = {0},
    .client_id = {0},
    .username = {0},
    .password = {0},
    .read_buf = NULL,
    .write_buf = NULL,
    .raw_payload = NULL,
    .handle = NULL,
    .lock = NULL,
    .lock_inited = 0,
    .raw_publish_pending = 0,
    .raw_qos = 0,
    .raw_retain = 0,
    .raw_expected_len = 0,
    .raw_received_len = 0,
    .raw_topic = {0},
    .subs = {{0}},
};

static void at_mqtt_lock(void)
{
    if (!s_mqtt_session.lock_inited) {
        return;
    }

    rtos_lock_mutex(&s_mqtt_session.lock);
}

static void at_mqtt_unlock(void)
{
    if (!s_mqtt_session.lock_inited) {
        return;
    }

    rtos_unlock_mutex(&s_mqtt_session.lock);
}

static int at_mqtt_init_lock(void)
{
    bk_err_t ret;

    if (s_mqtt_session.lock_inited) {
        return 0;
    }

    ret = rtos_init_mutex(&s_mqtt_session.lock);
    if (ret != BK_OK) {
        BK_LOGE(TAG, "init mutex failed:%d\r\n", ret);
        return -1;
    }

    s_mqtt_session.lock_inited = 1;
    return 0;
}

static int at_mqtt_parse_u16(const char *str, uint16_t *value)
{
    char *end = NULL;
    unsigned long number;

    if ((str == NULL) || (value == NULL)) {
        return -1;
    }

    errno = 0;
    number = strtoul(str, &end, 10);
    if ((errno != 0) || (end == str) || (*end != '\0') || (number > 65535UL) || (number == 0)) {
        return -1;
    }

    *value = (uint16_t)number;
    return 0;
}

static int at_mqtt_parse_u32(const char *str, uint32_t *value)
{
    char *end = NULL;
    unsigned long number;

    if ((str == NULL) || (value == NULL)) {
        return -1;
    }

    errno = 0;
    number = strtoul(str, &end, 10);
    if ((errno != 0) || (end == str) || (*end != '\0')) {
        return -1;
    }

    *value = (uint32_t)number;
    return 0;
}

static int at_mqtt_parse_u8_range(const char *str, uint8_t *value, uint8_t max_value)
{
    uint32_t tmp = 0;

    if ((str == NULL) || (value == NULL)) {
        return -1;
    }

    if (at_mqtt_parse_u32(str, &tmp) != 0) {
        return -1;
    }

    if (tmp > max_value) {
        return -1;
    }

    *value = (uint8_t)tmp;
    return 0;
}

static int at_mqtt_copy_arg(char *dst, size_t dst_size, const char *src, bool allow_empty)
{
    size_t len;

    if ((dst == NULL) || (dst_size == 0) || (src == NULL)) {
        return -1;
    }

    len = strlen(src);
    if ((len >= 2) && (src[0] == '"') && (src[len - 1] == '"')) {
        src += 1;
        len -= 2;
    }

    if ((!allow_empty) && (len == 0)) {
        return -1;
    }

    if (len >= dst_size) {
        return -1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int at_mqtt_wifi_ready(void)
{
    wifi_link_status_t link_status;
    netif_ip4_config_t ip4_config;

    if (bk_wifi_sta_get_link_status(&link_status) != BK_OK) {
        return 0;
    }
    (void)link_status;

    if (bk_netif_get_ip4_config(NETIF_IF_STA, &ip4_config) != BK_OK) {
        return 0;
    }

    if ((ip4_config.ip[0] == '\0') || (strcmp(ip4_config.ip, "0.0.0.0") == 0)) {
        return 0;
    }

    return 1;
}

static void at_mqtt_clear_subscriptions(void)
{
    memset(s_mqtt_session.subs, 0, sizeof(s_mqtt_session.subs));
}

static int at_mqtt_find_subscription(const char *topic)
{
    int i;

    if (topic == NULL) {
        return -1;
    }

    for (i = 0; i < AT_MQTT_SUB_TOPIC_MAX; ++i) {
        if (s_mqtt_session.subs[i].used && (strcmp(s_mqtt_session.subs[i].topic, topic) == 0)) {
            return i;
        }
    }

    return -1;
}

static int at_mqtt_store_subscription(const char *topic, uint8_t qos)
{
    int index;
    int i;

    index = at_mqtt_find_subscription(topic);
    if (index >= 0) {
        s_mqtt_session.subs[index].qos = qos;
        return 0;
    }

    for (i = 0; i < AT_MQTT_SUB_TOPIC_MAX; ++i) {
        if (!s_mqtt_session.subs[i].used) {
            s_mqtt_session.subs[i].used = 1;
            s_mqtt_session.subs[i].qos = qos;
            os_strlcpy(s_mqtt_session.subs[i].topic, topic, sizeof(s_mqtt_session.subs[i].topic));
            return 0;
        }
    }

    return -1;
}

static int at_mqtt_ensure_buffers(void)
{
    if (s_mqtt_session.read_buf == NULL) {
        s_mqtt_session.read_buf = at_malloc(AT_MQTT_IOBUF_LEN);
        if (s_mqtt_session.read_buf == NULL) {
            return -1;
        }
    }

    if (s_mqtt_session.write_buf == NULL) {
        s_mqtt_session.write_buf = at_malloc(AT_MQTT_IOBUF_LEN);
        if (s_mqtt_session.write_buf == NULL) {
            at_free(s_mqtt_session.read_buf);
            s_mqtt_session.read_buf = NULL;
            return -1;
        }
    }

    return 0;
}

static void at_mqtt_release_buffers(void)
{
    if (s_mqtt_session.read_buf != NULL) {
        at_free(s_mqtt_session.read_buf);
        s_mqtt_session.read_buf = NULL;
    }

    if (s_mqtt_session.write_buf != NULL) {
        at_free(s_mqtt_session.write_buf);
        s_mqtt_session.write_buf = NULL;
    }
}

static void at_mqtt_reset_raw_publish_locked(void)
{
    if (s_mqtt_session.raw_payload != NULL) {
        at_free(s_mqtt_session.raw_payload);
        s_mqtt_session.raw_payload = NULL;
    }

    s_mqtt_session.raw_publish_pending = 0;
    s_mqtt_session.raw_qos = 0;
    s_mqtt_session.raw_retain = 0;
    s_mqtt_session.raw_expected_len = 0;
    s_mqtt_session.raw_received_len = 0;
    memset(s_mqtt_session.raw_topic, 0, sizeof(s_mqtt_session.raw_topic));
}

static int at_mqtt_prepare_device_info(const char *client_id, const char *password)
{
    char device_name[DEVICE_NAME_LEN + 1];
    char device_secret[DEVICE_SECRET_LEN + 1];

    if ((client_id == NULL) || (password == NULL)) {
        return -1;
    }

    memset(device_name, 0, sizeof(device_name));
    memset(device_secret, 0, sizeof(device_secret));
    os_strlcpy(device_name, client_id, sizeof(device_name));

    if (password[0] != '\0') {
        os_strlcpy(device_secret, password, sizeof(device_secret));
    } else {
        os_strlcpy(device_secret, client_id, sizeof(device_secret));
    }

    if (iotx_device_info_init() != SUCCESS_RETURN) {
        BK_LOGE(TAG, "device info init failed\r\n");
        return -1;
    }

    if (iotx_device_info_set(AT_MQTT_PRODUCT_KEY, device_name, device_secret) != SUCCESS_RETURN) {
        BK_LOGE(TAG, "device info set failed\r\n");
        return -1;
    }

    return 0;
}

static void at_mqtt_output_formatted(const char *format, ...)
{
    char buffer[AT_MQTT_EVENT_BUF_LEN];
    va_list args;
    int len;

    va_start(args, format);
    len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if ((len < 0) || (len >= (int)sizeof(buffer))) {
        BK_LOGE(TAG, "event output overflow\r\n");
        return;
    }

    atsvr_output_msg(buffer);
}

static void at_mqtt_output_connected_event(const char *host, uint16_t port)
{
    at_mqtt_output_formatted("\r\n+MQTTCONNECTED:0,\"%s\",%u\r\n", host, port);
}

static void at_mqtt_output_disconnected_event(void)
{
    at_mqtt_output_formatted("\r\n+MQTTDISCONNECTED:0\r\n");
}

static void at_mqtt_output_pubraw_result(int success)
{
    if (success) {
        at_mqtt_output_formatted("\r\n+MQTTPUB:OK\r\n");
    } else {
        at_mqtt_output_formatted("\r\n+MQTTPUB:FAIL\r\n");
    }
}

static int at_mqtt_publish_payload(void *handle, const char *topic, const void *payload, uint32_t payload_len, uint8_t qos, uint8_t retain)
{
    iotx_mqtt_topic_info_t topic_msg;
    int rc;

    if ((handle == NULL) || (topic == NULL) || (payload == NULL)) {
        return -1;
    }

    memset(&topic_msg, 0, sizeof(topic_msg));
    topic_msg.qos = qos;
    topic_msg.retain = retain;
    topic_msg.dup = 0;
    topic_msg.payload = (void *)payload;
    topic_msg.payload_len = payload_len;

    rc = IOT_MQTT_Publish(handle, topic, &topic_msg);
    if (rc < 0) {
        BK_LOGE(TAG, "publish failed:%d\r\n", rc);
        return -1;
    }

    return 0;
}

static void at_mqtt_handle_incoming_payload(iotx_mqtt_topic_info_pt topic_info)
{
    char topic[AT_MQTT_TOPIC_MAX_LEN + 1];
    size_t payload_len;
    size_t event_len;
    size_t header_len;
    char *event_buf;

    if ((topic_info == NULL) || (topic_info->ptopic == NULL) || (topic_info->payload == NULL)) {
        return;
    }

    if ((topic_info->topic_len == 0) || (topic_info->topic_len > AT_MQTT_TOPIC_MAX_LEN)) {
        BK_LOGE(TAG, "topic too long:%u\r\n", topic_info ? topic_info->topic_len : 0);
        return;
    }

    memcpy(topic, topic_info->ptopic, topic_info->topic_len);
    topic[topic_info->topic_len] = '\0';

    payload_len = topic_info->payload_len;
    event_len = strlen("\r\n+MQTTSUBRECV:0,\"\",,\r\n") + strlen(topic) + 20 + payload_len + 1;
    event_buf = at_malloc(event_len);
    if (event_buf == NULL) {
        BK_LOGE(TAG, "alloc subrecv buffer failed\r\n");
        return;
    }

    header_len = snprintf(event_buf, event_len, "\r\n+MQTTSUBRECV:0,\"%s\",%u,", topic, (unsigned int)payload_len);
    if ((header_len >= event_len) || ((event_len - header_len) < (payload_len + 3))) {
        BK_LOGE(TAG, "subrecv header overflow\r\n");
        at_free(event_buf);
        return;
    }

    memcpy(event_buf + header_len, topic_info->payload, payload_len);
    memcpy(event_buf + header_len + payload_len, "\r\n", 3);

    atsvr_output_msg(event_buf);
    at_free(event_buf);
}

static void at_mqtt_subscribe_message_handler(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    iotx_mqtt_topic_info_pt topic_info;

    (void)pcontext;
    (void)pclient;

    if ((msg == NULL) || (msg->event_type != IOTX_MQTT_EVENT_PUBLISH_RECVEIVED) || (msg->msg == NULL)) {
        return;
    }

    topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
    at_mqtt_handle_incoming_payload(topic_info);
}

static void at_mqtt_event_handler(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    iotx_mqtt_topic_info_pt topic_info = msg ? (iotx_mqtt_topic_info_pt)msg->msg : NULL;
    char host[AT_MQTT_HOST_MAX_LEN + 1];
    uint16_t port = 0;
    int suppress_event = 0;

    (void)pcontext;
    (void)pclient;

    if (msg == NULL) {
        return;
    }

    switch (msg->event_type) {
    case IOTX_MQTT_EVENT_DISCONNECT:
        at_mqtt_lock();
        suppress_event = s_mqtt_session.suppress_disconnect_event;
        s_mqtt_session.connected = 0;
        at_mqtt_reset_raw_publish_locked();
        at_mqtt_unlock();
        if (!suppress_event) {
            at_mqtt_output_disconnected_event();
        }
        break;
    case IOTX_MQTT_EVENT_RECONNECT:
        at_mqtt_lock();
        s_mqtt_session.connected = 1;
        os_strlcpy(host, s_mqtt_session.host, sizeof(host));
        port = s_mqtt_session.port;
        at_mqtt_unlock();
        at_mqtt_output_connected_event(host, port);
        break;
    case IOTX_MQTT_EVENT_PUBLISH_RECVEIVED:
        at_mqtt_handle_incoming_payload(topic_info);
        break;
    default:
        break;
    }
}

static int at_mqtt_usercfg_cmd(int sync, int argc, char **argv)
{
    uint32_t keepalive = AT_MQTT_DEFAULT_KEEPALIVE_S;
    uint8_t clean_session = 1;
    char client_id[AT_MQTT_CLIENT_ID_MAX_LEN + 1];
    char username[AT_MQTT_USERNAME_MAX_LEN + 1];
    char password[AT_MQTT_PASSWORD_MAX_LEN + 1];

    (void)sync;

    if ((argc < 4) || (argc > 6)) {
        goto error;
    }

    if (atoi(argv[0]) != AT_MQTT_LINK_ID) {
        goto error;
    }

    if (at_mqtt_copy_arg(client_id, sizeof(client_id), argv[1], false) != 0) {
        goto error;
    }

    if (at_mqtt_copy_arg(username, sizeof(username), argv[2], true) != 0) {
        goto error;
    }

    if (at_mqtt_copy_arg(password, sizeof(password), argv[3], true) != 0) {
        goto error;
    }

    if ((argc >= 5) && (at_mqtt_parse_u32(argv[4], &keepalive) != 0 || keepalive == 0)) {
        goto error;
    }

    if ((argc >= 6) && (at_mqtt_parse_u8_range(argv[5], &clean_session, 1) != 0)) {
        goto error;
    }

    at_mqtt_lock();
    if (s_mqtt_session.connected) {
        at_mqtt_unlock();
        goto error;
    }
    os_strlcpy(s_mqtt_session.client_id, client_id, sizeof(s_mqtt_session.client_id));
    os_strlcpy(s_mqtt_session.username, username, sizeof(s_mqtt_session.username));
    os_strlcpy(s_mqtt_session.password, password, sizeof(s_mqtt_session.password));
    s_mqtt_session.keepalive_s = keepalive;
    s_mqtt_session.clean_session = clean_session;
    s_mqtt_session.configured = 1;
    at_mqtt_unlock();

    atsvr_cmd_rsp_ok();
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_mqtt_conn_cmd(int sync, int argc, char **argv)
{
    iotx_mqtt_param_t mqtt_params;
    void *handle = NULL;
    uint16_t port = 0;
    char host[AT_MQTT_HOST_MAX_LEN + 1];
    char client_id[AT_MQTT_CLIENT_ID_MAX_LEN + 1];
    char username[AT_MQTT_USERNAME_MAX_LEN + 1];
    char password[AT_MQTT_PASSWORD_MAX_LEN + 1];
    uint32_t keepalive = 0;
    uint8_t clean_session = 1;

    (void)sync;

    if (argc != 3) {
        goto error;
    }

    if (atoi(argv[0]) != AT_MQTT_LINK_ID) {
        goto error;
    }

    if (!at_mqtt_wifi_ready()) {
        BK_LOGE(TAG, "wifi not ready\r\n");
        goto error;
    }

    if (at_mqtt_copy_arg(host, sizeof(host), argv[1], false) != 0) {
        goto error;
    }

    if (at_mqtt_parse_u16(argv[2], &port) != 0) {
        goto error;
    }

    at_mqtt_lock();
    if ((!s_mqtt_session.configured) || s_mqtt_session.connected) {
        at_mqtt_unlock();
        goto error;
    }
    os_strlcpy(client_id, s_mqtt_session.client_id, sizeof(client_id));
    os_strlcpy(username, s_mqtt_session.username, sizeof(username));
    os_strlcpy(password, s_mqtt_session.password, sizeof(password));
    keepalive = s_mqtt_session.keepalive_s;
    clean_session = s_mqtt_session.clean_session;
    at_mqtt_unlock();

    if (at_mqtt_prepare_device_info(client_id, password) != 0) {
        goto error;
    }

    if (at_mqtt_ensure_buffers() != 0) {
        BK_LOGE(TAG, "alloc mqtt io buffers failed\r\n");
        goto error;
    }

    memset(&mqtt_params, 0, sizeof(mqtt_params));
    mqtt_params.port = port;
    mqtt_params.host = host;
    mqtt_params.client_id = client_id;
    mqtt_params.username = username;
    mqtt_params.password = password;
    mqtt_params.pub_key = NULL;
    mqtt_params.request_timeout_ms = 3000;
    mqtt_params.clean_session = clean_session;
    mqtt_params.keepalive_interval_ms = keepalive * 1000;
    mqtt_params.pread_buf = s_mqtt_session.read_buf;
    mqtt_params.read_buf_size = AT_MQTT_IOBUF_LEN;
    mqtt_params.pwrite_buf = s_mqtt_session.write_buf;
    mqtt_params.write_buf_size = AT_MQTT_IOBUF_LEN;
    mqtt_params.handle_event.h_fp = at_mqtt_event_handler;
    mqtt_params.handle_event.pcontext = NULL;

    handle = IOT_MQTT_Construct(&mqtt_params);
    if (handle == NULL) {
        BK_LOGE(TAG, "mqtt construct failed\r\n");
        at_mqtt_release_buffers();
        goto error;
    }

    at_mqtt_lock();
    s_mqtt_session.handle = handle;
    s_mqtt_session.connected = 1;
    s_mqtt_session.port = port;
    os_strlcpy(s_mqtt_session.host, host, sizeof(s_mqtt_session.host));
    at_mqtt_unlock();

    at_mqtt_output_connected_event(host, port);
    atsvr_cmd_rsp_ok();
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_mqtt_sub_cmd(int sync, int argc, char **argv)
{
    void *handle;
    uint8_t qos = 0;
    char topic[AT_MQTT_TOPIC_MAX_LEN + 1];
    int sub_index;
    int rc;

    (void)sync;

    if (argc != 3) {
        goto error;
    }

    if (atoi(argv[0]) != AT_MQTT_LINK_ID) {
        goto error;
    }

    if (at_mqtt_copy_arg(topic, sizeof(topic), argv[1], false) != 0) {
        goto error;
    }

    if (at_mqtt_parse_u8_range(argv[2], &qos, 1) != 0) {
        goto error;
    }

    at_mqtt_lock();
    if (!s_mqtt_session.connected || (s_mqtt_session.handle == NULL)) {
        at_mqtt_unlock();
        goto error;
    }

    if (at_mqtt_find_subscription(topic) >= 0) {
        at_mqtt_unlock();
        atsvr_cmd_rsp_ok();
        return 0;
    }

    if (at_mqtt_store_subscription(topic, qos) != 0) {
        at_mqtt_unlock();
        BK_LOGE(TAG, "subscription table full\r\n");
        goto error;
    }
    sub_index = at_mqtt_find_subscription(topic);
    handle = s_mqtt_session.handle;
    at_mqtt_unlock();

    rc = IOT_MQTT_Subscribe(handle, topic, (iotx_mqtt_qos_t)qos, at_mqtt_subscribe_message_handler, NULL);
    if (rc < 0) {
        at_mqtt_lock();
        if (sub_index >= 0) {
            memset(&s_mqtt_session.subs[sub_index], 0, sizeof(at_mqtt_sub_topic_t));
        }
        at_mqtt_unlock();
        BK_LOGE(TAG, "subscribe failed:%d\r\n", rc);
        goto error;
    }

    atsvr_cmd_rsp_ok();
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_mqtt_pub_cmd(int sync, int argc, char **argv)
{
    void *handle;
    uint8_t qos = 0;
    uint8_t retain = 0;
    char topic[AT_MQTT_TOPIC_MAX_LEN + 1];
    const char *payload = NULL;
    char payload_buf[AT_MQTT_EVENT_BUF_LEN];

    (void)sync;

    if (argc != 5) {
        goto error;
    }

    if (atoi(argv[0]) != AT_MQTT_LINK_ID) {
        goto error;
    }

    if (at_mqtt_copy_arg(topic, sizeof(topic), argv[1], false) != 0) {
        goto error;
    }

    if (at_mqtt_copy_arg(payload_buf, sizeof(payload_buf), argv[2], true) != 0) {
        goto error;
    }
    payload = payload_buf;

    if (at_mqtt_parse_u8_range(argv[3], &qos, 1) != 0) {
        goto error;
    }

    if (at_mqtt_parse_u8_range(argv[4], &retain, 1) != 0) {
        goto error;
    }

    at_mqtt_lock();
    if (!s_mqtt_session.connected || (s_mqtt_session.handle == NULL)) {
        at_mqtt_unlock();
        goto error;
    }
    handle = s_mqtt_session.handle;
    at_mqtt_unlock();

    if (at_mqtt_publish_payload(handle, topic, payload, strlen(payload), qos, retain) != 0) {
        goto error;
    }

    atsvr_cmd_rsp_ok();
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_mqtt_clean_cmd(int sync, int argc, char **argv)
{
    void *handle = NULL;

    (void)sync;

    if (argc != 1) {
        goto error;
    }

    if (atoi(argv[0]) != AT_MQTT_LINK_ID) {
        goto error;
    }

    at_mqtt_lock();
    if (!s_mqtt_session.connected || (s_mqtt_session.handle == NULL)) {
        at_mqtt_unlock();
        goto error;
    }
    at_mqtt_reset_raw_publish_locked();
    s_mqtt_session.suppress_disconnect_event = 1;
    handle = s_mqtt_session.handle;
    s_mqtt_session.handle = NULL;
    s_mqtt_session.connected = 0;
    s_mqtt_session.port = 0;
    memset(s_mqtt_session.host, 0, sizeof(s_mqtt_session.host));
    at_mqtt_clear_subscriptions();
    at_mqtt_unlock();

    IOT_MQTT_Destroy(&handle);
    at_mqtt_release_buffers();

    at_mqtt_lock();
    s_mqtt_session.suppress_disconnect_event = 0;
    at_mqtt_unlock();

    atsvr_cmd_rsp_ok();
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_mqtt_pubraw_cmd(int sync, int argc, char **argv)
{
    uint32_t data_len = 0;
    uint8_t qos = 0;
    uint8_t retain = 0;
    char topic[AT_MQTT_TOPIC_MAX_LEN + 1];
    char *payload = NULL;

    (void)sync;

    if (argc != 5) {
        goto error;
    }

    if (atoi(argv[0]) != AT_MQTT_LINK_ID) {
        goto error;
    }

    if (at_mqtt_copy_arg(topic, sizeof(topic), argv[1], false) != 0) {
        goto error;
    }

    if ((at_mqtt_parse_u32(argv[2], &data_len) != 0) || (data_len == 0) || (data_len > AT_MQTT_PUBRAW_MAX_LEN)) {
        BK_LOGE(TAG, "AT+MQTTPUBRAW invalid length:%u\r\n", (unsigned int)data_len);
        goto error;
    }

    if (at_mqtt_parse_u8_range(argv[3], &qos, 1) != 0) {
        goto error;
    }

    if (at_mqtt_parse_u8_range(argv[4], &retain, 1) != 0) {
        goto error;
    }

    payload = at_malloc(data_len + 1);
    if (payload == NULL) {
        BK_LOGE(TAG, "alloc raw publish buffer failed\r\n");
        goto error;
    }

    at_mqtt_lock();
    if (!s_mqtt_session.connected || (s_mqtt_session.handle == NULL) || s_mqtt_session.raw_publish_pending) {
        at_mqtt_unlock();
        at_free(payload);
        goto error;
    }

    at_mqtt_reset_raw_publish_locked();
    s_mqtt_session.raw_payload = payload;
    s_mqtt_session.raw_publish_pending = 1;
    s_mqtt_session.raw_qos = qos;
    s_mqtt_session.raw_retain = retain;
    s_mqtt_session.raw_expected_len = data_len;
    s_mqtt_session.raw_received_len = 0;
    os_strlcpy(s_mqtt_session.raw_topic, topic, sizeof(s_mqtt_session.raw_topic));
    at_mqtt_unlock();

    atsvr_cmd_rsp_ok();
    atsvr_output_msg(">\r\n");
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

int at_mqtt_raw_input(const char *buf, int len)
{
    void *handle = NULL;
    char *payload = NULL;
    uint32_t total_len = 0;
    uint8_t qos = 0;
    uint8_t retain = 0;
    char topic[AT_MQTT_TOPIC_MAX_LEN + 1];
    int success = 0;

    if ((buf == NULL) || (len <= 0)) {
        return 0;
    }

    at_mqtt_lock();
    if (!s_mqtt_session.raw_publish_pending) {
        at_mqtt_unlock();
        return 0;
    }

    if (!s_mqtt_session.connected || (s_mqtt_session.handle == NULL) || (s_mqtt_session.raw_payload == NULL)) {
        at_mqtt_reset_raw_publish_locked();
        at_mqtt_unlock();
        at_mqtt_output_pubraw_result(0);
        return 1;
    }

    if ((uint32_t)len > (s_mqtt_session.raw_expected_len - s_mqtt_session.raw_received_len)) {
        at_mqtt_reset_raw_publish_locked();
        at_mqtt_unlock();
        BK_LOGE(TAG, "MQTTPUBRAW input longer than expected\r\n");
        at_mqtt_output_pubraw_result(0);
        return 1;
    }

    memcpy(s_mqtt_session.raw_payload + s_mqtt_session.raw_received_len, buf, len);
    s_mqtt_session.raw_received_len += len;

    if (s_mqtt_session.raw_received_len < s_mqtt_session.raw_expected_len) {
        at_mqtt_unlock();
        return 1;
    }

    payload = s_mqtt_session.raw_payload;
    total_len = s_mqtt_session.raw_expected_len;
    payload[total_len] = '\0';
    handle = s_mqtt_session.handle;
    qos = s_mqtt_session.raw_qos;
    retain = s_mqtt_session.raw_retain;
    os_strlcpy(topic, s_mqtt_session.raw_topic, sizeof(topic));
    s_mqtt_session.raw_payload = NULL;
    at_mqtt_reset_raw_publish_locked();
    at_mqtt_unlock();

    success = (at_mqtt_publish_payload(handle, topic, payload, total_len, qos, retain) == 0);
    at_free(payload);
    at_mqtt_output_pubraw_result(success);
    return 1;
}

static const struct _atsvr_command s_mqtt_cmds_table[] = {
    ATSVR_CMD_HADLER("AT+MQTTUSERCFG", "AT+MQTTUSERCFG=0,\"client_id\",\"username\",\"password\"[,<keepalive_s>[,<clean_session>]]",
                     NULL, at_mqtt_usercfg_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+MQTTCONN", "AT+MQTTCONN=0,\"host\",port",
                     NULL, at_mqtt_conn_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+MQTTSUB", "AT+MQTTSUB=0,\"topic\",qos",
                     NULL, at_mqtt_sub_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+MQTTPUB", "AT+MQTTPUB=0,\"topic\",\"payload\",qos,retain",
                     NULL, at_mqtt_pub_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+MQTTPUBRAW", "AT+MQTTPUBRAW=0,\"topic\",len,qos,retain",
                     NULL, at_mqtt_pubraw_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+MQTTCLEAN", "AT+MQTTCLEAN=0",
                     NULL, at_mqtt_clean_cmd, false, 0, 0, NULL, false),
};

void mqtt_at_cmd_init(void)
{
    int ret;

    if (at_mqtt_init_lock() != 0) {
        return;
    }

    ret = atsvr_register_commands(s_mqtt_cmds_table,
                                  sizeof(s_mqtt_cmds_table) / sizeof(s_mqtt_cmds_table[0]),
                                  "mqtt",
                                  NULL);
    if (ret == 0) {
        BK_LOGI(TAG, "MQTT AT CMDS INIT OK\r\n");
    } else {
        BK_LOGE(TAG, "MQTT AT CMDS INIT FAIL:%d\r\n", ret);
    }
}

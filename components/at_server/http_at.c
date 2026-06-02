#include <components/log.h>
#include <components/netif.h>
#include <modules/wifi.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <os/os.h>
#include <os/str.h>

#include "atsvr_unite.h"
#include "http_at.h"
#include "httpc.h"

#define TAG "AT_HTTP"

#define AT_HTTP_URL_MAX_LEN             511
#define AT_HTTP_HOST_MAX_LEN            127
#define AT_HTTP_RESOURCE_MAX_LEN        511
#define AT_HTTP_METHOD_MAX_LEN          7
#define AT_HTTP_CONTENT_TYPE_MAX_LEN    127
#define AT_HTTP_OUTPUT_BUF_LEN          256
#define AT_HTTP_BODY_MAX_LEN            4096
#define AT_HTTP_READ_CHUNK_LEN          512
#define AT_HTTP_DEFAULT_TIMEOUT_MS      5000

typedef struct {
    uint8_t secure;
    uint16_t port;
    char host[AT_HTTP_HOST_MAX_LEN + 1];
    char resource[AT_HTTP_RESOURCE_MAX_LEN + 1];
} at_http_url_t;

typedef struct {
    uint8_t raw_pending;
    uint8_t request_in_progress;
    uint32_t expected_len;
    uint32_t received_len;
    char method[AT_HTTP_METHOD_MAX_LEN + 1];
    char url[AT_HTTP_URL_MAX_LEN + 1];
    char content_type[AT_HTTP_CONTENT_TYPE_MAX_LEN + 1];
    char *body;
    beken_mutex_t lock;
    uint8_t lock_inited;
} at_http_session_t;

static at_http_session_t s_http_session = {
    .raw_pending = 0,
    .request_in_progress = 0,
    .expected_len = 0,
    .received_len = 0,
    .method = {0},
    .url = {0},
    .content_type = {0},
    .body = NULL,
    .lock = NULL,
    .lock_inited = 0,
};

static void at_http_lock(void)
{
    if (!s_http_session.lock_inited) {
        return;
    }

    rtos_lock_mutex(&s_http_session.lock);
}

static void at_http_unlock(void)
{
    if (!s_http_session.lock_inited) {
        return;
    }

    rtos_unlock_mutex(&s_http_session.lock);
}

static int at_http_init_lock(void)
{
    bk_err_t ret;

    if (s_http_session.lock_inited) {
        return 0;
    }

    ret = rtos_init_mutex(&s_http_session.lock);
    if (ret != BK_OK) {
        BK_LOGE(TAG, "init mutex failed:%d\r\n", ret);
        return -1;
    }

    s_http_session.lock_inited = 1;
    return 0;
}

static int at_http_wifi_ready(void)
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

static int at_http_parse_u16(const char *str, uint16_t *value)
{
    char *end = NULL;
    unsigned long number;

    if ((str == NULL) || (value == NULL)) {
        return -1;
    }

    errno = 0;
    number = strtoul(str, &end, 10);
    if ((errno != 0) || (end == str) || (*end != '\0') || (number == 0) || (number > 65535UL)) {
        return -1;
    }

    *value = (uint16_t)number;
    return 0;
}

static int at_http_parse_u32(const char *str, uint32_t *value)
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

static int at_http_copy_arg(char *dst, size_t dst_size, const char *src, bool allow_empty)
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

static void at_http_reset_pending_locked(void)
{
    if (s_http_session.body != NULL) {
        at_free(s_http_session.body);
        s_http_session.body = NULL;
    }

    s_http_session.raw_pending = 0;
    s_http_session.expected_len = 0;
    s_http_session.received_len = 0;
    memset(s_http_session.method, 0, sizeof(s_http_session.method));
    memset(s_http_session.url, 0, sizeof(s_http_session.url));
    memset(s_http_session.content_type, 0, sizeof(s_http_session.content_type));
}

static void at_http_set_request_in_progress(int in_progress)
{
    at_http_lock();
    s_http_session.request_in_progress = (uint8_t)(in_progress ? 1 : 0);
    at_http_unlock();
}

static int at_http_has_pending_or_busy_locked(void)
{
    return (s_http_session.raw_pending || s_http_session.request_in_progress);
}

static void at_http_output_formatted(const char *format, ...)
{
    char buffer[AT_HTTP_OUTPUT_BUF_LEN];
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

static void at_http_output_block(const char *prefix, const void *data, size_t data_len)
{
    size_t prefix_len;
    char *buffer;

    if ((prefix == NULL) || (data == NULL)) {
        return;
    }

    prefix_len = strlen(prefix);
    buffer = at_malloc(prefix_len + data_len + 3);
    if (buffer == NULL) {
        BK_LOGE(TAG, "alloc output block failed\r\n");
        return;
    }

    memcpy(buffer, prefix, prefix_len);
    memcpy(buffer + prefix_len, data, data_len);
    memcpy(buffer + prefix_len + data_len, "\r\n", 3);
    atsvr_output_msg(buffer);
    at_free(buffer);
}

static void at_http_output_status(const char *status)
{
    int code = 0;
    const char *reason = "";
    char reason_buf[AT_HTTP_OUTPUT_BUF_LEN];
    size_t reason_len;

    if (status == NULL) {
        at_http_output_formatted("\r\n+HTTPSTATUS:0,\"\"\r\n");
        return;
    }

    code = atoi(status);
    reason = strchr(status, ' ');
    if (reason != NULL) {
        while (*reason == ' ') {
            reason++;
        }
    } else {
        reason = "";
    }

    reason_len = strlen(reason);
    while ((reason_len > 0) && ((reason[reason_len - 1] == '\r') || (reason[reason_len - 1] == '\n'))) {
        reason_len--;
    }
    if (reason_len >= sizeof(reason_buf)) {
        reason_len = sizeof(reason_buf) - 1;
    }

    memcpy(reason_buf, reason, reason_len);
    reason_buf[reason_len] = '\0';
    at_http_output_formatted("\r\n+HTTPSTATUS:%d,\"%s\"\r\n", code, reason_buf);
}

static int at_http_parse_url(const char *url, at_http_url_t *parsed)
{
    const char *host_begin;
    const char *path_begin;
    const char *port_begin = NULL;
    const char *p;
    size_t host_len;
    size_t resource_len;
    char port_buf[8];

    if ((url == NULL) || (parsed == NULL)) {
        return -1;
    }

    memset(parsed, 0, sizeof(*parsed));

    if (strncmp(url, "http://", 7) == 0) {
        parsed->secure = HTTPC_SECURE_NONE;
        parsed->port = 80;
        host_begin = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        parsed->secure = HTTPC_SECURE_TLS;
        parsed->port = 443;
        host_begin = url + 8;
    } else {
        return -1;
    }

    path_begin = strchr(host_begin, '/');
    if (path_begin == NULL) {
        path_begin = host_begin + strlen(host_begin);
    }

    for (p = host_begin; p < path_begin; ++p) {
        if (*p == ':') {
            port_begin = p;
            break;
        }
    }

    if (port_begin != NULL) {
        host_len = (size_t)(port_begin - host_begin);
    } else {
        host_len = (size_t)(path_begin - host_begin);
    }

    if ((host_len == 0) || (host_len > AT_HTTP_HOST_MAX_LEN)) {
        return -1;
    }

    memcpy(parsed->host, host_begin, host_len);
    parsed->host[host_len] = '\0';

    if (port_begin != NULL) {
        size_t port_len = (size_t)(path_begin - port_begin - 1);
        if ((port_len == 0) || (port_len >= sizeof(port_buf))) {
            return -1;
        }
        memcpy(port_buf, port_begin + 1, port_len);
        port_buf[port_len] = '\0';
        if (at_http_parse_u16(port_buf, &parsed->port) != 0) {
            return -1;
        }
    }

    if (*path_begin == '\0') {
        os_strlcpy(parsed->resource, "/", sizeof(parsed->resource));
        return 0;
    }

    resource_len = strlen(path_begin);
    if (resource_len > AT_HTTP_RESOURCE_MAX_LEN) {
        return -1;
    }

    memcpy(parsed->resource, path_begin, resource_len);
    parsed->resource[resource_len] = '\0';
    return 0;
}

static void at_http_cleanup_conn(struct httpc_conn *conn)
{
    if (conn == NULL) {
        return;
    }

    httpc_conn_close(conn);
    httpc_conn_free(conn);
    httpc_free(conn);
}

static int at_http_execute_request(const char *method, const char *url, const char *content_type,
                                   const uint8_t *body, size_t body_len)
{
    at_http_url_t parsed;
    struct httpc_conn *conn = NULL;
    uint8_t read_buf[AT_HTTP_READ_CHUNK_LEN];
    size_t total_read = 0;
    int read_size;
    int written;

    if ((method == NULL) || (url == NULL)) {
        return -1;
    }

    if (at_http_parse_url(url, &parsed) != 0) {
        BK_LOGE(TAG, "invalid url:%s\r\n", url);
        return -1;
    }

    conn = httpc_conn_new(parsed.secure, NULL, NULL, NULL);
    if (conn == NULL) {
        BK_LOGE(TAG, "httpc conn alloc failed\r\n");
        return -1;
    }

    if (httpc_conn_connect(conn, parsed.host, parsed.port, AT_HTTP_DEFAULT_TIMEOUT_MS) != BK_OK) {
        BK_LOGE(TAG, "httpc connect failed\r\n");
        at_http_cleanup_conn(conn);
        return -1;
    }

    if (httpc_request_write_header_start(conn, (char *)method, parsed.resource,
                                         (char *)content_type, body_len) != 0) {
        BK_LOGE(TAG, "write header start failed\r\n");
        at_http_cleanup_conn(conn);
        return -1;
    }

    (void)httpc_request_write_header(conn, "User-Agent", "BK7239 HTTP AT/1.0");
    (void)httpc_request_write_header(conn, "Accept", "*/*");

    if (httpc_request_write_header_finish(conn) < 0) {
        BK_LOGE(TAG, "write header finish failed\r\n");
        at_http_cleanup_conn(conn);
        return -1;
    }

    if ((body != NULL) && (body_len > 0)) {
        written = httpc_request_write_data(conn, (uint8_t *)body, body_len);
        if (written != (int)body_len) {
            BK_LOGE(TAG, "write body failed:%d/%u\r\n", written, (unsigned int)body_len);
            at_http_cleanup_conn(conn);
            return -1;
        }
    }

    if (httpc_response_read_header(conn) != 0) {
        BK_LOGE(TAG, "read header failed\r\n");
        at_http_cleanup_conn(conn);
        return -1;
    }

    at_http_output_status((char *)conn->response.status);
    at_http_output_formatted("\r\n+HTTPHEADERS:%u\r\n", (unsigned int)conn->response.header_len);
    if ((conn->response.header != NULL) && (conn->response.header_len > 0)) {
        at_http_output_block("", conn->response.header, conn->response.header_len);
    }

    if (strcmp(method, "HEAD") != 0) {
        while (1) {
            read_size = httpc_response_read_data(conn, read_buf, sizeof(read_buf));
            if (read_size < 0) {
                BK_LOGE(TAG, "read body failed\r\n");
                at_http_cleanup_conn(conn);
                return -1;
            }

            if (read_size == 0) {
                break;
            }

            total_read += (size_t)read_size;
            at_http_output_formatted("\r\n+HTTPDATA:%d\r\n", read_size);
            at_http_output_block("", read_buf, (size_t)read_size);

            if ((conn->response.content_len > 0) && (total_read >= conn->response.content_len)) {
                break;
            }
        }
    }

    at_http_output_formatted("\r\n+HTTPDONE\r\n");
    at_http_cleanup_conn(conn);
    return 0;
}

static int at_http_prepare_raw_request(const char *method, const char *url, uint32_t data_len, const char *content_type)
{
    char *body = NULL;

    body = at_malloc(data_len + 1);
    if (body == NULL) {
        BK_LOGE(TAG, "alloc http raw buffer failed\r\n");
        return -1;
    }

    at_http_lock();
    if (at_http_has_pending_or_busy_locked()) {
        at_http_unlock();
        at_free(body);
        return -1;
    }

    at_http_reset_pending_locked();
    s_http_session.body = body;
    s_http_session.raw_pending = 1;
    s_http_session.expected_len = data_len;
    s_http_session.received_len = 0;
    os_strlcpy(s_http_session.method, method, sizeof(s_http_session.method));
    os_strlcpy(s_http_session.url, url, sizeof(s_http_session.url));
    os_strlcpy(s_http_session.content_type, content_type, sizeof(s_http_session.content_type));
    at_http_unlock();
    return 0;
}

static int at_http_simple_cmd_common(int argc, char **argv, const char *method)
{
    char url[AT_HTTP_URL_MAX_LEN + 1];

    if ((argc != 1) || (method == NULL)) {
        goto error;
    }

    if (!at_http_wifi_ready()) {
        BK_LOGE(TAG, "wifi not ready\r\n");
        goto error;
    }

    if (at_http_copy_arg(url, sizeof(url), argv[0], false) != 0) {
        goto error;
    }

    at_http_lock();
    if (at_http_has_pending_or_busy_locked()) {
        at_http_unlock();
        goto error;
    }
    s_http_session.request_in_progress = 1;
    at_http_unlock();

    if (at_http_execute_request(method, url, NULL, NULL, 0) != 0) {
        at_http_set_request_in_progress(0);
        goto error;
    }

    at_http_set_request_in_progress(0);
    atsvr_cmd_rsp_ok();
    return 0;

error:
    at_http_set_request_in_progress(0);
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_http_head_cmd(int sync, int argc, char **argv)
{
    (void)sync;
    return at_http_simple_cmd_common(argc, argv, "HEAD");
}

static int at_http_get_cmd(int sync, int argc, char **argv)
{
    (void)sync;
    return at_http_simple_cmd_common(argc, argv, "GET");
}

static int at_http_delete_cmd(int sync, int argc, char **argv)
{
    (void)sync;
    return at_http_simple_cmd_common(argc, argv, "DELETE");
}

static int at_http_body_cmd_common(int argc, char **argv, const char *method)
{
    uint32_t data_len = 0;
    at_http_url_t parsed;
    char url[AT_HTTP_URL_MAX_LEN + 1];
    char content_type[AT_HTTP_CONTENT_TYPE_MAX_LEN + 1];

    if ((argc < 2) || (argc > 3) || (method == NULL)) {
        goto error;
    }

    if (!at_http_wifi_ready()) {
        BK_LOGE(TAG, "wifi not ready\r\n");
        goto error;
    }

    if (at_http_copy_arg(url, sizeof(url), argv[0], false) != 0) {
        goto error;
    }

    if (at_http_parse_url(url, &parsed) != 0) {
        goto error;
    }

    if ((at_http_parse_u32(argv[1], &data_len) != 0) || (data_len == 0) || (data_len > AT_HTTP_BODY_MAX_LEN)) {
        BK_LOGE(TAG, "invalid body length:%u\r\n", (unsigned int)data_len);
        goto error;
    }

    os_strlcpy(content_type, "application/octet-stream", sizeof(content_type));
    if ((argc == 3) && (at_http_copy_arg(content_type, sizeof(content_type), argv[2], false) != 0)) {
        goto error;
    }

    if (at_http_prepare_raw_request(method, url, data_len, content_type) != 0) {
        goto error;
    }

    atsvr_cmd_rsp_ok();
    atsvr_output_msg(">\r\n");
    return 0;

error:
    atsvr_cmd_rsp_error();
    return -1;
}

static int at_http_post_cmd(int sync, int argc, char **argv)
{
    (void)sync;
    return at_http_body_cmd_common(argc, argv, "POST");
}

static int at_http_put_cmd(int sync, int argc, char **argv)
{
    (void)sync;
    return at_http_body_cmd_common(argc, argv, "PUT");
}

int at_http_raw_input(const char *buf, int len)
{
    char method[AT_HTTP_METHOD_MAX_LEN + 1];
    char url[AT_HTTP_URL_MAX_LEN + 1];
    char content_type[AT_HTTP_CONTENT_TYPE_MAX_LEN + 1];
    char *body = NULL;
    uint32_t expected_len = 0;
    int execute_now = 0;

    if ((buf == NULL) || (len <= 0)) {
        return 0;
    }

    at_http_lock();
    if (!s_http_session.raw_pending) {
        at_http_unlock();
        return 0;
    }

    if ((uint32_t)len > (s_http_session.expected_len - s_http_session.received_len)) {
        at_http_reset_pending_locked();
        at_http_unlock();
        BK_LOGE(TAG, "HTTP raw input longer than expected\r\n");
        atsvr_cmd_rsp_error();
        return 1;
    }

    memcpy(s_http_session.body + s_http_session.received_len, buf, len);
    s_http_session.received_len += (uint32_t)len;

    if (s_http_session.received_len < s_http_session.expected_len) {
        at_http_unlock();
        return 1;
    }

    expected_len = s_http_session.expected_len;
    body = s_http_session.body;
    body[expected_len] = '\0';
    os_strlcpy(method, s_http_session.method, sizeof(method));
    os_strlcpy(url, s_http_session.url, sizeof(url));
    os_strlcpy(content_type, s_http_session.content_type, sizeof(content_type));
    s_http_session.body = NULL;
    s_http_session.request_in_progress = 1;
    at_http_reset_pending_locked();
    execute_now = 1;
    at_http_unlock();

    if (!execute_now) {
        return 1;
    }

    if (at_http_execute_request(method, url, content_type, (const uint8_t *)body, expected_len) != 0) {
        at_free(body);
        at_http_set_request_in_progress(0);
        atsvr_cmd_rsp_error();
        return 1;
    }

    at_free(body);
    at_http_set_request_in_progress(0);
    return 1;
}

static const struct _atsvr_command s_http_cmds_table[] = {
    ATSVR_CMD_HADLER("AT+HTTPHEAD", "AT+HTTPHEAD=\"<url>\"",
                     NULL, at_http_head_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+HTTPGET", "AT+HTTPGET=\"<url>\"",
                     NULL, at_http_get_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+HTTPDELETE", "AT+HTTPDELETE=\"<url>\"",
                     NULL, at_http_delete_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+HTTPPOST", "AT+HTTPPOST=\"<url>\",<len>[,\"<content_type>\"]",
                     NULL, at_http_post_cmd, false, 0, 0, NULL, false),
    ATSVR_CMD_HADLER("AT+HTTPPUT", "AT+HTTPPUT=\"<url>\",<len>[,\"<content_type>\"]",
                     NULL, at_http_put_cmd, false, 0, 0, NULL, false),
};

void http_at_cmd_init(void)
{
    int ret;

    if (at_http_init_lock() != 0) {
        return;
    }

    ret = atsvr_register_commands(s_http_cmds_table,
                                  sizeof(s_http_cmds_table) / sizeof(s_http_cmds_table[0]),
                                  "http",
                                  NULL);
    if (ret == 0) {
        BK_LOGI(TAG, "HTTP AT CMDS INIT OK\r\n");
    } else {
        BK_LOGE(TAG, "HTTP AT CMDS INIT FAIL:%d\r\n", ret);
    }
}

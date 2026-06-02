#include <common/bk_typedef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

//#include "common.h"
#include <../../../include/os/os.h>

#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include "lwip/api.h"


#include "atsvr_unite.h"
#include "bk_at_netif.h"




/*global variable defination*/

#define TAG "AT_NETIF"
#define AT_NETIF_TCP_CONN_ID          0
#define AT_NETIF_TCP_HOST_MAX_LEN     63
#define AT_NETIF_RX_BUF_LEN           512
#define AT_NETIF_CIPSEND_MAX_LEN      4096

typedef struct {
	int socket_fd;
	int connected;
	uint16_t remote_port;
	char remote_host[AT_NETIF_TCP_HOST_MAX_LEN + 1];
} at_netif_tcp_client_t;

static at_netif_tcp_client_t s_tcp_client = {
	.socket_fd = -1,
	.connected = 0,
	.remote_port = 0,
	.remote_host = {0},
};

static beken_thread_t s_tcp_rx_thread = NULL;
static volatile int s_tcp_rx_running = 0;
static int s_cipmode = 0;
static int s_cipsend_pending = 0;
static int s_cipsend_remain = 0;

__attribute__((weak)) int at_mqtt_raw_input(const char *buf, int len)
{
	return 0;
}

__attribute__((weak)) int at_http_raw_input(const char *buf, int len)
{
	return 0;
}

static int at_netif_handle_pending_input(const char *buf, int len)
{
	int sent;
	int to_send;
	int consumed = 0;

	if (s_cipsend_pending == 1) {
		if ((!s_tcp_client.connected) || (s_tcp_client.socket_fd < 0)) {
			s_cipsend_pending = 0;
			s_cipsend_remain = 0;
			atsvr_cmd_rsp_error();
			return 1;
		}

		if ((buf == NULL) || (len <= 0)) {
			return 1;
		}

		to_send = (len > s_cipsend_remain) ? s_cipsend_remain : len;
		while (consumed < to_send) {
			sent = send(s_tcp_client.socket_fd, buf + consumed, to_send - consumed, 0);
			if (sent <= 0) {
				s_cipsend_pending = 0;
				s_cipsend_remain = 0;
				atsvr_cmd_rsp_error();
				return 1;
			}
			consumed += sent;
		}

		s_cipsend_remain -= consumed;
		if (len > to_send) {
			s_cipsend_pending = 0;
			s_cipsend_remain = 0;
			BK_LOGE(TAG, "CIPSEND input longer than expected\r\n");
			atsvr_cmd_rsp_error();
			return 1;
		}

		if (s_cipsend_remain == 0) {
			s_cipsend_pending = 0;
			atsvr_cmd_rsp_ok();
		}
		return 1;
	}

	if (at_mqtt_raw_input(buf, len) == 1) {
		return 1;
	}

	if (at_http_raw_input(buf, len) == 1) {
		return 1;
	}

	return 0;
}

int atsvr_pending_data_input(const char *buf, int len)
{
	return at_netif_handle_pending_input(buf, len);
}

static int at_netif_parse_u16(const char *str, uint16_t *value)
{
	char *end = NULL;
	unsigned long number;

	if ((str == NULL) || (value == NULL)) {
		return -1;
	}

	errno = 0;
	number = strtoul(str, &end, 10);
	if ((errno != 0) || (end == str) || (*end != '\0') || (number > 65535UL)) {
		return -1;
	}

	*value = (uint16_t)number;
	return 0;
}

static int at_netif_is_tcp_type(const char *type)
{
	if (type == NULL) {
		return 0;
	}

	if ((strcmp(type, "TCP") == 0) || (strcmp(type, "tcp") == 0)) {
		return 1;
	}

	return 0;
}

static void at_netif_reset_tcp_client(void)
{
	s_tcp_rx_running = 0;
	s_cipsend_pending = 0;
	s_cipsend_remain = 0;
	if (s_tcp_client.socket_fd >= 0) {
		close(s_tcp_client.socket_fd);
	}

	s_tcp_client.socket_fd = -1;
	s_tcp_client.connected = 0;
	s_tcp_client.remote_port = 0;
	memset(s_tcp_client.remote_host, 0, sizeof(s_tcp_client.remote_host));
}

static int netif_at_demo(int sync,int argc, char **argv)
{

	BK_LOGI(TAG,"This is NETIF AT DEMO\r\n");
	return 0;
}

int atsvr_transparent_send(const char *buf, int len)
{
	int sent;

	if (at_netif_handle_pending_input(buf, len) == 1) {
		return 1;
	}

	if ((s_cipmode != 1) || (!s_tcp_client.connected) || (s_tcp_client.socket_fd < 0)) {
		return 0;
	}

	if ((buf == NULL) || (len <= 0)) {
		return 1;
	}

	sent = send(s_tcp_client.socket_fd, buf, len, 0);
	if (sent < 0) {
		BK_LOGE(TAG, "transparent send failed\r\n");
	}

	return 1;
}

static void at_netif_tcp_rx_thread(beken_thread_arg_t arg)
{
	int fd;
	int len;
	char buf[AT_NETIF_RX_BUF_LEN + 1];

	while (s_tcp_rx_running) {
		fd = s_tcp_client.socket_fd;
		if (fd < 0) {
			break;
		}

		len = recv(fd, buf, AT_NETIF_RX_BUF_LEN, 0);
		if (len <= 0) {
			break;
		}

		buf[len] = '\0';
		atsvr_output_msg(buf);
		atsvr_output_msg("\r\n");
	}

	s_tcp_rx_running = 0;
	rtos_delete_thread(NULL);
}

static int at_netif_start_tcp_rx(void)
{
	if (s_tcp_rx_running) {
		return 0;
	}

	s_tcp_rx_running = 1;
	return rtos_create_thread(&s_tcp_rx_thread,
						  BEKEN_APPLICATION_PRIORITY,
						  "tcp_rx",
						  at_netif_tcp_rx_thread,
						  1024 * 2,
						  0);
}

static int at_netif_cipstart_cmd(int sync, int argc, char **argv)
{
	int socket_fd = -1;
	uint16_t remote_port = 0;
	uint16_t local_port = 0;
	struct sockaddr_in remote_addr;
	ip_addr_t ip_address;

	if ((argc < 4) || (argc > 5)) {
		BK_LOGE(TAG, "AT+CIPSTART argc error:%d\r\n", argc);
		goto error;
	}

	if (atoi(argv[0]) != AT_NETIF_TCP_CONN_ID) {
		BK_LOGE(TAG, "AT+CIPSTART only supports id=0\r\n");
		goto error;
	}

	if (!at_netif_is_tcp_type(argv[1])) {
		BK_LOGE(TAG, "AT+CIPSTART only supports TCP type\r\n");
		goto error;
	}

	if (at_netif_parse_u16(argv[3], &remote_port) != 0 || (remote_port == 0)) {
		BK_LOGE(TAG, "AT+CIPSTART invalid remote port:%s\r\n", argv[3]);
		goto error;
	}

	if (argc == 5) {
		if (at_netif_parse_u16(argv[4], &local_port) != 0) {
			BK_LOGE(TAG, "AT+CIPSTART invalid local port:%s\r\n", argv[4]);
			goto error;
		}
	}

	if (s_tcp_client.connected) {
		BK_LOGE(TAG, "AT+CIPSTART rejected, tcp client already connected\r\n");
		goto error;
	}

	memset(&remote_addr, 0, sizeof(remote_addr));
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(remote_port);

	if (!inet_aton(argv[2], &remote_addr.sin_addr.s_addr)) {
		if (0 != netconn_gethostbyname(argv[2], &ip_address)) {
			BK_LOGE(TAG, "AT+CIPSTART dns resolve failed, host:%s\r\n", argv[2]);
			goto error;
		}
		memcpy((void *)(&remote_addr.sin_addr), (void *)&ip_address, sizeof(ip_address));
	}

	socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_fd < 0) {
		BK_LOGE(TAG, "AT+CIPSTART socket create failed\r\n");
		goto error;
	}

	if (local_port > 0) {
		struct sockaddr_in local_addr;
		memset(&local_addr, 0, sizeof(local_addr));
		local_addr.sin_family = AF_INET;
		local_addr.sin_addr.s_addr = 0;
		local_addr.sin_port = htons(local_port);

		if (bind(socket_fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
			BK_LOGE(TAG, "AT+CIPSTART bind local port failed:%u\r\n", local_port);
			goto error;
		}
	}

	if (connect(socket_fd, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) != 0) {
		BK_LOGE(TAG, "AT+CIPSTART connect failed\r\n");
		goto error;
	}

	s_tcp_client.socket_fd = socket_fd;
	s_tcp_client.connected = 1;
	s_tcp_client.remote_port = remote_port;
	strncpy(s_tcp_client.remote_host, argv[2], AT_NETIF_TCP_HOST_MAX_LEN);
	s_tcp_client.remote_host[AT_NETIF_TCP_HOST_MAX_LEN] = '\0';

	atsvr_cmd_rsp_ok();
	if (at_netif_start_tcp_rx() != kNoErr) {
		BK_LOGE(TAG, "AT+CIPSTART start rx thread failed\r\n");
	}
	return 0;

error:
	if (socket_fd >= 0) {
		close(socket_fd);
	}
	atsvr_cmd_rsp_error();
	return -1;
}

static int at_netif_cipstop_cmd(int sync, int argc, char **argv)
{
	if ((argc < 1) || (argc > 2)) {
		BK_LOGE(TAG, "AT+CIPSTOP argc error:%d\r\n", argc);
		goto error;
	}

	if (atoi(argv[0]) != AT_NETIF_TCP_CONN_ID) {
		BK_LOGE(TAG, "AT+CIPSTOP only supports id=0\r\n");
		goto error;
	}

	if (!s_tcp_client.connected) {
		BK_LOGE(TAG, "AT+CIPSTOP no active connection\r\n");
		goto error;
	}

	at_netif_reset_tcp_client();
	atsvr_cmd_rsp_ok();
	return 0;

error:
	atsvr_cmd_rsp_error();
	return -1;
}

static int at_netif_cipsend_cmd(int sync, int argc, char **argv)
{
	int fd;
	int sent;
	int payload_len = 0;
	uint16_t data_len = 0;
	char payload[AT_NETIF_RX_BUF_LEN + 1];

	if (argc < 2) {
		BK_LOGE(TAG, "AT+CIPSEND argc error:%d\r\n", argc);
		goto error;
	}

	if (atoi(argv[0]) != AT_NETIF_TCP_CONN_ID) {
		BK_LOGE(TAG, "AT+CIPSEND only supports id=0\r\n");
		goto error;
	}

	if (!s_tcp_client.connected || s_tcp_client.socket_fd < 0) {
		BK_LOGE(TAG, "AT+CIPSEND no active connection\r\n");
		goto error;
	}

	/* Length mode: AT+CIPSEND=<id>,<len> */
	if ((argc == 2) && (at_netif_parse_u16(argv[1], &data_len) == 0)) {
		if ((data_len == 0) || (data_len > AT_NETIF_CIPSEND_MAX_LEN)) {
			BK_LOGE(TAG, "AT+CIPSEND invalid length:%u\r\n", data_len);
			goto error;
		}

		s_cipsend_pending = 1;
		s_cipsend_remain = data_len;
		atsvr_output_msg(">\r\n");
		return 0;
	}

	for (int i = 1; i < argc; i++) {
		int part_len = strlen(argv[i]);
		if (payload_len + part_len >= AT_NETIF_RX_BUF_LEN) {
			BK_LOGE(TAG, "AT+CIPSEND payload too long\r\n");
			goto error;
		}
		memcpy(&payload[payload_len], argv[i], part_len);
		payload_len += part_len;
		if (i < argc - 1) {
			payload[payload_len++] = ',';
		}
	}
	payload[payload_len] = '\0';

	fd = s_tcp_client.socket_fd;
	sent = send(fd, payload, payload_len, 0);
	if (sent < 0) {
		BK_LOGE(TAG, "AT+CIPSEND send failed\r\n");
		goto error;
	}

	atsvr_cmd_rsp_ok();
	return 0;

error:
	atsvr_cmd_rsp_error();
	return -1;
}

static int at_netif_cipmode_query_cmd(int sync, int argc, char **argv)
{
	char resp[24];

	if (argc != 0) {
		atsvr_cmd_rsp_error();
		return -1;
	}

	snprintf(resp, sizeof(resp), "+CIPMODE:%d\r\n", s_cipmode);
	atsvr_output_msg(resp);
	atsvr_cmd_rsp_ok();
	return 0;
}

static int at_netif_cipmode_setup_cmd(int sync, int argc, char **argv)
{
	int mode;

	if (argc != 1) {
		atsvr_cmd_rsp_error();
		return -1;
	}

	mode = atoi(argv[0]);
	if ((mode != 0) && (mode != 1)) {
		atsvr_cmd_rsp_error();
		return -1;
	}

	s_cipmode = mode;
	atsvr_cmd_rsp_ok();
	return 0;
}


const struct _atsvr_command netif_cmds_table[] = {
	/*STA*/
	ATSVR_CMD_HADLER("AT+NETIF","AT+NETIF",NULL,netif_at_demo,false,0,0,NULL,false),
	ATSVR_CMD_HADLER("AT+CIPSTART","AT+CIPSTART=<id>,<type>,<domain>,<remote_port>[,<local_port>]",NULL,at_netif_cipstart_cmd,false,0,0,NULL,false),
	ATSVR_CMD_HADLER("AT+CIPSTOP","AT+CIPSTOP=<id>",NULL,at_netif_cipstop_cmd,false,0,0,NULL,false),
	ATSVR_CMD_HADLER("AT+CIPSEND","AT+CIPSEND=<id>,<data>",NULL,at_netif_cipsend_cmd,false,0,0,NULL,false),
	ATSVR_CMD_HADLER("AT+CIPMODE","AT+CIPMODE? / AT+CIPMODE=<mode>",at_netif_cipmode_query_cmd,at_netif_cipmode_setup_cmd,false,0,0,NULL,false),

};

void netif_at_cmd_init(void)
{
	int ret;
	ret = atsvr_register_commands(netif_cmds_table, sizeof(netif_cmds_table) / sizeof(netif_cmds_table[0]),"netif",NULL);
	if(0 == ret)
		BK_LOGI(TAG,"NETIF AT CMDS INIT OK\r\n");
}


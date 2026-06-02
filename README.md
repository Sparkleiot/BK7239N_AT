# BK7239N AT 固件

基于 BK 官方 SDK 改造的 BK7239N AT 固件，当前重点支持 WiFi、TCP、MQTT、HTTP/HTTPS 等串口 AT 能力。

这个仓库的顶层 README 面向 GitHub 首页阅读，目标是帮助你快速了解固件能力、串口交互方式和常用测试链路。更细的底层实现和原始 SDK 文档，仍可参考仓库内组件文档。

## 项目简介

当前固件聚焦以下使用场景：

- 通过串口发送基础 AT 指令，验证固件状态与版本信息
- 连接 2.4G WiFi，并查询连接状态或扫描周围 AP
- 建立 TCP 客户端连接，进行发送、断开和简单透传
- 连接 MQTT Broker，完成订阅、发布和大 payload 原始发布
- 发起 HTTP/HTTPS 的 HEAD、GET、POST、PUT、DELETE 请求

## 当前已实现功能

### 基础 AT

- `AT`
- `AT+HELP`
- `AT+VERSION`
- `AT+ATVERSION`
- `AT+ECHO`
- `AT+RST`

### WiFi

- `AT+STASTART`
- `AT+STASTOP`
- `AT+WIFISCAN`
- `AT+WIFISTATUS`

### TCP

- `AT+CIPSTART`
- `AT+CIPSTOP`
- `AT+CIPSEND`
- `AT+CIPMODE`

### MQTT

- `AT+MQTTUSERCFG`
- `AT+MQTTCONN`
- `AT+MQTTSUB`
- `AT+MQTTPUB`
- `AT+MQTTPUBRAW`
- `AT+MQTTCLEAN`

### HTTP/HTTPS

- `AT+HTTPHEAD`
- `AT+HTTPGET`
- `AT+HTTPDELETE`
- `AT+HTTPPOST`
- `AT+HTTPPUT`

## 串口交互约定

### 启动提示

模块启动后会输出类似内容：

```text
https://github.com/Sparkleiot

ready
```

### 基础响应

- 命令执行成功通常以 `OK` 结束
- 命令执行失败返回 `CMDRSP:ERROR`
- 部分命令会在 `OK` 之前或之后附带事件信息

例如：

```text
AT

OK
```

或：

```text
+MQTTCONNECTED:0,"broker.emqx.io",1883

OK
```

### 事件上报

运行过程中可能出现异步事件，例如：

- WiFi 连接相关事件
- MQTT 连接事件
- MQTT 订阅消息上报

常见事件示例：

```text
EVT:WLAN STA CONNECTED
EVT:GOT-IP
+MQTTCONNECTED:0,"broker.emqx.io",1883
+MQTTDISCONNECTED:0
+MQTTSUBRECV:0,"xh/bk7239/test1",427,...
```

### 二阶段发送交互

某些命令需要先声明长度，模块返回 `>` 后再继续发送精确长度的数据：

- `AT+CIPSEND=<id>,<len>`
- `AT+MQTTPUBRAW=0,"topic",len,qos,retain`
- `AT+HTTPPOST="url",len,"content-type"`
- `AT+HTTPPUT="url",len,"content-type"`

需要注意：

- 不是所有二阶段命令都会在最后再次返回 `OK`
- 不同命令的第二阶段完成提示不同，下面的示例已按当前实现写出

## 快速上手

### 1. 基础连通

```text
AT
AT+ECHO=0
AT+VERSION
AT+ATVERSION
```

### 2. WiFi 入网

```text
AT+STASTART="YourSSID","YourPassword"
AT+WIFISTATUS
```

连接成功后通常会看到类似事件：

```text
EVT:WLAN STA CONNECTED
EVT:GOT-IP
```

### 3. TCP 示例

建立 TCP 客户端连接：

```text
AT+CIPSTART=0,TCP,192.168.2.109,1234
```

直接发送文本：

```text
AT+CIPSEND=0,666999
```

长度模式发送：

```text
AT+CIPSEND=0,6
```

模块先返回：

```text
>
```

再继续发送精确 6 字节数据，例如：

```text
666999
```

发送完成后返回：

```text
OK
```

查询或设置透传模式：

```text
AT+CIPMODE?
AT+CIPMODE=1
```

断开连接：

```text
AT+CIPSTOP=0
```

### 4. MQTT 示例

完整链路：

```text
AT+MQTTUSERCFG=0,"bk7239_01","desktop","123456"
AT+MQTTCONN=0,"broker.emqx.io",1883
AT+MQTTSUB=0,"xh/bk7239/test1",0
AT+MQTTPUB=0,"xh/bk7239/test1","{\"msg\":\"hello\"}",0,0
AT+MQTTCLEAN=0
```

连接成功后会先上报：

```text
+MQTTCONNECTED:0,"broker.emqx.io",1883
```

然后命令本身结束为：

```text
OK
```

小数据发布建议使用 `AT+MQTTPUB`。

大数据或原始 payload 建议使用 `AT+MQTTPUBRAW`：

```text
AT+MQTTPUBRAW=0,"xh/bk7239/test1",427,0,0
```

模块先返回：

```text
OK
>
```

然后继续发送“精确 427 字节”的原始内容，不转义，不额外附加回车换行。发送完成后返回：

```text
+MQTTPUB:OK
```

如果当前已经订阅了同一主题，订阅端还会收到：

```text
+MQTTSUBRECV:0,"xh/bk7239/test1",427,...
```

### 5. HTTP/HTTPS 示例

HEAD：

```text
AT+HTTPHEAD="http://httpbin.org/get"
```

GET：

```text
AT+HTTPGET="http://httpbin.org/get?x=1"
```

DELETE：

```text
AT+HTTPDELETE="http://httpbin.org/delete"
```

HEAD/GET/DELETE 成功时，响应结构通常为：

```text
+HTTPSTATUS:200,"OK"
+HTTPHEADERS:<len>
<原始响应头内容>
+HTTPDATA:<len>
<响应体内容，可能按多段重复输出；HEAD 无 body 时不会有该段>
+HTTPDONE

OK
```

POST：

```text
AT+HTTPPOST="http://httpbin.org/post",25,"application/json"
```

PUT：

```text
AT+HTTPPUT="http://httpbin.org/put",24,"application/json"
```

模块先返回：

```text
OK
>
```

然后发送精确长度的 body，例如：

```text
{"msg":"hello http post"}
```

请求完成后输出：

```text
+HTTPSTATUS:200,"OK"
+HTTPHEADERS:<len>
<原始响应头内容>
+HTTPDATA:<len>
<响应体内容>
+HTTPDONE
```

按当前实现，`AT+HTTPPOST` 和 `AT+HTTPPUT` 的第二阶段完成后不会再额外补一个 `OK`。

## AT 指令概览

| 类别 | 指令 | 说明 |
| --- | --- | --- |
| 基础 | `AT` | 基础连通性测试 |
| 基础 | `AT+HELP` | 查看已注册命令 |
| 基础 | `AT+VERSION` | 查询固件版本、chip id、SoC 信息 |
| 基础 | `AT+ATVERSION` | 查询 AT 协议版本 |
| 基础 | `AT+ECHO=0/1/2` | 切换串口回显模式 |
| 基础 | `AT+RST` | 响应成功后立即重启 |
| WiFi | `AT+STASTART="ssid","pwd"` | 连接 WiFi |
| WiFi | `AT+STASTOP` | 断开当前 STA 连接 |
| WiFi | `AT+WIFISCAN` | 扫描周围 AP |
| WiFi | `AT+WIFISTATUS` | 查询 WiFi 状态 |
| TCP | `AT+CIPSTART=0,TCP,host,port[,local_port]` | 建立 TCP 客户端连接 |
| TCP | `AT+CIPSTOP=0` | 断开 TCP 连接 |
| TCP | `AT+CIPSEND=0,data` | 文本模式发送 |
| TCP | `AT+CIPSEND=0,len` | 长度模式发送 |
| TCP | `AT+CIPMODE?` / `AT+CIPMODE=0/1` | 查询或设置透传模式 |
| MQTT | `AT+MQTTUSERCFG=0,"client","user","pass"` | 配置 MQTT 身份 |
| MQTT | `AT+MQTTCONN=0,"host",port` | 连接 MQTT Broker |
| MQTT | `AT+MQTTSUB=0,"topic",qos` | 订阅主题 |
| MQTT | `AT+MQTTPUB=0,"topic","payload",qos,retain` | 小 payload 发布 |
| MQTT | `AT+MQTTPUBRAW=0,"topic",len,qos,retain` | 大 payload 或原始发布 |
| MQTT | `AT+MQTTCLEAN=0` | 断开 MQTT 连接 |
| HTTP | `AT+HTTPHEAD="url"` | 发起 HEAD 请求 |
| HTTP | `AT+HTTPGET="url"` | 发起 GET 请求 |
| HTTP | `AT+HTTPDELETE="url"` | 发起 DELETE 请求 |
| HTTP | `AT+HTTPPOST="url",len[,"content-type"]` | 发起 POST 请求 |
| HTTP | `AT+HTTPPUT="url",len[,"content-type"]` | 发起 PUT 请求 |

## 已知限制与注意事项

- `scan` 是 CLI 命令，不是标准 `AT+...` 指令；AT 固件中应使用 `AT+WIFISCAN`
- TCP 当前仅支持单连接，连接 `id` 固定为 `0`
- `AT+CIPSTART` 当前仅支持 `TCP` 客户端，不支持 UDP
- `AT+CIPSEND` 支持文本模式和长度模式
- `AT+CIPSEND` 长度模式最大为 `4096` 字节
- `AT+CIPMODE=1` 时为简单 TCP 透传模式，非 `AT` 前缀输入会直接透传到当前 TCP 连接
- MQTT 当前 `link id` 固定为 `0`
- MQTT 订阅 QoS 当前仅支持 `0` 或 `1`
- MQTT 大多数字符串参数受内部缓冲区限制，首页不承诺更高上限
- MQTT 小数据建议使用 `AT+MQTTPUB`，大数据或原始 payload 建议使用 `AT+MQTTPUBRAW`
- HTTP 支持 `http://` 与 `https://` URL，协议由 URL 自动判断
- HTTP POST/PUT body 最大为 `4096` 字节
- WiFi 未联网或 STA 未获取 IP 时，MQTT/HTTP 命令应返回错误
- URL 不带 `http://` 或 `https://` 时，HTTP 命令应返回错误
- POST/PUT 长度为 `0` 或超出上限时应返回错误
- 进入 `>` 等待态后，如果发送长度超出声明值，应返回错误
- DNS 解析失败、连接失败、TLS 握手失败、响应头解析失败等场景，应返回错误
- `AT+MQTTCLEAN=0` 会断开连接，但按当前实现不会额外上报 `+MQTTDISCONNECTED:0`

## 参考资料

- 组件级 AT 说明：[components/at_server/README.md](/home/ymy/Code/7239n-git/components/at_server/README.md)
- WiFi AT 参考：[docs/bk7239n/zh_CN/developer-guide/wifi/bk_wifi_at.rst](/home/ymy/Code/7239n-git/docs/bk7239n/zh_CN/developer-guide/wifi/bk_wifi_at.rst)
- MQTT 测试记录：[mqtt-at.md](/home/ymy/.codex/attachments/6c145354-f1be-424c-bb7e-1801d16a58cd/mqtt-at.md)
- HTTP 测试记录：[http-at测试.md](/home/ymy/.codex/attachments/37523c25-51d3-4284-8829-cc1df7013fc9/http-at测试.md)

# 05 UDP/TCP 应用实现

## 本章解决什么问题

10GbE 只是高速二层链路。真正应用还要选择传输方式：

```text
裸 Ethernet：
    最简单、最低开销，但不可路由。

UDP：
    低延迟、易硬件化，适合实时数据流。

TCP：
    可靠传输，适合文件、控制、普通网络应用。

自定义协议：
    可按业务优化，但需要自己处理可靠性和兼容性。
```

## 协议选择

| 场景 | 推荐方式 | 理由 |
| --- | --- | --- |
| 固定点对点高速采集 | UDP 或裸 Ethernet | 延迟低，硬件实现简单 |
| 需要跨网段 | UDP/TCP over IP | 可路由 |
| 可靠文件传输 | TCP | 自动重传和顺序恢复 |
| 金融/测量超低延迟 | 裸 Ethernet 或 UDP | 避免复杂协议栈 |
| 标准服务器通信 | TCP/IP | 兼容系统网络栈 |
| 硬件线速包处理 | UDP/自定义 | TCP 硬件实现复杂 |

## 裸 Ethernet

裸 Ethernet 只构造二层帧：

```text
Destination MAC
Source MAC
Custom EtherType
Payload
FCS
```

优点：

```text
简单
延迟低
不需要 IP/ARP
容易在 PL 中实现
```

缺点：

```text
不能跨三层路由
需要自定义上位机或驱动
生态兼容性差
```

## UDP

UDP 是 10GbE FPGA 项目中最常见的自定义高速传输方案。

优点：

```text
报文边界清晰
状态少
容易硬件实现
可跨 IP 网络
上位机 socket 易接收
```

缺点：

```text
不保证可靠
不保证顺序
可能丢包
需要应用层序号和重传策略
```

### UDP 包构造

```text
Ethernet Header
IPv4 Header
UDP Header
Payload
```

应用层建议加：

```text
magic
version
sequence number
timestamp
payload length
header checksum
```

这样上位机可以判断：

```text
是否丢包
是否乱序
是否版本不匹配
延迟和抖动
```

### UDP TX 伪代码

```c
int send_sample_udp(const u8 *sample, u16_t len)
{
    u8 *frame = alloc_tx_buffer();
    struct app_hdr hdr;

    hdr.magic = 0xA55A;
    hdr.version = 1;
    hdr.seq = tx_sequence++;
    hdr.timestamp = read_hw_timestamp();
    hdr.length = len;

    build_eth_header(frame, dst_mac, src_mac, ETH_TYPE_IPV4);
    build_ipv4_header(frame + ETH_HDR_LEN, src_ip, dst_ip, IP_PROTO_UDP);
    build_udp_header(frame + ETH_HDR_LEN + IP_HDR_LEN, src_port, dst_port);
    memcpy(frame + HEADERS_LEN, &hdr, sizeof(hdr));
    memcpy(frame + HEADERS_LEN + sizeof(hdr), sample, len);

    update_ipv4_checksum(frame);
    update_udp_checksum_or_zero(frame);

    return eth10g_send_frame(frame, total_len);
}
```

说明：

```text
FCS 通常由 MAC 添加。
IPv4 header checksum 必须正确。
UDP checksum 在 IPv4 中可以为 0，但建议实现。
```

## TCP

TCP 提供可靠字节流，但在 FPGA 里完整实现复杂。

如果用 CPU/Firmware 协议栈：

```text
硬件负责 10GbE MAC/DMA
Firmware/OS 网络栈负责 TCP
应用使用 socket 或类似 API
```

如果全硬件 TCP：

```text
需要 TCP state machine
需要 sequence/ack 管理
需要窗口管理
需要重传缓存
需要拥塞控制
需要连接关闭处理
```

因此初学和多数工程建议：

```text
高吞吐低延迟数据流：
    UDP + 应用层序号。

需要可靠和兼容：
    Firmware/Linux TCP 栈。
```

### TCP send 伪代码

```c
int tcp_app_send(struct tcp_conn *c, const u8 *data, u32 len)
{
    u32 off = 0;

    while (off < len) {
        u32 room = tcp_send_window(c);
        u32 chunk;

        if (room == 0) {
            wait_for_tcp_ack(c);
            continue;
        }

        chunk = min(room, len - off);
        tcp_write(c, data + off, chunk);
        off += chunk;
    }

    tcp_flush(c);
    return 0;
}
```

核心点：

```text
TCP 是字节流。
send 一次不等于对端 recv 一次。
大数据要按发送窗口分片。
ACK 到来后继续发送。
```

## PL 硬件 UDP 数据面

RTL 数据流：

```text
payload_fifo
  |
udp_header_insert
  |
ipv4_header_insert
  |
ethernet_header_insert
  |
axis_mac_tx
```

状态机：

```text
IDLE
  |
SEND_ETH_HEADER
  |
SEND_IP_HEADER
  |
SEND_UDP_HEADER
  |
SEND_PAYLOAD
  |
SEND_PAD_IF_NEEDED
  |
DONE
```

AXI-Stream 关键：

```text
tvalid/tready 握手必须正确
tlast 只能在包最后一个 beat 拉高
tkeep 要表示最后一拍有效字节
tuser 不应错误标记正常包
```

## RX 解析

硬件 RX 可以按层过滤：

```text
目的 MAC 是否本机/广播
EtherType 是否 IPv4
目的 IP 是否本机
Protocol 是否 UDP
目的端口是否匹配
长度和 checksum 是否正确
```

解析后输出：

```text
payload
source IP
source port
sequence
timestamp
error flags
```

## 应用层可靠性

如果 UDP 需要可靠性，可加：

```text
sequence number
ACK bitmap
retransmission request
drop counter
timestamp
rate control
```

不要把 UDP 当成自动可靠协议。它只负责把 datagram 发出去。

## 性能估算

10GbE 线速对应：

```text
10 Gbps = 1.25 GB/s 原始 bit 速率
```

实际应用吞吐会受影响：

```text
以太网头
IP/UDP/TCP 头
IFG/preamble
包长
DMA 效率
DDR 带宽
CPU 处理能力
cache
中断频率
```

大包比小包更容易接近线速，因为每个包的固定开销更低。

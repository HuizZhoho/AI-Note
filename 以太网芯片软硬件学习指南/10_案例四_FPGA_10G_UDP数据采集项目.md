# 10 案例四：FPGA 10G UDP 数据采集项目

## 项目目标与硬件框图

目标：把 FPGA 内部高速采集数据通过 10GbE UDP 发到 PC，实现高吞吐、低延迟、可抓包验证的数据链路。

典型硬件：

```text
ADC / Sensor / PL 数据源
  |
AXI-Stream / FIFO
  |
UDP Packet Engine
  |
10G Ethernet MAC
  |
10GBASE-R PCS/PMA
  |
GT Transceiver
  |
SFP+ 光模块
  |
光纤 / DAC
  |
PC 10G NIC
  |
上位机采集程序
```

代表架构以 Xilinx/AMD FPGA 常见 10GbE IP 为学习锚点，不绑定具体 Vivado IP 版本。精确端口名、寄存器和时钟约束以实际 IP 文档为准。

## 能力边界

必须先明确：

```text
ZYNQ7010 PS GEM 只支持 10/100/1000M。
10GbE 通常需要 FPGA GT 或专用 10G PHY/MAC。
10G UDP 数据面通常由 RTL 硬件完成。
Firmware 负责配置、状态监控、DMA/buffer 和异常恢复。
```

10G UDP 项目不是“把 lwIP 速率调大”。10GbE 的关键在高速串行、PCS/PMA、MAC、AXI-Stream、DMA 和上位机接收能力。

## 硬件设计检查点

| 模块 | 检查项 |
| --- | --- |
| SFP+ | TX/RX 差分、LOS、TX_DISABLE、MOD_ABS、I2C 管理 |
| GT | 参考时钟频率、lane 极性、复位、PLL lock |
| PCS/PMA | 10GBASE-R、64b/66b、block lock、hi BER |
| MAC | FCS、pause、jumbo、统计计数、AXI-Stream 宽度 |
| Clock | GT refclk、TX/RX user clock、AXI clock、CDC |
| Reset | GT reset、PCS reset、MAC reset、DMA reset 顺序 |
| DDR/DMA | 带宽、burst、buffer 对齐、cache 一致性 |
| PC NIC | MTU、10G link、ring size、驱动 offload |

如果使用光模块，还要确认：

```text
模块速率是否支持 10G。
光纤类型是否匹配。
模块是否被板卡或交换机白名单限制。
LOS 是否正常。
TX_DISABLE 是否释放。
```

## 数据面分层

```text
PMD：
    SFP+ 模块、光纤、差分物理媒介。

PMA：
    GT 收发器、CDR、串并转换。

PCS：
    64b/66b 编码、block lock、扰码。

MAC：
    以太网帧、FCS、帧间隔、PAUSE。

Packet Engine：
    Ethernet/IP/UDP 头生成、checksum、payload 分片。

DMA/FIFO：
    数据缓存、跨时钟域、背压。

Firmware：
    配置寄存器、link 状态、统计、错误恢复。
```

AXI-Stream 信号：

```text
tdata：数据。
tkeep：哪些字节有效。
tvalid：发送方数据有效。
tready：接收方可以接收。
tlast：一个包结束。
tuser：错误、sideband 或元信息。
```

`tlast` 错误会导致包边界错，表现为抓包长度异常或 MAC 报错。

## Firmware 初始化流程

```text
1. 配置系统时钟和 reset。
2. 初始化 GT，等待 PLL lock。
3. 初始化 PCS/PMA，等待 block lock。
4. 初始化 10G MAC，设置本机 MAC、MTU、FCS、pause。
5. 初始化 UDP Packet Engine，设置本机/目标 MAC、IP、端口。
6. 初始化 DMA/FIFO。
7. 读取 SFP+ 状态。
8. 等待 link up。
9. 清零统计计数。
10. 允许数据源发送。
```

伪代码：

```c
int eth10g_system_init(void)
{
    reset_assert_all();
    clock_enable_all();
    reset_release_gt();

    if (wait_gt_pll_lock() != 0) {
        return -1;
    }

    reset_release_pcs();
    if (wait_pcs_block_lock() != 0) {
        return -1;
    }

    mac10g_config_local_mac(LOCAL_MAC);
    mac10g_set_mtu(9000);
    mac10g_enable_rx_tx();

    udp_engine_set_local_ip(LOCAL_IP);
    udp_engine_set_remote_ip(REMOTE_IP);
    udp_engine_set_ports(LOCAL_PORT, REMOTE_PORT);
    udp_engine_enable();

    dma_init_rings();
    stats_clear();
    return 0;
}
```

## UDP 包格式

发送包：

```text
Ethernet Header
  dst_mac
  src_mac
  ether_type = 0x0800

IPv4 Header
  src_ip
  dst_ip
  protocol = 17
  total_length
  header_checksum

UDP Header
  src_port
  dst_port
  length
  checksum

Payload
  sequence
  timestamp
  sample_count
  sample_data
```

建议应用层 payload 加：

```text
sequence：检测丢包。
timestamp：计算延迟和抖动。
payload_len：自描述长度。
stream_id：区分多个数据源。
crc 或简单校验：检测应用层数据损坏。
```

UDP 不保证可靠，10G 采集项目通常在应用层补序号、统计和可选重传。

## TX 数据路径

```text
PL 数据源产生 sample
  |
写入 FIFO
  |
Packet Engine 取 payload
  |
补 Ethernet/IP/UDP header
  |
计算 IPv4 header checksum
  |
AXI-Stream 送入 10G MAC
  |
MAC 添加 FCS
  |
PCS/PMA 编码
  |
GT 串行发送
  |
SFP+ 出光
```

背压处理：

```text
如果 MAC tready 拉低，Packet Engine 必须暂停。
如果 FIFO 满，数据源必须停、丢弃或记录 overflow。
如果 PC 接收不过来，应降低速率或增大 buffer。
```

## RX 控制路径

如果只做单向采集，RX 仍然有价值：

```text
接收 ARP request 并回复。
接收配置 UDP 命令。
接收 start/stop 控制。
接收上位机 ACK 或速率调整。
```

最小硬件 ARP：

```text
收到广播 ARP request。
检查 target IP 是否本机。
返回 ARP reply，带本机 MAC。
```

如果不实现 ARP，可以在 PC 上静态绑定 ARP：

```bash
arp -s 192.168.1.10 00-0a-35-00-01-02
```

但真实产品建议支持 ARP。

## PC 上位机验证

网卡配置：

```bash
ip link set <iface> up
ip addr add 192.168.1.100/24 dev <iface>
ip link set <iface> mtu 9000
ethtool <iface>
ethtool -S <iface>
```

抓包：

```bash
tcpdump -i <iface> -nn -e udp port 5001
```

吞吐：

```text
吞吐 = 每秒收到的 payload 字节数 * 8。
10GbE 线速不是应用 payload 10Gbps。
以太网头、IP/UDP 头、FCS、IFG、preamble 都有开销。
Jumbo frame 可以提高有效载荷比例。
```

## ILA 调试点

建议加 ILA：

```text
GT/PCS 状态：
    pll_lock、block_lock、hi_ber、rx_reset_done、tx_reset_done。

MAC AXI-Stream：
    tdata、tkeep、tvalid、tready、tlast、tuser。

Packet Engine：
    state、seq、payload_len、fifo_empty、fifo_full。

DMA/FIFO：
    wr_count、rd_count、overflow、underflow。
```

如果抓包没有任何包：

```text
先看 SFP/GT/PCS lock。
再看 MAC TX 计数。
再看 AXI-Stream 是否有 tvalid/tready/tlast。
再看 PC NIC 是否 link up 和 RX 计数增加。
```

## 常见故障定位

| 现象 | 排查 |
| --- | --- |
| SFP+ link 不 up | 模块、光纤、TX_DISABLE、LOS、GT refclk |
| block lock 失败 | GT reset、极性、参考时钟、速率配置 |
| 有 TX 无抓包 | MAC FCS、PCS、SFP、PC NIC、目标 MAC/IP |
| 抓包长度异常 | tkeep/tlast、UDP length、IP total length |
| 丢包 | PC ring、应用处理慢、FIFO overflow、MTU |
| 吞吐不足 | 小包 PPS、DDR/DMA、AXI 宽度、PC CPU |
| DMA 卡死 | descriptor owner、地址对齐、cache、tlast 缺失 |

## 迁移到不同 FPGA/IP

迁移重点：

```text
10G MAC IP AXI-Stream 宽度。
是否由 MAC 自动生成 FCS。
PCS/PMA 和 GT reset 顺序。
时钟域数量和 CDC。
统计计数寄存器定义。
SFP+ 控制脚和 I2C 地址。
DMA 是否需要 cache flush/invalidate。
```

本案例核心：

```text
10GbE 是硬件高速流水线，Firmware 只做配置、监控、buffer 管理和错误恢复。
```

## 延伸阅读

- [万兆以太网 FPGA + Firmware 实现详解](../万兆以太网_FPGA实现详解/README.md)
- [10GbE 收发数据完整流程](../万兆以太网_FPGA实现详解/03_收发数据完整流程.md)
- [10GbE UDP/TCP 应用实现](../万兆以太网_FPGA实现详解/05_UDP_TCP应用实现.md)

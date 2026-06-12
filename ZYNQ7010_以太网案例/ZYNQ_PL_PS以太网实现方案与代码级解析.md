# ZYNQ PL/PS 以太网实现方案与代码级解析

本文专门解释 ZYNQ7010 上实现以太网通信的不同方案，并给出代码级调用链。重点是理解：

```text
什么时候用 PS GEM
什么时候用 PS GEM + EMIO
什么时候用 PL AXI Ethernet
什么时候才需要 PL 自定义 MAC/UDP
PS 和 PL 如何交换网络数据
代码从 main() 到驱动再到 DMA/PHY 是怎么走的
```

## 1. ZYNQ7010 的 PS 和 PL

ZYNQ7010 由两部分组成：

```text
PS：
    Processing System，ARM Cortex-A9、DDR 控制器、GEM、UART、SPI、I2C 等硬核外设。

PL：
    Programmable Logic，FPGA 逻辑，可实现自定义 AXI IP、DMA、FIFO、协议处理、视频/采集逻辑等。
```

以太网通信可以完全由 PS 完成，也可以由 PL 参与，甚至可以主要在 PL 中实现。

## 2. 方案总览

| 方案 | 数据面位置 | PHY 连接 | 软件复杂度 | 适合场景 |
| --- | --- | --- | --- | --- |
| PS GEM + MIO | PS | PHY 接 PS MIO | 低 | 最常见入门、lwIP/Linux |
| PS GEM + EMIO | PS + PL 引脚 | PHY 接 PL IO | 中 | 板级 PHY 接到 PL 管脚 |
| PL AXI Ethernet | PL MAC + PS 软件 | PHY 接 PL | 高 | PS GEM 不够用、PL 网络接口 |
| PL 自定义 UDP/MAC | PL | PHY 接 PL | 很高 | 超低延迟、固定协议、硬件转发 |
| PS 网络 + PL 数据源 | PS 协议栈，PL 采集 | PHY 多为 PS GEM | 中 | 采集/控制数据经 TCP/UDP 发出 |

最推荐的学习路径：

```text
先掌握 PS GEM + MIO
  |
再理解 PS GEM + EMIO
  |
再学 PS 网络协议栈如何搬运 PL 数据
  |
最后再考虑 PL AXI Ethernet 或自定义协议
```

## 3. 方案一：PS GEM + MIO

### 3.1 架构

```text
RJ45
  |
PHY
  |
RGMII/RMII/MII
  |
PS MIO
  |
PS GEM
  |
DMA
  |
DDR
  |
ARM + lwIP/Linux
```

这是 ZYNQ7010 以太网最常见方案。PL 不参与以太网收发。

### 3.2 Vivado 配置

```text
ZYNQ7 Processing System
  |
MIO Configuration
  |
Enable Ethernet 0 或 Ethernet 1
  |
Enable MDIO
  |
选择 MIO 引脚
  |
配置 UART 和 DDR
```

### 3.3 Vitis 裸机代码调用链

主流程：

```c
int main(void)
{
    init_platform();

    IP4_ADDR(&ipaddr,  192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    lwip_init();

    xemac_add(&server_netif,
              &ipaddr,
              &netmask,
              &gw,
              mac_ethernet_address,
              PLATFORM_EMAC_BASEADDR);

    netif_set_default(&server_netif);
    netif_set_up(&server_netif);

    start_application();

    while (1) {
        xemacif_input(&server_netif);
        transfer_data();
    }
}
```

代码级解释：

```text
init_platform()
    初始化 UART、cache、timer、中断等基础环境。

lwip_init()
    初始化 lwIP 内存池、ARP、IP、ICMP、TCP、UDP。

xemac_add()
    把 PS GEM 注册为 lwIP netif。
    内部会初始化 GEM、DMA、PHY、MAC 地址和底层收发函数。

netif_set_up()
    启用软件网络接口。

start_application()
    创建 TCP/UDP 应用，例如 TCP Echo Server。

xemacif_input()
    从 GEM DMA 接收包，封装成 pbuf，交给 lwIP。
```

### 3.4 底层发送路径

```text
应用调用 tcp_write() 或 udp_sendto()
  |
lwIP 生成 TCP/UDP/IP/Ethernet 数据
  |
netif->linkoutput()
  |
low_level_output()
  |
cache flush
  |
GEM DMA 读取 DDR buffer
  |
GEM 通过 RGMII/RMII 发给 PHY
  |
PHY 发到网线
```

### 3.5 底层接收路径

```text
PHY 从网线收到数据
  |
RGMII/RMII 送给 GEM
  |
GEM DMA 写入 DDR
  |
xemacif_input()
  |
cache invalidate
  |
pbuf_alloc()
  |
netif->input()
  |
lwIP 分发到 ARP/ICMP/TCP/UDP
  |
应用 callback
```

### 3.6 优缺点

优点：

```text
硬件简单
软件资料多
lwIP/Linux 支持成熟
最适合入门和常规产品
```

缺点：

```text
GEM 数量有限
PHY 必须能接到 MIO 或通过 EMIO 引出
极低延迟或硬件线速自定义处理不如 PL 灵活
```

## 4. 方案二：PS GEM + EMIO

### 4.1 架构

```text
RJ45
  |
PHY
  |
RGMII/RMII/MII
  |
PL IO
  |
EMIO
  |
PS GEM
  |
ARM + lwIP/Linux
```

GEM 仍然在 PS，协议栈仍然跑在 ARM 上，只是 GEM 的外部信号不走 MIO，而是通过 EMIO 引到 PL 管脚。

### 4.2 什么时候用

```text
板子上的 PHY 接到了 PL Bank
MIO 引脚不够
板级设计需要经过 PL 管脚复用
```

### 4.3 Vivado 配置差异

```text
Enable GEM
  |
选择 EMIO
  |
把 RGMII/RMII/MDIO 信号引出到 Block Design
  |
Create HDL Wrapper
  |
XDC 约束 PL 管脚
  |
确认 IO 电压和 PHY 一致
```

XDC 关注：

```text
TXD/RXD
TX_CTL/RX_CTL
TXC/RXC
MDC/MDIO
PHY reset
PHY interrupt
IOSTANDARD
PACKAGE_PIN
```

### 4.4 软件代码是否变化

通常应用层代码几乎不变：

```text
main.c 主流程不变
xemac_add() 仍然注册 PS GEM
PLATFORM_EMAC_BASEADDR 仍然指向 XPAR_XEMACPS_x_BASEADDR
```

变化主要来自硬件描述 XSA：

```text
MIO 变成 EMIO
管脚约束由 XDC 决定
PHY reset 可能需要额外 GPIO 控制
```

如果 PHY reset 接 PL GPIO，代码可能增加：

```c
static void reset_phy(void)
{
    XGpioPs gpio;
    XGpioPs_Config *cfg;

    cfg = XGpioPs_LookupConfig(XPAR_XGPIOPS_0_DEVICE_ID);
    XGpioPs_CfgInitialize(&gpio, cfg, cfg->BaseAddr);

    XGpioPs_SetDirectionPin(&gpio, PHY_RESET_PIN, 1);
    XGpioPs_SetOutputEnablePin(&gpio, PHY_RESET_PIN, 1);

    XGpioPs_WritePin(&gpio, PHY_RESET_PIN, 0);
    usleep(10000);
    XGpioPs_WritePin(&gpio, PHY_RESET_PIN, 1);
    usleep(50000);
}
```

注意：`PHY_RESET_PIN` 要按实际原理图和 MIO/EMIO/GPIO 映射填写。

### 4.5 常见问题

```text
Vivado 配成 EMIO 但 XDC 管脚错
IOSTANDARD 与 PHY IO 电压不匹配
RGMII delay 没处理
MDIO 没有上拉
PHY reset 没释放
PHY 地址和软件不一致
```

## 5. 方案三：PL AXI Ethernet

### 5.1 架构

```text
RJ45
  |
PHY
  |
RGMII/GMII/SGMII
  |
PL Ethernet MAC IP
  |
AXI DMA / FIFO
  |
AXI Interconnect
  |
PS DDR
  |
ARM + lwIP/Linux driver
```

这里 MAC 不再使用 PS GEM，而是使用 PL 中的 Ethernet MAC IP，例如 AXI Ethernet 类 IP。PS 通过 AXI 访问 PL MAC 和 DMA。

### 5.2 什么时候用

```text
PS GEM 数量不够
需要更多网口
PHY 接口只方便接 PL
需要在 PL 做包处理
需要和高速 PL 数据流深度耦合
```

### 5.3 硬件模块

典型 Block Design：

```text
ZYNQ7 Processing System
  |
AXI Interconnect
  |
AXI Ethernet
  |
AXI DMA
  |
Processor System Reset
  |
Interrupt concat
  |
RGMII/GMII to PHY
```

关键是数据路径：

```text
AXI Ethernet MAC
  |
AXI Stream TX/RX
  |
AXI DMA
  |
DDR
```

### 5.4 软件代码差异

PS GEM 使用的底层驱动通常是 `xemacps`。

PL AXI Ethernet 使用的驱动通常会变成：

```text
xaxiethernet
xaxidma
或 Xilinx lwIP xadapter 中对应 AXI Ethernet 的适配层
```

应用层仍然可以是 lwIP：

```text
lwip_init()
xemac_add()
netif_set_up()
start_application()
xemacif_input()
```

但 `xemac_add()` 内部会根据 base address 识别不同 MAC 类型：

```text
PS GEM：
    xemacpsif_init()

AXI Ethernet：
    xaxiemacif_init()
```

### 5.5 代码级初始化思路

伪代码：

```c
int init_pl_axi_ethernet_netif(void)
{
    lwip_init();

    if (!xemac_add(&netif,
                   &ipaddr,
                   &netmask,
                   &gw,
                   macaddr,
                   XPAR_AXIETHERNET_0_BASEADDR)) {
        xil_printf("AXI Ethernet add failed\r\n");
        return -1;
    }

    netif_set_default(&netif);
    netif_set_up(&netif);

    return 0;
}
```

重点：

```text
base address 不再是 XPAR_XEMACPS_0_BASEADDR
而是 PL AXI Ethernet 的基地址
BSP 必须包含 AXI Ethernet 和 AXI DMA 驱动
中断必须正确连接到 PS GIC
DMA buffer 和 cache 仍然要处理
```

### 5.6 AXI DMA 关注点

AXI Ethernet 通常通过 AXI DMA 搬运帧。

发送方向：

```text
lwIP pbuf
  |
拷贝/映射到 DMA buffer
  |
cache flush
  |
AXI DMA MM2S
  |
AXI Stream
  |
AXI Ethernet TX
```

接收方向：

```text
AXI Ethernet RX
  |
AXI Stream
  |
AXI DMA S2MM
  |
DDR buffer
  |
cache invalidate
  |
lwIP pbuf
```

常见错误：

```text
DMA 中断没接
BD ring 没初始化
cache 未 flush/invalidate
buffer 地址未对齐
AXI Stream tlast/tkeep 异常
PHY 时钟或 reset 异常
```

## 6. 方案四：PL 自定义 MAC/UDP

### 6.1 架构

```text
RJ45
  |
PHY
  |
RGMII/GMII
  |
PL 自定义 MAC
  |
PL ARP/IP/UDP 或固定协议逻辑
  |
FIFO/BRAM/AXI
  |
PS 可选参与配置和管理
```

### 6.2 什么时候考虑

只有在这些需求明确时才建议：

```text
极低延迟
固定 UDP 协议
硬件线速处理
PS 参与会成为瓶颈
不需要完整 TCP/IP 协议栈
需要精确时序
```

不建议初学者一开始就做完整 PL TCP，因为 TCP 状态机复杂：

```text
三次握手
重传
窗口
乱序
拥塞控制
四次挥手
```

PL 更适合先做：

```text
Ethernet + ARP + IPv4 + UDP
```

### 6.3 PL UDP 发送逻辑

固定 UDP 包发送需要硬件拼帧：

```text
Ethernet Header
  |
IPv4 Header
  |
UDP Header
  |
Payload
  |
FCS
```

字段包括：

```text
目的 MAC
源 MAC
EtherType 0x0800
源 IP / 目的 IP
Protocol = 17
源端口 / 目的端口
IP checksum
UDP checksum 可选但建议正确实现
```

### 6.4 PL UDP 接收逻辑

接收过滤：

```text
检查目的 MAC 是否匹配或广播
检查 EtherType 是否 IPv4
检查目的 IP 是否本机
检查 Protocol 是否 UDP
检查目的端口是否匹配
提取 payload
写入 FIFO/BRAM/AXI Stream
```

### 6.5 PS 在自定义 PL 网络中的角色

PS 可以做：

```text
配置本机 MAC/IP/端口
配置目标 MAC/IP/端口
读取统计计数
处理 ARP 表
通过 AXI-Lite 配置 PL 寄存器
通过 DMA 取大块数据
```

示例寄存器设计：

```text
0x00 control
0x04 status
0x08 local_mac_low
0x0C local_mac_high
0x10 local_ip
0x14 local_udp_port
0x18 remote_mac_low
0x1C remote_mac_high
0x20 remote_ip
0x24 remote_udp_port
0x28 tx_packet_count
0x2C rx_packet_count
0x30 error_count
```

PS 代码：

```c
#define ETH_PL_BASE XPAR_MY_ETH_UDP_0_S00_AXI_BASEADDR

static void pl_eth_write(u32 offset, u32 value)
{
    Xil_Out32(ETH_PL_BASE + offset, value);
}

static u32 pl_eth_read(u32 offset)
{
    return Xil_In32(ETH_PL_BASE + offset);
}

void pl_udp_config(void)
{
    pl_eth_write(0x10, 0xC0A8010A); /* 192.168.1.10 */
    pl_eth_write(0x14, 5001);
    pl_eth_write(0x20, 0xC0A80164); /* 192.168.1.100 */
    pl_eth_write(0x24, 5001);
    pl_eth_write(0x00, 0x00000001); /* enable */
}
```

## 7. 方案五：PS 协议栈 + PL 数据源

### 7.1 架构

这是很多 ZYNQ 产品最实用的方案：

```text
PL 采集/处理数据
  |
AXI FIFO / BRAM / AXI DMA
  |
DDR 或 PS 内存
  |
PS lwIP/Linux 协议栈
  |
PS GEM
  |
PHY/RJ45
```

PL 不直接实现以太网协议，只负责产生数据。PS 负责 TCP/IP。

### 7.2 适合场景

```text
ADC 数据采集
GPIO/传感器数据上报
视频帧发送
控制命令下发
普通工业以太网
Web/MQTT/Modbus TCP
```

### 7.3 AXI BRAM 方式

PL 把数据写到 BRAM，PS 读取后通过 TCP/UDP 发送。

PS 代码示例：

```c
#define PL_BRAM_BASE XPAR_BRAM_0_BASEADDR
#define SAMPLE_COUNT 256

void send_pl_samples_udp(struct udp_pcb *pcb,
                         const ip_addr_t *dst_ip,
                         u16_t dst_port)
{
    struct pbuf *p;
    u32 *bram = (u32 *)PL_BRAM_BASE;

    p = pbuf_alloc(PBUF_TRANSPORT,
                   SAMPLE_COUNT * sizeof(u32),
                   PBUF_RAM);
    if (p == NULL) {
        return;
    }

    memcpy(p->payload, bram, SAMPLE_COUNT * sizeof(u32));
    udp_sendto(pcb, p, dst_ip, dst_port);
    pbuf_free(p);
}
```

适合：

```text
低速控制
小批量数据
实现简单
```

不适合：

```text
高速连续流
大吞吐视频/采集
```

### 7.4 AXI DMA 方式

PL 通过 AXI Stream 输出数据，AXI DMA 写入 DDR，PS 从 DDR 取数据发网络。

数据路径：

```text
PL AXI Stream
  |
AXI DMA S2MM
  |
DDR buffer
  |
PS cache invalidate
  |
TCP/UDP send
```

PS 伪代码：

```c
#define RX_BUFFER_BASE 0x01000000
#define RX_LENGTH      4096

static XAxiDma AxiDma;

int start_pl_dma_rx(void)
{
    Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_LENGTH);

    return XAxiDma_SimpleTransfer(&AxiDma,
                                  RX_BUFFER_BASE,
                                  RX_LENGTH,
                                  XAXIDMA_DEVICE_TO_DMA);
}

void send_dma_buffer_tcp(struct tcp_pcb *tpcb)
{
    Xil_DCacheInvalidateRange(RX_BUFFER_BASE, RX_LENGTH);
    tcp_write(tpcb,
              (void *)RX_BUFFER_BASE,
              RX_LENGTH,
              TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
}
```

关键点：

```text
DMA 写 DDR 后，CPU 读之前必须 invalidate cache
TCP 发送大块数据要检查 tcp_write() 返回值
TCP send buffer 不够时要分片发送
UDP 单包不能超过 MTU 太多，否则会分片或失败
```

### 7.5 TCP 大数据发送注意

错误写法：

```c
tcp_write(tpcb, big_buffer, big_len, TCP_WRITE_FLAG_COPY);
```

如果 `big_len` 超过 TCP 发送缓冲，可能返回 `ERR_MEM`。

更稳的思路：

```c
u16_t available = tcp_sndbuf(tpcb);
u16_t chunk = MIN(available, remaining);
err = tcp_write(tpcb, ptr, chunk, TCP_WRITE_FLAG_COPY);
```

伪代码：

```c
static err_t send_stream_chunk(struct tcp_pcb *tpcb,
                               const u8 *data,
                               u32 *offset,
                               u32 total)
{
    while (*offset < total) {
        u16_t snd = tcp_sndbuf(tpcb);
        u32 remain = total - *offset;
        u16_t chunk;

        if (snd == 0) {
            break;
        }

        chunk = (remain > snd) ? snd : (u16_t)remain;

        if (tcp_write(tpcb,
                      data + *offset,
                      chunk,
                      TCP_WRITE_FLAG_COPY) != ERR_OK) {
            break;
        }

        *offset += chunk;
    }

    tcp_output(tpcb);
    return ERR_OK;
}
```

真实工程还要用 `tcp_sent()` 回调，在 ACK 到来后继续发送后续数据。

## 8. Linux 下 PS/PL 以太网思路

如果 ZYNQ 跑 Linux，PS GEM 通常由内核驱动管理。

常见路径：

```text
Device Tree 描述 GEM/PHY
  |
macb/cadence gem driver
  |
Linux network stack
  |
socket API
```

用户态代码：

```c
int sock = socket(AF_INET, SOCK_DGRAM, 0);
sendto(sock, buffer, len, 0,
       (struct sockaddr *)&addr, sizeof(addr));
```

PL 数据进入 Linux：

```text
UIO
字符设备驱动
DMAengine
AXI DMA driver
mmap
零拷贝 buffer
```

Linux 优点：

```text
协议栈完整
应用开发方便
支持 SSH/HTTP/MQTT/SNMP 等生态
```

Linux 缺点：

```text
启动慢
实时性不如裸机
驱动和设备树复杂
极低延迟控制难度更高
```

## 9. 方案选择建议

| 需求 | 推荐方案 |
| --- | --- |
| 入门 ping/TCP echo | PS GEM + MIO |
| PHY 接在 PL 管脚 | PS GEM + EMIO |
| 常规产品网络通信 | PS GEM + lwIP/Linux |
| PL 采集数据通过网络发送 | PS 协议栈 + PL 数据源 |
| 多网口或 PS GEM 不够 | PL AXI Ethernet |
| 固定 UDP 超低延迟 | PL 自定义 UDP |
| 完整 Web/MQTT/SNMP | Linux + PS GEM |
| 高精度同步 | 评估 PTP 硬件 timestamp |

## 10. 代码级调试清单

### 10.1 PS GEM 裸机

```text
main() 是否调用 init_platform()
lwip_init() 是否调用
xemac_add() 是否成功
PLATFORM_EMAC_BASEADDR 是否正确
netif_set_up() 是否调用
start_application() 是否调用
while(1) 是否持续调用 xemacif_input()
PHY ID 是否读出
link speed 是否正确
```

### 10.2 TCP Echo

```text
tcp_new_ip_type() 是否返回非 NULL
tcp_bind() 是否 ERR_OK
tcp_listen() 是否返回非 NULL
tcp_accept() 是否注册
accept_callback() 是否进入
tcp_recv() 是否注册
recv_callback() 是否进入
tcp_recved() 是否调用
tcp_write() 是否 ERR_OK
pbuf_free() 是否调用
```

### 10.3 UDP

```text
udp_new() 是否成功
udp_bind() 端口是否正确
udp_recv() 是否注册
udp_sendto() 是否返回 ERR_OK
pbuf 是否释放
PC 防火墙是否放行 UDP
```

### 10.4 PL 数据路径

```text
AXI 地址映射是否正确
PL 寄存器读写是否正常
DMA 初始化是否成功
DMA 中断是否触发
cache 是否 invalidate/flush
buffer 是否对齐
数据长度是否匹配
AXI Stream tlast 是否正确
```

## 11. 常见错误定位

```text
PHY link 不亮：
    查电源、reset、时钟、网线、PHY strap。

PHY ID 读不到：
    查 MDIO/MDC、PHY 地址、MDIO 上拉、Vivado 配置。

ping 不通但 link up：
    查 ARP、IP、netif、xemacif_input()。

TCP 连不上：
    查 start_application()、端口、防火墙、tcp_bind()。

TCP 大数据卡：
    查 tcp_sndbuf()、tcp_sent()、lwIP 内存、cache。

PL 数据不对：
    查 AXI 地址、DMA、cache、字节序、数据有效信号。

PL AXI Ethernet 无收发：
    查 AXI DMA、MAC 中断、PHY reset、时钟、XDC、base address。
```

## 12. 最小掌握总结

```text
最简单可靠：
    PS GEM + MIO + lwIP。

PHY 接 PL 管脚但仍想用 PS GEM：
    PS GEM + EMIO。

PS 负责协议、PL 负责数据：
    最常见产品架构。

PL 直接做以太网：
    适合特殊低延迟/线速需求，复杂度显著更高。

代码调试主线：
    先保证 PHY/GEM/netif 通，再保证 ARP/ping 通，再做 TCP/UDP 应用，再接 PL 数据。
```

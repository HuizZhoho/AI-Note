# 02 FPGA 硬件架构

## 本章解决什么问题

本章从 FPGA 设计角度解释 10GbE 系统由哪些硬件模块组成，以及它们如何连接。

典型架构：

```text
SFP+
  |
GT Transceiver
  |
10G PCS/PMA
  |
10G MAC
  |
AXI-Stream
  |
FIFO / Packet Engine / DMA
  |
DDR / BRAM / 应用逻辑
  |
Firmware 控制寄存器
```

## SFP+

SFP+ 是常见 10GbE 光模块/铜缆模块接口。

常见信号和管理：

```text
TX/RX 高速差分对
TX_DISABLE
TX_FAULT
RX_LOS
MOD_ABS
I2C 管理接口
参考时钟
```

Firmware 可以通过 I2C 读取模块信息：

```text
模块类型
厂商信息
温度
电压
TX/RX 光功率
告警状态
```

硬件关注：

```text
差分阻抗
走线损耗
参考时钟抖动
光模块电源
LOS/TX_FAULT 状态
散热
```

## GT Transceiver

GT 是 FPGA 内部高速串行收发器。

负责：

```text
串并转换
CDR
均衡
预加重
极性控制
时钟恢复
复位状态机
```

10GbE 常见线路速率：

```text
10.3125 Gbps
```

GT 调试重点：

```text
QPLL/CPLL lock
RX reset done
TX reset done
CDR lock
眼图质量
误码计数
极性是否反了
```

## Clocking

10GbE 对时钟质量很敏感。

常见时钟：

```text
GT reference clock：
    给高速收发器使用。

156.25 MHz user clock：
    常见 10G MAC/AXI-Stream 用户侧时钟。

management clock：
    AXI-Lite 或寄存器访问时钟。
```

时钟域示意：

```text
GT clock domain
  |
PCS/MAC user clock domain
  |
AXI-Stream data clock domain
  |
DMA/DDR/CPU clock domain
```

如果跨时钟域，需要：

```text
Async FIFO
CDC synchronizer
握手同步
复位同步
```

## Reset

10GbE 初始化高度依赖复位顺序。

常见顺序：

```text
稳定参考时钟
  |
释放 GT reset
  |
等待 QPLL/CPLL lock
  |
等待 TX/RX reset done
  |
释放 PCS/PMA reset
  |
等待 block lock
  |
释放 MAC reset
  |
释放 DMA 和应用逻辑 reset
```

错误复位顺序可能导致：

```text
link 不 up
block lock 不稳定
AXI-Stream 没数据
DMA 永远等不到包
```

## 10G MAC

MAC 负责以太网帧层。

接口通常包括：

```text
AXI-Stream TX
AXI-Stream RX
AXI-Lite config/status
statistics counters
pause control
reset/status
```

MAC 配置项：

```text
local MAC address
promiscuous mode
jumbo frame enable
FCS insert/check
pause enable
RX/TX enable
```

## AXI-Stream

AXI-Stream 是 FPGA 内部常见数据流接口。

核心信号：

```text
tdata：
    数据。

tkeep：
    每个 byte 是否有效。

tvalid：
    发送方声明当前数据有效。

tready：
    接收方声明可以接收。

tlast：
    一个 packet 的最后一个 beat。

tuser：
    额外边带信息，常用于错误标记。
```

一次包传输：

```text
tvalid && tready
  |
每拍传输 tdata
  |
tlast 表示包结束
```

如果下游来不及接收，会拉低 `tready`，形成 backpressure。

## FIFO 和 CDC

FIFO 用于：

```text
跨时钟域
吸收突发流量
缓冲 backpressure
宽度转换
包边界保持
```

设计要点：

```text
不能丢 tlast
tkeep 要随数据对齐
tuser 错误标记要传递
包模式 FIFO 要避免半包残留
```

## DMA 和 DDR

如果 Firmware 或 CPU 要处理数据，通常需要 DMA。

RX：

```text
MAC RX AXI-Stream
  |
AXI DMA S2MM
  |
DDR buffer
  |
CPU/Firmware 处理
```

TX：

```text
CPU/Firmware 填 DDR buffer
  |
AXI DMA MM2S
  |
MAC TX AXI-Stream
```

DMA 关注：

```text
descriptor ring
buffer alignment
cache flush/invalidate
interrupt coalescing
zero-copy
large packet segmentation
```

## Firmware 控制面

Firmware 不处理每个高速 bit，它负责：

```text
配置寄存器
控制 reset
读取 link 状态
初始化 DMA
分配 buffer
处理中断
维护统计计数
处理异常恢复
把数据交给应用协议栈
```

数据面是硬件流水线，控制面是 Firmware。

## 最小硬件框图

```text
                +-------------------+
SFP+ RX ------> | GT + PCS/PMA      |
SFP+ TX <------ |                   |
                +---------+---------+
                          |
                    XGMII/internal
                          |
                +---------v---------+
                | 10G Ethernet MAC  |
                +----+---------+----+
                     |         |
               AXIS RX       AXIS TX
                     |         |
                +----v---------^----+
                | FIFO / DMA / App  |
                +----+---------+----+
                     |
                    DDR
                     |
                  Firmware
```

# 万兆以太网 FPGA + Firmware 实现详解

本文档集从 FPGA 工程视角解释 10GbE，也就是万兆以太网。重点不是背标准条文，而是理解：

```text
10GbE 协议栈由哪些层组成
SFP+、GT、PCS/PMA、MAC、AXI-Stream、DMA 各自负责什么
Firmware 如何配置和监控硬件
一个包如何从光口进入 FPGA，再进入 DDR 和应用
一个包如何从应用缓冲区发出到 SFP+
出现 link 不 up、丢包、FCS 错误、DMA 卡死时怎么定位
```

这里的 `Firmware` 指固件控制面。用户常写的 `Fireware` 按同义拼写误差处理。

## 能力边界

必须先明确：

```text
ZYNQ7010 PS GEM 只支持 10/100/1000M 以太网，不是 10GbE。
10GbE 通常需要 FPGA/SoC 中的高速串行收发器 GT，或外部 10G PHY/MAC 方案。
本文按 Xilinx/AMD FPGA 通用架构讲，不绑定具体 Vivado IP 版本。
```

典型 10GbE FPGA 架构：

```text
SFP+ 光模块
  |
高速串行差分线
  |
GT Transceiver
  |
10GBASE-R PCS/PMA
  |
10G Ethernet MAC
  |
AXI-Stream
  |
FIFO / DMA / Packet Engine
  |
DDR / BRAM / 应用逻辑
  |
Firmware 控制和管理
```

## 推荐阅读顺序

```text
1. 01_万兆以太网协议栈.md
   先理解 10GBASE-R、64b/66b、PCS/PMA/PMD、XGMII、MAC 和以太网帧。

2. 02_FPGA硬件架构.md
   再理解 SFP+、GT、时钟复位、MAC、AXI-Stream、FIFO、DMA、DDR。

3. 03_收发数据完整流程.md
   把 RX 和 TX 从光口到应用缓冲区完整串起来。

4. 04_Firmware控制与代码级调用链.md
   看固件如何初始化、配置、轮询 link、中断、DMA ring 和 buffer。

5. 05_UDP_TCP应用实现.md
   理解 UDP、TCP、裸 Ethernet、自定义协议在 10GbE 上怎么选。

6. 06_调试与性能优化.md
   学会定位 block lock、FCS、丢包、吞吐不足和时钟复位问题。

7. 术语表.md
   复习核心术语。
```

## 文档目录

- [01 万兆以太网协议栈](./01_万兆以太网协议栈.md)
- [02 FPGA 硬件架构](./02_FPGA硬件架构.md)
- [03 收发数据完整流程](./03_收发数据完整流程.md)
- [04 Firmware 控制与代码级调用链](./04_Firmware控制与代码级调用链.md)
- [05 UDP/TCP 应用实现](./05_UDP_TCP应用实现.md)
- [06 调试与性能优化](./06_调试与性能优化.md)
- [术语表](./术语表.md)

## 和已有资料的关系

本目录关注 10GbE FPGA 实现。建议同时参考：

- [TCP/IP + IPv6 协议详解](../TCP_IP协议详解/README.md)
- [LAN 交换技术详解](../LAN交换技术详解/README.md)
- [ZYNQ7010 以太网案例](../ZYNQ7010_以太网案例/README.md)
- [以太网 PHY / Switch / NIC 芯片软硬件学习指南](../以太网芯片软硬件学习指南/README.md)

## 一句话总览

```text
10GbE 的数据面是高速硬件流水线，Firmware 负责配置、状态监控、DMA buffer 管理和异常恢复。
```

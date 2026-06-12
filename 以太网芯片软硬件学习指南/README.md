# 以太网 PHY / Switch / NIC 芯片软硬件学习指南

这套文档补齐“芯片级视角”的以太网知识。前面已有资料分别讲了 ZYNQ7010 案例、TCP/IP 协议、LAN 交换和 10GbE FPGA 实现；本目录把这些内容往下落到实际芯片：

```text
PHY 芯片：把网线或光模块上的物理信号变成 MAC 能处理的数字数据。
Switch 芯片：在多个端口之间按 MAC/VLAN/ACL/QoS 规则线速转发帧。
NIC 网卡芯片：把主机内存中的数据通过 DMA、队列和 MAC/PHY 发到网络。
```

学习目标不是背某个芯片手册，而是建立可迁移的工程能力：看到原理图、datasheet、寄存器表、驱动代码和抓包现象时，能判断问题在哪一层。

## 推荐阅读顺序

```text
1. 01_PHY芯片硬件与驱动详解.md
   理解 PHY 的供电、时钟、reset、strap、MDIO、RGMII/SGMII、自动协商和驱动初始化。

2. 02_Switch交换芯片架构与配置.md
   理解交换芯片端口、MAC 表、VLAN、STP/LACP/LLDP、CPU 管理口和寄存器/SDK 配置。

3. 03_NIC网卡芯片架构与驱动.md
   理解网卡芯片的 PCIe、DMA ring、descriptor、MSI-X、中断、checksum offload 和驱动收发路径。

4. 04_完整案例_PHY_Switch_NIC联调.md
   用一个 PC 网卡、交换机、ZYNQ/FPGA 板卡的完整案例，把上电、link、ARP、ping、TCP/UDP 和排障串起来。

5. 05_调试排障与学习路线.md
   按“硬件电气 -> PHY link -> MAC 帧 -> 协议 -> 应用”的顺序建立系统排查方法。

6. 术语表.md
   复习中英术语。
```

## 和已有资料的关系

- [ZYNQ7010_以太网案例](../ZYNQ7010_以太网案例/README.md)：嵌入式 PS GEM + PHY + lwIP 的完整案例。
- [TCP_IP协议详解](../TCP_IP协议详解/README.md)：ARP、IPv4/IPv6、ICMP、UDP、TCP、DNS、HTTP 等协议主线。
- [LAN交换技术详解](../LAN交换技术详解/README.md)：透明桥、VLAN、STP、LACP、QoS、交换芯片内部实现。
- [万兆以太网_FPGA实现详解](../万兆以太网_FPGA实现详解/README.md)：10GbE、SFP+、GT、PCS/PMA、MAC、AXI-Stream、DMA 和 Firmware。

## 一张总图

```text
PC CPU / OS / Socket
  |
NIC Driver
  |
PCIe + DMA Ring + NIC MAC
  |
NIC PHY / SerDes
  |
网线 / 光纤
  |
Switch Port PHY
  |
Switch MAC + Parser + Lookup + Queue + Scheduler
  |
Switch Port PHY
  |
网线 / 光纤
  |
板卡 PHY / SFP+
  |
ZYNQ GEM 或 FPGA MAC
  |
DMA / DDR / lwIP 或用户逻辑
```

这张图里最容易混淆的是：

```text
PHY 不是协议栈，它处理物理信号和链路能力。
MAC 不是 IP 层，它处理以太网帧、FCS、地址过滤和收发时序。
Switch 芯片主要转发二层帧，也可能带三层路由、ACL、QoS 和管理 CPU。
NIC 芯片是主机和网络之间的硬件数据通道，重点是 PCIe、DMA、队列和驱动。
```

## 高效学习主线

先按这条线学习：

```text
硬件：电源、时钟、reset、strap、接口电平
  |
管理：MDIO/I2C/SPI/PCIe 配置寄存器
  |
链路：link up、速率、双工、自动协商
  |
帧：MAC 地址、EtherType、VLAN、FCS
  |
搬运：DMA、descriptor、buffer、cache、一致性
  |
协议：ARP、IP、ICMP、UDP、TCP
  |
应用：ping、iperf、echo、抓包、统计计数
```

判断一个以太网问题时，不要先猜软件或协议。先问五个问题：

```text
1. PHY 供电、时钟、reset 正常吗？
2. MDIO/I2C/PCIe 能读到芯片 ID 吗？
3. link 是否 up，速率和双工是否正确？
4. MAC 侧是否有收发计数，FCS/CRC 错误是否增加？
5. 抓包能看到 ARP、ICMP、TCP/UDP 到哪一步？
```

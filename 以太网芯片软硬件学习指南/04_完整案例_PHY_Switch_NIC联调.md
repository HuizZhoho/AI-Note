# 04 完整案例：PHY、Switch、NIC 联调

## 案例目标

用一个完整链路把 PHY、Switch、NIC 和 ZYNQ/FPGA 板卡串起来：

```text
PC 应用
  |
PC NIC 网卡芯片
  |
网线
  |
Switch 交换芯片端口 1
  |
Switch 交换 fabric
  |
Switch 交换芯片端口 2
  |
网线
  |
ZYNQ/FPGA 板卡 PHY
  |
ZYNQ GEM 或 FPGA MAC
  |
lwIP / Firmware / 用户逻辑
```

目标结果：

```text
PC 能 ARP 到板卡 MAC。
PC 能 ping 通板卡。
PC 能通过 TCP echo 或 UDP echo 收发数据。
交换芯片能学到 PC 和板卡的 MAC。
NIC、Switch、PHY 统计计数能解释整个过程。
```

## 硬件准备

假设：

```text
PC IP：192.168.1.100/24
板卡 IP：192.168.1.10/24
Switch Port1 接 PC。
Switch Port2 接 ZYNQ/FPGA 板卡。
两个端口放在 VLAN 10。
PC Port 为 Access VLAN 10。
板卡 Port 为 Access VLAN 10。
```

如果使用 10GbE FPGA：

```text
PC NIC 可能是 10G SFP+。
Switch Port 可能是 SFP+。
板卡侧可能是 SFP+ -> GT -> PCS/PMA -> 10G MAC。
```

如果使用 ZYNQ7010 千兆案例：

```text
板卡侧是 RJ45 PHY -> RGMII/RMII/MII -> PS GEM。
ZYNQ7010 PS GEM 只支持 10/100/1000M，不是 10GbE。
```

## Step 1：上电和芯片识别

先确认所有芯片能被管理接口读到。

```text
PC：
    操作系统能看到 NIC。

Switch：
    管理 CPU 能读到 switch chip ID。

板卡 PHY：
    ZYNQ/Firmware 能通过 MDIO 读到 PHY ID。
```

Linux/PC 侧：

```bash
ip link show
ethtool eth0
lspci -nn | grep -i ethernet
```

ZYNQ 裸机侧：

```text
xemac_add() 成功。
串口打印网卡添加成功。
底层能读到 PHY ID。
```

如果芯片 ID 都读不到，不要看 ARP/TCP，先查硬件电气、reset、时钟、管理总线。

## Step 2：link up

link up 说明物理链路基本成立。

检查：

```text
PC NIC：
    Speed、Duplex、Link detected。

Switch Port1/Port2：
    link、speed、duplex。

板卡 PHY：
    link status、auto-negotiation complete、speed。
```

链路不 up 的常见原因：

```text
网线或光模块不匹配。
SFP 模块不兼容。
强制速率和自协商不一致。
RGMII delay 或 SGMII mode 配错。
PHY reset 没释放。
参考时钟缺失。
```

## Step 3：Switch VLAN 配置

目标：

```text
Port1：PVID 10，untagged member of VLAN 10。
Port2：PVID 10，untagged member of VLAN 10。
CPU Port：可选加入 VLAN 10，用于管理和抓 CPU 报文。
```

交换芯片配置逻辑：

```c
sw_vlan_create(10);
sw_port_set_pvid(PORT_PC, 10);
sw_port_set_pvid(PORT_BOARD, 10);
sw_vlan_add_untagged(10, PORT_PC);
sw_vlan_add_untagged(10, PORT_BOARD);
sw_port_set_stp_state(PORT_PC, STP_FORWARDING);
sw_port_set_stp_state(PORT_BOARD, STP_FORWARDING);
sw_learning_enable(true);
```

排障重点：

```text
同 VLAN 才能二层互通。
Access 口收 untagged，进芯片后打上 PVID。
Access 口出方向通常剥离 tag。
Trunk 口出方向通常保留 tag。
STP blocked 时 link 亮但不转发。
```

## Step 4：第一次 ARP

PC ping 板卡前通常先发 ARP：

```text
PC -> ff:ff:ff:ff:ff:ff
Who has 192.168.1.10? Tell 192.168.1.100
```

交换芯片行为：

```text
收到 ARP request。
学习源 MAC：PC MAC 在 Port1。
目的 MAC 是广播，泛洪到 VLAN 10 的其他端口。
Port2 发给板卡。
```

板卡行为：

```text
PHY 收到帧。
MAC 校验 FCS。
DMA 写入 DDR 或 FPGA 数据面。
lwIP/Firmware 解析 ARP。
发现目标 IP 是自己。
返回 ARP reply。
```

ARP reply：

```text
板卡 -> PC
192.168.1.10 is at board_mac
```

交换芯片再学习：

```text
学习源 MAC：board_mac 在 Port2。
查目的 MAC：PC MAC 在 Port1。
只转发到 Port1，不再泛洪。
```

验证：

```powershell
arp -a
ping 192.168.1.10
```

Switch 侧看：

```text
MAC 表中有：
PC MAC -> Port1, VLAN 10
Board MAC -> Port2, VLAN 10
```

## Step 5：ICMP ping

ARP 完成后，PC 发 ICMP Echo Request。

```text
Ethernet:
    dst = board_mac
    src = pc_mac
    type = IPv4

IPv4:
    src = 192.168.1.100
    dst = 192.168.1.10
    protocol = ICMP

ICMP:
    Echo Request
```

板卡返回：

```text
ICMP Echo Reply
```

如果 ARP 有但 ping 不通：

```text
板卡 IP 配置错误。
防火墙拦截 ICMP。
MAC 收到但 lwIP 没处理。
xemacif_input() 没持续调用。
RGMII/FCS 错误导致 IP 包损坏。
Switch ACL 丢弃 ICMP。
```

## Step 6：TCP Echo 或 UDP Echo

TCP Echo：

```text
PC -> Board：SYN
Board -> PC：SYN ACK
PC -> Board：ACK
PC -> Board：Data
Board -> PC：Same Data
```

ZYNQ/lwIP 代码路径：

```text
main()
  |
  |-- lwip_init()
  |-- xemac_add()
  |-- netif_set_up()
  |-- start_application()
  |
  `-- while (1)
       |-- xemacif_input()
       `-- transfer_data()

start_application()
  |
  |-- tcp_new_ip_type()
  |-- tcp_bind()
  |-- tcp_listen()
  `-- tcp_accept()

recv_callback()
  |
  |-- tcp_recved()
  |-- tcp_write()
  |-- tcp_output()
  `-- pbuf_free()
```

UDP Echo：

```text
udp_new()
udp_bind()
udp_recv()
收到 pbuf 后 udp_sendto() 原样返回
pbuf_free()
```

UDP 更适合入门验证数据面，因为没有三次握手和窗口；TCP 更适合验证完整协议栈和应用回调。

## Step 7：统计计数对照

完整联调时，统计计数比“猜”可靠。

PC NIC：

```text
TX packets 增加。
RX packets 增加。
RX errors 不应增加。
CRC/FCS 错误不应增加。
```

Switch：

```text
Port1 RX/TX 增加。
Port2 RX/TX 增加。
MAC table 有两个终端。
VLAN 10 member 正确。
丢弃计数、ACL drop、STP blocked 不应异常。
```

板卡 PHY/MAC：

```text
PHY link up。
MAC RX/TX frame 计数增加。
FCS error 不应增加。
DMA descriptor 正常回收。
lwIP pbuf 不耗尽。
```

## 完整排障决策树

```text
芯片 ID 读不到
  -> 查供电、reset、时钟、MDIO/I2C/SPI/PCIe。

芯片 ID 正常但 link 不 up
  -> 查网线/光模块、PHY/SerDes 模式、自协商、RGMII/SGMII。

link up 但 ARP 不通
  -> 查 VLAN、PVID、STP、MAC 收发计数、目的 MAC 广播是否被转发。

ARP 通但 ping 不通
  -> 查 IP、ICMP、防火墙、lwIP input、FCS 错误、ACL。

ping 通但 TCP 不通
  -> 查端口、listen、SYN/SYN ACK、tcp_accept、tcp_recv、防火墙。

TCP 通但吞吐低
  -> 查 MTU、窗口、DMA、cache、ring size、interrupt coalescing、CPU 负载。
```

## 本案例一句话总结

```text
PHY 先让链路成立，Switch 负责二层转发，NIC 负责主机 DMA 收发，ZYNQ/FPGA 负责板卡侧 MAC/协议栈或硬件数据面。
```

# 02 Switch 交换芯片架构与配置

## 本章解决什么问题

交换芯片不是“很多个 PHY 接在一起”。它内部有端口 MAC、解析器、MAC 表、VLAN 表、ACL/QoS 表、交换 fabric、队列、调度器和管理 CPU 接口。

典型结构：

```text
Port PHY / SerDes
  |
Port MAC
  |
Parser
  |
Ingress Pipeline
  |
MAC Learning / VLAN / ACL / QoS
  |
Switch Fabric / Shared Buffer
  |
Egress Queue / Scheduler
  |
Port MAC
  |
Port PHY / SerDes
```

通俗理解：Switch 芯片像一个硬件分拣中心。每个包进来后，芯片先看标签和地址，再决定要丢、转发、复制、改标签、限速，最后从一个或多个端口发出去。

## 交换芯片与普通 MCU 的区别

普通 MCU 主要靠 CPU 顺序执行代码。交换芯片的数据转发主要靠硬件流水线并行处理。

```text
管理面：
    CPU/SDK/CLI/SNMP/寄存器配置，负责下发规则。

数据面：
    ASIC pipeline 线速处理数据帧，不会每个包都交给 CPU。
```

如果每个包都交给 CPU，吞吐会非常低。真正的交换性能来自 ASIC 硬件表项和队列。

## 端口类型

交换芯片常见端口：

```text
User Port：
    连接终端、PHY、RJ45 或 SFP。

CPU Port：
    连接管理 CPU，用于收发控制协议、异常包、管理流量。

Cascade / Stack Port：
    多颗交换芯片级联或堆叠。

SerDes Port：
    连接 SGMII、QSGMII、USXGMII、10GBASE-R、XAUI 等高速串行接口。
```

CPU Port 很重要。STP、LLDP、LACP、ARP 管理报文、SNMP、Web 管理等经常需要送到 CPU，而普通数据流量则由硬件转发。

## 核心表项

### MAC 地址表

学习键通常是：

```text
VLAN ID + MAC Address
```

输出是：

```text
出口端口
是否静态
是否老化
是否需要丢弃或送 CPU
```

学习过程：

```text
收到帧
  |
提取源 MAC 和 VID
  |
记录这个 MAC 在入端口
  |
下一次目的 MAC 命中时直接转发到该端口
```

### VLAN 表

VLAN 表决定：

```text
哪些端口属于 VLAN。
入口 untagged 帧应该归到哪个 PVID。
出口是否带 tag。
是否允许该 VLAN 在 trunk 上通过。
```

### ACL/TCAM

ACL 常用匹配项：

```text
入端口、VID、源/目的 MAC、EtherType。
IPv4/IPv6 地址、TCP/UDP 端口、DSCP。
```

动作：

```text
permit、drop、redirect、mirror、remark priority、send to CPU。
```

### QoS 队列

交换芯片会把包放进不同优先级队列。

```text
PCP/802.1p -> 队列
DSCP -> 队列
端口默认优先级 -> 队列
ACL remark -> 队列
```

调度算法可能是严格优先级、WRR、DRR 或混合模式。QoS 不能增加带宽，只能决定拥塞时谁优先。

## Switch 芯片初始化流程

通用 bring-up：

```text
1. 上电，释放 switch reset。
2. 管理 CPU 通过 MDIO/I2C/SPI/PCIe/并行总线读取 chip ID。
3. 加载 strap 或 EEPROM 配置。
4. 初始化 PLL、SerDes、端口 MAC。
5. 配置每个端口的接口模式和速率。
6. 配置 VLAN、PVID、tag/untag 行为。
7. 配置 STP 状态、LACP、LLDP trap。
8. 配置 CPU port。
9. 打开 MAC learning。
10. 打开端口收发。
11. 读取端口 link 和统计计数。
```

伪代码：

```c
static int switch_init(void)
{
    uint32_t chip_id = sw_reg_read(CHIP_ID_REG);
    if (!is_supported_switch(chip_id)) {
        return -1;
    }

    sw_reset_assert();
    delay_ms(10);
    sw_reset_deassert();
    delay_ms(100);

    sw_init_pll();
    sw_init_serdes();

    for (int port = 0; port < PORT_COUNT; port++) {
        sw_port_set_mode(port, PORT_MODE_SGMII);
        sw_port_set_admin(port, true);
        sw_port_set_pvid(port, 1);
    }

    sw_vlan_create(1);
    sw_vlan_add_untagged(1, PORT0 | PORT1 | PORT2 | CPU_PORT);
    sw_cpu_port_enable(CPU_PORT);
    sw_learning_enable(true);

    return 0;
}
```

真实 SDK 会把这些操作封装成 API，但底层本质仍然是写寄存器或表项。

## CPU Port 与控制报文

交换机必须把部分报文送 CPU：

```text
STP BPDU：目的 MAC 01:80:C2:00:00:00
LACP：目的 MAC 01:80:C2:00:00:02
LLDP：目的 MAC 01:80:C2:00:00:0E
未知管理地址、ARP for switch IP、DHCP snooping、IGMP snooping
```

典型策略：

```text
普通数据帧：
    ASIC 查表转发。

控制协议帧：
    trap to CPU，CPU 协议栈或守护进程处理。

异常帧：
    丢弃、镜像或限速送 CPU。
```

如果 CPU trap 没配，STP/LLDP/LACP 可能不工作；如果 trap 没有限速，异常流量可能打爆 CPU。

## Switch 芯片常见软件形态

### 裸寄存器方式

适合小型 unmanaged switch 或简单板卡。

```text
直接通过 MDIO/I2C/SPI 写寄存器。
代码简单，但可维护性差。
```

### 厂商 SDK

中高端交换芯片通常提供 SDK。

```text
API 负责端口、VLAN、L2、L3、ACL、QoS、统计。
驱动隐藏寄存器差异。
```

### Linux DSA

Linux Distributed Switch Architecture 把交换芯片建模为 Linux 网络设备。

常见形态：

```text
CPU eth0 连接 switch CPU port。
lan0/lan1/lan2/wan 等是 switch user ports。
bridge/vlan/filtering 由内核和 switch driver 协同下发硬件规则。
```

示例命令：

```bash
ip link set lan0 up
ip link set lan1 up
ip link add br0 type bridge vlan_filtering 1
ip link set lan0 master br0
ip link set lan1 master br0
bridge vlan add vid 10 dev lan0 pvid untagged
bridge vlan add vid 10 dev lan1 tagged
bridge fdb show
```

## 最小交换案例

目标：让端口 1 和端口 2 在 VLAN 10 内互通，CPU 能管理。

```text
Port1：Access VLAN 10，untagged
Port2：Trunk，允许 VLAN 10 tagged
CPU Port：tagged，管理 CPU 能看到 VLAN 10 报文
```

芯片配置逻辑：

```text
1. 创建 VLAN 10。
2. 设置 Port1 PVID=10。
3. Port1 加入 VLAN 10，出口 untagged。
4. Port2 加入 VLAN 10，出口 tagged。
5. CPU Port 加入 VLAN 10，出口 tagged。
6. 打开 learning。
7. STP 状态设置为 forwarding。
```

排障：

```text
Port link 不 up：
    查 PHY/SerDes。

Port link up 但不通：
    查 VLAN membership、PVID、egress tag mode。

ARP 能看到但 TCP 不通：
    查 ACL、CPU 防火墙、MTU、QoS 丢包。

MAC 表不学习：
    查 learning 开关、STP 状态、端口是否 blocked。
```

## 交换芯片学习重点

```text
先会读端口状态和统计。
再会配 VLAN 和 PVID。
再会看 MAC 表。
再会理解 CPU port 和控制协议 trap。
最后再学 ACL、QoS、L3、组播和堆叠。
```

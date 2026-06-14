# 08 案例二：嵌入式 Linux Switch 芯片网关项目

## 项目目标与硬件框图

目标：用一颗嵌入式 CPU 管理一颗多端口 Switch ASIC，实现多个 LAN/WAN 端口、VLAN 隔离、二层转发和 Linux 网络管理。

典型硬件：

```text
管理 CPU / SoC
  |
CPU MAC
  |
RGMII / SGMII / PCIe / 内部总线
  |
Switch ASIC CPU Port
  |
Switch Parser / Lookup / VLAN / Queue / Scheduler
  |
Port MAC 0..N
  |
PHY / SerDes / RJ45 / SFP
```

代表型号只作为学习锚点：常见嵌入式 Switch ASIC 可以来自 Realtek、Microchip、Marvell、Broadcom、Airoha 等系列。实际寄存器、SDK API、DSA 支持情况必须以芯片资料和内核驱动为准。

## 两种软件形态

### Linux DSA

Linux DSA 把 Switch ASIC 建模为多个 Linux 网口。

```text
eth0：
    CPU MAC，连接 Switch CPU Port。

lan0/lan1/lan2/wan：
    Switch user ports，由 DSA driver 表示。

bridge/vlan：
    Linux bridge 和 VLAN 配置会下发到 Switch 硬件。
```

优点：

```text
接近 Linux 标准网络管理方式。
ip、bridge、ethtool、tcpdump 等工具可用。
便于和 OpenWrt、嵌入式 Linux 集成。
```

### 厂商 SDK

厂商 SDK 通常提供：

```text
port_set_enable()
vlan_create()
l2_addr_add()
acl_rule_add()
qos_queue_set()
stats_get()
```

优点是覆盖芯片全部能力；缺点是移植和维护成本较高。

## 硬件设计检查点

| 模块 | 检查点 |
| --- | --- |
| Switch 电源 | Core、IO、SerDes、PLL 电源和上电顺序 |
| Reset/strap | 启动模式、端口模式、管理接口地址 |
| CPU Port | RGMII/SGMII/PCIe 模式、速率、tag 格式 |
| User Port | 外接 PHY、内部 PHY、SFP、SerDes lane 映射 |
| 管理接口 | MDIO/I2C/SPI/PCIe/SMI 是否能读 chip ID |
| 时钟 | 晶振、SerDes 参考时钟、端口时钟 |
| 中断 | link change、packet trap、错误事件中断 |
| LED | link/activity/speed 指示是否由 Switch 控制 |

Switch 项目最容易忽略 CPU Port。用户端口 link 都亮，但 CPU Port 配错时，Linux 看不到正确流量。

## 底层 bring-up 流程

```text
1. 释放 Switch reset。
2. 读取 chip ID。
3. 初始化 PLL 和 SerDes。
4. 初始化 CPU Port。
5. 初始化每个 user port。
6. 配置默认 VLAN。
7. 配置 MAC learning。
8. 配置控制报文 trap。
9. 使能端口收发。
10. 读取端口 link 和统计计数。
```

SDK 风格伪代码：

```c
int gateway_switch_init(void)
{
    uint32_t id = sw_read_chip_id();
    if (!chip_supported(id)) {
        return -1;
    }

    sw_init_clock();
    sw_init_serdes();

    sw_cpu_port_set(CPU_PORT, PORT_MODE_RGMII, SPEED_1000M);
    sw_cpu_port_enable(CPU_PORT);

    for (int p = 0; p < USER_PORTS; p++) {
        sw_port_set_admin(p, true);
        sw_port_set_learning(p, true);
        sw_port_set_stp_state(p, STP_FORWARDING);
    }

    sw_vlan_create(1);
    sw_vlan_add_untagged(1, PORT_LAN0 | PORT_LAN1 | PORT_LAN2);
    sw_vlan_add_tagged(1, CPU_PORT);

    sw_trap_lldp_to_cpu(true);
    sw_trap_lacp_to_cpu(true);
    sw_trap_stp_to_cpu(true);

    return 0;
}
```

## Linux DSA 配置路径

设备树核心关系：

```dts
ethernet@... {
    phy-mode = "rgmii-id";
    fixed-link {
        speed = <1000>;
        full-duplex;
    };
};

switch@0 {
    ports {
        port@0 { label = "lan0"; };
        port@1 { label = "lan1"; };
        port@2 { label = "wan"; };
        port@cpu {
            label = "cpu";
            ethernet = <&gmac0>;
        };
    };
};
```

命令示例：

```bash
ip link set eth0 up
ip link set lan0 up
ip link set lan1 up
ip link set wan up

ip link add br-lan type bridge vlan_filtering 1
ip link set lan0 master br-lan
ip link set lan1 master br-lan
ip link set br-lan up

bridge vlan add vid 10 dev lan0 pvid untagged
bridge vlan add vid 10 dev lan1 pvid untagged
bridge vlan add vid 10 dev br-lan self
```

查看状态：

```bash
bridge link
bridge vlan show
bridge fdb show
ethtool lan0
ethtool -S lan0
```

## VLAN 与 PVID 案例

目标：

```text
lan0 和 lan1 属于 VLAN 10，互通。
wan 属于 VLAN 20，与 lan0/lan1 隔离。
CPU 能管理 VLAN 10 和 VLAN 20。
```

逻辑：

```text
lan0：PVID 10，出口 untagged。
lan1：PVID 10，出口 untagged。
wan：PVID 20，出口 untagged。
CPU Port：VLAN 10/20 tagged。
```

数据过程：

```text
lan0 收到 untagged ARP。
Switch 入方向加内部 VID=10。
查 VLAN 10 member。
泛洪到 lan1 和 CPU Port，不发到 wan。
lan1 出方向剥离 tag。
CPU Port 出方向保留 tag 给 Linux。
```

如果 PVID 配错，抓包现象通常是：

```text
端口 link 正常。
设备发出 ARP。
对端收不到 ARP，或 ARP 出现在错误 VLAN。
MAC 表中 VID 不符合预期。
```

## 控制报文与 CPU Port

这些报文常需要送 CPU：

```text
STP BPDU：01:80:C2:00:00:00
LACP：01:80:C2:00:00:02
LLDP：01:80:C2:00:00:0E
ARP for switch IP
IGMP/MLD snooping control
DHCP snooping
```

控制报文策略：

```text
普通数据流：
    ASIC 硬件转发。

控制协议：
    trap to CPU。

异常或未知流量：
    丢弃、限速送 CPU 或镜像。
```

CPU trap 必须限速。否则广播风暴、未知单播或攻击流量可能把管理 CPU 打满。

## 应用层网关功能

在 Linux 上可以进一步跑：

```text
DHCP server/client
DNS forwarder
NAT/iptables/nftables
Web 管理页面
SNMP
LLDP daemon
工业协议网关
```

典型 LAN/WAN：

```text
LAN bridge：
    lan0、lan1、lan2 加入 br-lan。

WAN：
    wan 独立三层接口。

路由/NAT：
    br-lan -> wan 做转发。
```

## 验证流程

硬件层：

```text
读 Switch chip ID。
每个端口 link up。
CPU Port link up。
端口 RX/TX 计数增加。
FCS error 不增加。
```

二层层：

```text
bridge fdb show 能看到终端 MAC。
bridge vlan show 符合设计。
ARP 能在同 VLAN 内传播。
不同 VLAN 不互通，除非有三层路由。
```

应用层：

```bash
ping 网关 IP
ping 同 VLAN 主机
iperf3 -s
iperf3 -c <server-ip>
tcpdump -i lan0 -nn -e
```

## 常见故障定位

| 现象 | 定位路径 |
| --- | --- |
| Linux 没有 lan0/lan1 | DSA 驱动、设备树、chip ID、管理接口 |
| 用户端口 link 不 up | PHY/SerDes、网线、模块、端口模式 |
| CPU 能管理但数据不通 | CPU Port tag、DSA tag protocol、bridge/VLAN |
| 同 VLAN 不通 | PVID、VLAN member、STP state、learning |
| MAC 表不学习 | learning 关闭、端口 blocked、源 MAC 被过滤 |
| 管理 CPU 高 | trap 未限速、广播风暴、未知单播泛洪 |
| 吞吐低 | CPU 路径转发、硬件 offload 未生效、MTU/队列 |

## 迁移到不同 Switch 芯片

迁移时不要只改命令，要确认：

```text
Linux 内核是否已有 DSA driver。
DSA tag protocol 是否匹配。
CPU Port 接口和速率是否固定。
端口编号和丝印是否一致。
VLAN egress tag 规则是否和 SDK 定义一致。
统计计数命名是否不同。
控制报文 trap 默认行为是否不同。
```

Switch 项目核心判断：

```text
用户数据应该走 ASIC 硬件快路径。
管理和控制协议才应该上 CPU。
```

## 延伸阅读

- [LAN 交换技术详解](../LAN交换技术详解/README.md)
- [透明桥与交换机原理](../LAN交换技术详解/02_透明桥与交换机原理.md)
- [VLAN 与 802.1Q](../LAN交换技术详解/05_VLAN与802_1Q.md)
- [交换机内部实现](../LAN交换技术详解/09_交换机内部实现.md)

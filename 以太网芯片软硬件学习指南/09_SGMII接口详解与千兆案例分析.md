# 09 SGMII 接口详解与千兆案例分析

## 本章解决什么问题

前面的章节已经分别讲了 PHY、Switch 和项目案例，但 SGMII 往往只在原理图或设备树里一笔带过。真正做项目时，SGMII 不是一句“串行千兆接口”就能解决的。它同时牵涉：

```text
MAC
  |
PCS
  |
SerDes / CDR
  |
PHY 或 Switch CPU Port
  |
MDIO / 设备树 / 驱动 / 自协商 / in-band status
```

如果这几层混在一起看，常见结果就是：

```text
MDIO 能读到 PHY ID，但链路不通。
SerDes lock 了，但 ping 不通。
铜口 link up 了，但 MAC 侧一直没包。
Linux 里写了 phy-mode = "sgmii"，却仍然协商异常。
CPU Port 明明是千兆，DSA 还是收不到用户端口流量。
```

本章把 SGMII 单独拆开，目标是建立一条完整主线：

```text
SGMII 是什么
  ->
为什么 1.25 Gbps 可以承载 10/100/1000M
  ->
MAC / PCS / SerDes / PHY / Switch 各自做什么
  ->
硬件设计和板级检查点
  ->
裸机 bring-up 路径
  ->
Linux phylib / phylink 路径
  ->
两个实际千兆案例
  ->
实际工程软硬件架构和代码级流程分析
  ->
常见故障与证据链
```

## 1. SGMII 到底是什么

`SGMII` 的全称是 `Serial Gigabit Media Independent Interface`。它不是网线协议，也不是 TCP/IP 协议，而是 **MAC 和 PHY 或 Switch CPU Port 之间的串行接口**。

通俗理解：

```text
RGMII：
    像 4 车道并行公路，线多，时序是并行 DDR。

SGMII：
    像 1 条高速专线，线少，靠 PCS 和 SerDes 把数据串起来跑。
```

典型链路：

```text
MAC / DMA / 协议栈
  |
MAC 控制器
  |
PCS
  |
SerDes TX/RX 差分对
  |
SGMII PHY 或 Switch CPU Port
  |
铜口 PHY / Switch Fabric
  |
RJ45 / 其他端口
```

要先记住一句话：

```text
SGMII 解决的是“MAC 侧怎么把数据送给 PHY 或 Switch”。
它不替代 MDIO，不替代铜口自协商，也不等于外部网线已经 link up。
```

## 2. 为什么 1.25 Gbps 可以承载 10/100/1000M

SGMII 常见线速是 `1.25 Gbaud`，并且通常使用 `8b/10b` 编码。

千兆时可以这样理解：

```text
GMII 逻辑侧：
    8bit * 125 MHz = 1 Gbit/s 有效数据

SGMII 线上：
    每 8bit 数据编码成 10bit
    125 MHz * 10bit = 1.25 Gbit/s 线速
```

所以：

```text
1 Gbit/s 有效负载
  +
8b/10b 编码开销
  =
1.25 Gbit/s 串行线速
```

那 10M 和 100M 怎么办？

关键点不是把 SerDes 改成更低速，而是：

```text
SGMII lane 通常仍保持 1.25 Gbaud 工作。
较低速率通过 PCS 侧的速率适配或符号重复来承载。
```

这就是为什么很多项目里你看到：

```text
10M / 100M / 1000M
都跑在同一对 SGMII 差分线上。
```

## 3. SGMII 和 RGMII / 1000BASE-X / QSGMII 的关系

### 3.1 和 RGMII 的区别

| 接口 | 传输方式 | 典型速率 | 板级重点 |
| --- | --- | --- | --- |
| RGMII | 并行 DDR | 10/100/1000M | 时钟相位、delay、IO 电压 |
| SGMII | 串行 SerDes | 10/100/1000M | PCS、SerDes lock、极性、参考时钟 |

工程上最核心的区别是：

```text
RGMII 主要盯并行时序。
SGMII 主要盯 PCS/SerDes 状态、模式和控制字。
```

### 3.2 和 1000BASE-X 的关系

很多器件的数据手册会把 `SGMII` 和 `1000BASE-X` 放在一起，因为它们：

```text
都常见于 1.25 Gbaud 串行 SerDes。
都和 PCS / 8b/10b / control word 有关。
```

但它们不是一回事。

简化理解：

```text
1000BASE-X：
    更接近光口/背板千兆 PCS 语义。

SGMII：
    在类似物理层之上，用控制字表达速度/双工等 MAC-PHY 侧状态。
```

所以一个常见坑是：

```text
两端都说“1.25G SerDes”，但一端配成 1000BASE-X，另一端配成 SGMII，
结果电气上像是通的，链路语义却不对。
```

### 3.3 和 QSGMII 的关系

`QSGMII` 可以理解为：

```text
把 4 路逻辑 SGMII 复用到 1 路更高速的串行接口上。
```

如果你做的是单路 CPU Port 或单路 PHY，对你最重要的不是 QSGMII 本身，而是这条经验：

```text
看到 QSGMII，不要按单路 SGMII 去想 lane。
先确认 lane 映射、端口编号和 SerDes 模式。
```

## 4. SGMII 的软硬件分层

### 4.1 数据面分层

一条典型 SGMII 链路里，常见层次是：

```text
MAC：
    处理以太网帧收发、FCS、地址过滤、DMA 对接。

PCS：
    负责编码、解码、对齐、控制字、速率适配。

SerDes：
    把并行逻辑数据变成高速串行差分信号。

CDR：
    从接收数据流里恢复时钟。

PHY：
    如果外部接铜口，PHY 还负责铜缆侧自协商、均衡、回波消除、link 状态。
```

一个很重要的判断是：

```text
PCS/SerDes lock
    只说明 MAC 侧和 PHY/Switch 侧串行链路大体建立了。

铜口 link up
    说明 PHY 和网线对端完成了物理层协商。
```

这两者不是同一件事。

### 4.2 控制面分层

除了 SGMII 差分对，项目里通常还会同时存在：

```text
MDC / MDIO：
    管理 PHY 寄存器，读取 PHY ID、link、speed、duplex，配置模式和厂商扩展寄存器。

Reset / Strap：
    决定 PHY 地址、MAC 侧接口模式、LED 模式、延时、启动配置。

Interrupt：
    某些 PHY 用中断脚上报 link change。
```

要特别强调：

```text
SGMII 传数据和控制字。
MDIO 负责显式管理寄存器。
二者通常都需要，不是二选一。
```

### 4.3 链路状态怎么传

常见有三种模式：

```text
1. PHY mode：
   软件通过 MDIO 读 PHY 状态，再把 speed/duplex 配给 MAC。

2. In-band mode：
   PHY 通过 SGMII control word 把协商结果传给 MAC。

3. Fixed mode：
   两端约定固定速率和双工，不依赖 MDIO 状态或 in-band 协商。
```

工程上最容易写错的是把模式混用。比如：

```text
PHY 侧强制 fixed 1000M，
MAC 侧却等待 in-band status；

或者 PHY 已经能通过 MDIO 报 link，
MAC 驱动却又错误地强制开启另一套协商逻辑。
```

## 5. 硬件设计与板级检查点

### 5.1 常见信号

一组典型 SGMII 板级信号会包含：

```text
TXP / TXN：
    本端发出的差分对。

RXP / RXN：
    本端接收的差分对。

REFCLK：
    某些器件需要外部参考时钟，频率和方向必须看 datasheet。

MDC / MDIO：
    管理接口。

RESET_N / INT_N：
    复位和中断。

STRAP：
    接口模式、PHY 地址、LED、延时等启动配置。
```

### 5.2 原理图检查清单

| 项目 | 要确认什么 | 典型错误现象 |
| --- | --- | --- |
| 电源 | Core/PLL/IO/模拟电源是否满足 datasheet | SerDes 不锁定、PHY 发热、ID 异常 |
| 参考时钟 | 频率、输入输出方向、抖动规格是否匹配 | PCS 不起、link 抖动 |
| 差分对 | 100 欧姆差分阻抗、对内长度匹配、少过孔少 stub | 间歇误码、只在千兆下异常 |
| AC 耦合 | 是否需要、位置和容值是否按器件要求 | 一端完全无链路 |
| 极性 | lane polarity 是否允许反转或需要寄存器修正 | SerDes 一直不 lock |
| Reset | 极性、拉电阻、释放时序 | MDIO 能读但 PHY 模式错 |
| Strap | PHY 地址、SGMII/1000BASE-X、主从模式是否正确 | 模式不对、地址扫不到 |
| 管理总线 | MDIO 上拉、电压兼容、共享总线地址规划 | 读错 PHY、总线冲突 |

### 5.3 三个高频硬件坑

#### 坑 1：把 SerDes lock 当成最终 link

现象：

```text
MAC 或 PCS 寄存器显示 lock。
但铜口没有 link，或者 PC 侧根本 ping 不通。
```

原因：

```text
SerDes 只证明 MAC 到 PHY 这段串行通道大体建立。
PHY 到网线对端这段仍可能没有完成协商。
```

#### 坑 2：一端配 SGMII，一端配 1000BASE-X

现象：

```text
有时能看到偶发计数变化。
但速度、双工、link 状态传播异常。
```

原因：

```text
电气层近似，不代表 PCS 语义一致。
```

#### 坑 3：忽略参考时钟和 PLL 电源

现象：

```text
MDIO 全正常。
但上电后 SGMII 完全不起，或者热启动偶发恢复。
```

原因：

```text
很多问题不在 MAC 驱动，而在 PLL 电源、参考时钟质量或 reset 时序。
```

## 6. 软件框架：裸机与 Linux

### 6.1 裸机 bring-up 主线

裸机或 bootloader 场景，推荐按这条路径做：

```text
1. 释放 PHY / PCS / SerDes reset
2. 检查参考时钟和 PLL ready
3. 通过 MDIO 读取 PHY ID
4. 配置 PHY 的 MAC 侧模式为 SGMII
5. 配置 MAC / PCS / SerDes 进入 SGMII 模式
6. 按设计决定是否开启 in-band status
7. 启动铜口 auto-negotiation 或强制速率
8. 等待 PCS lock 与链路稳定
9. 读取 speed / duplex
10. 配置 MAC 收发与 DMA
```

伪代码：

```c
int sgmii_port_init(void)
{
    phy_reset_assert();
    serdes_reset_assert();
    delay_ms(10);

    phy_reset_deassert();
    serdes_reset_deassert();

    if (!refclk_ok() || !serdes_pll_locked()) {
        return -1;
    }

    if (mdio_read_phy_id(PHY_ADDR) < 0) {
        return -1;
    }

    phy_set_mac_side_mode(PHY_ADDR, PHY_MAC_IF_SGMII);
    pcs_config_sgmii(MAC_PORT, PCS_MODE_SGMII);

    if (board_uses_inband()) {
        pcs_enable_inband_status(MAC_PORT, true);
    } else {
        pcs_enable_inband_status(MAC_PORT, false);
    }

    phy_restart_autoneg(PHY_ADDR);

    if (!pcs_wait_lock(MAC_PORT, 1000)) {
        return -1;
    }

    if (!board_uses_inband()) {
        struct link_state st = phy_wait_link(PHY_ADDR, 3000);
        mac_set_speed_duplex(MAC_PORT, st.speed, st.full_duplex);
    }

    mac_enable_rx_tx(MAC_PORT, true);
    dma_enable(MAC_PORT, true);
    return 0;
}
```

### 6.2 Linux 里的三种常见模式

Linux 下最重要的是区分 `PHY mode`、`fixed-link` 和 `in-band-status`。

#### 模式 1：普通 PHY mode

```dts
&eth0 {
    phy-mode = "sgmii";
    phy-handle = <&phy0>;
};
```

这表示：

```text
接口类型是 SGMII。
但 MAC 不一定使用 SGMII in-band status。
链路信息主要还是由 phylib 从 PHY 读出来，再交给 MAC 驱动。
```

#### 模式 2：in-band status

```dts
&eth0 {
    phy-mode = "sgmii";
    phy-handle = <&phy0>;
    managed = "in-band-status";
};
```

这表示：

```text
PHY 侧的协商结果通过 SGMII control word 传给 MAC。
MAC 驱动必须允许 PCS / phylink 去接收和确认这个 control word。
这时 MAC 侧不应该再把链路简单地强制拉起或拉低。
```

关键边界：

```text
写了 phy-mode = "sgmii"
不等于
自动开启 in-band status。
```

#### 模式 3：fixed-link

```dts
&eth0 {
    phy-mode = "sgmii";
    fixed-link {
        speed = <1000>;
        full-duplex;
    };
};
```

适用场景：

```text
CPU MAC 连接 Switch CPU Port。
两端固定千兆全双工。
链路不依赖外部 PHY 的 MDIO 协商结果。
```

对 MAC 驱动来说：

```text
fixed-link 和普通 PHY mode 的 MAC 行为很接近。
主要区别不在 MAC 传输协议，而在 link 参数来源是固定值还是来自 PHY。
```

判断原则：

```text
有可管理 PHY，且想用 PHY 寄存器结果：
    用 phy-handle。

两端通过 SGMII control word 传状态：
    用 managed = "in-band-status"。

链路本来就是固定的：
    用 fixed-link。
```

### 6.3 phylib 与 phylink 的角色

Linux 里常见分工可以简单记为：

```text
phylib：
    主要管理 PHY 本身，读写 MDIO，处理 PHY 状态。

phylink：
    管理 MAC、PCS、PHY 之间的协同，尤其适合 SGMII、1000BASE-X、SFP 这类场景。
```

如果 MAC 驱动支持独立 PCS，phylink 往往是更稳妥的模型，因为它能把这些动作分开：

```text
mac_config()
mac_link_up()
mac_link_down()
mac_link_state()
PCS 状态读取与接口模式协调
```

工程上要点不是背 API，而是知道：

```text
SGMII 这类串行接口经常不是“只有一个 PHY 驱动”就能解释完整的。
MAC / PCS / PHY 三方都可能参与链路状态机。
```

## 7. 案例 A：MAC/SerDes -> SGMII PHY -> RJ45

### 7.1 场景目标

目标：让一颗带 SerDes MAC 的 SoC/FPGA 通过外部 `SGMII PHY` 接 RJ45，跑通千兆以太网。

典型硬件：

```text
CPU / 协议栈
  |
DMA
  |
MAC
  |
PCS / SerDes
  |
SGMII PHY
  |
磁性器件 / RJ45
  |
PC / 交换机
```

这一类项目的难点不在 TCP，而在：

```text
PHY MAC-side 模式是否真的是 SGMII
MAC PCS 是否也进了 SGMII
是否使用 in-band
铜口协商结果如何回传到 MAC
```

### 7.2 初始化顺序

推荐顺序：

```text
1. 确认 PHY strap 为正确的 MAC-side 接口模式
2. 读取 PHY ID，确认 MDIO 地址无误
3. 配置 PHY 厂商寄存器，必要时覆盖 strap
4. 配置 MAC PCS 为 SGMII
5. 确认 PCS lock
6. 启动铜口 auto-negotiation
7. 等待对端 link up
8. 读取 speed / duplex，或接收 in-band status
9. 打开 MAC RX/TX 与 DMA
10. 先跑 ARP / ping，再跑 TCP / iperf
```

### 7.3 数据路径

收包路径：

```text
RJ45 输入电信号
  |
PHY 铜口 PMA/PCS 解码
  |
PHY SGMII PCS / SerDes
  |
MAC PCS / SerDes
  |
MAC RX
  |
DMA 写内存
  |
协议栈收包
```

发包路径：

```text
协议栈发包
  |
DMA 取描述符和 buffer
  |
MAC TX
  |
MAC PCS / SerDes
  |
PHY SGMII PCS / SerDes
  |
PHY 铜口发到 RJ45
```

### 7.4 Linux 设备树示例

如果使用 PHY + in-band status：

```dts
&eth0 {
    status = "okay";
    phy-mode = "sgmii";
    phy-handle = <&phy0>;
    managed = "in-band-status";

    mdio {
        #address-cells = <1>;
        #size-cells = <0>;

        phy0: ethernet-phy@1 {
            reg = <1>;
        };
    };
};
```

如果驱动不走 in-band，而是纯 PHY mode：

```dts
&eth0 {
    status = "okay";
    phy-mode = "sgmii";
    phy-handle = <&phy0>;
};
```

### 7.5 裸机排障顺序

优先顺序不要乱：

```text
先看 PHY ID
  ->
再看 PCS lock
  ->
再看铜口 link
  ->
再看 MAC RX/TX 计数
  ->
再看 ARP / ping
```

原因是：

```text
PHY ID 证明管理总线通。
PCS lock 证明 SGMII 串行通道大体通。
铜口 link 证明外部物理链路通。
MAC 计数证明帧开始进入系统。
抓包才是协议层证据。
```

### 7.6 典型故障表

| 现象 | 先看什么 | 常见根因 |
| --- | --- | --- |
| MDIO 能读 ID，但 PCS 不 lock | 参考时钟、AC 耦合、极性、模式 | SGMII/1000BASE-X 配错，差分对接反 |
| PCS lock 了，但铜口不亮 | PHY 铜口侧协商、磁性器件、电源 | 网线侧问题，不是 MAC 问题 |
| 铜口 link up，但 MAC RX 一直是 0 | in-band/fixed/PHY mode 边界 | MAC 没接受到正确链路状态 |
| 100M 正常，1000M 不通 | 差分信号质量、PLL、板级完整性 | 千兆边界更小，误码暴露 |
| ping 通，小流量 TCP 抖动 | FCS/CRC 统计、DMA/cache | 误码或缓存一致性问题 |

## 8. 案例 B：SoC/CPU MAC -> SGMII CPU Port -> Switch ASIC

### 8.1 场景目标

目标：一颗 SoC 通过 SGMII 连接一颗 Switch ASIC 的 CPU Port，由 Linux 管理交换芯片，实现多端口网关或交换设备。

典型硬件：

```text
Linux CPU / SoC
  |
SoC MAC / PCS / SerDes
  |
SGMII
  |
Switch ASIC CPU Port
  |
Switch Fabric
  |
User Port MAC / PHY / SFP
  |
LAN / WAN / 上联端口
```

这类设计和“MAC 直连 PHY”最大的不同是：

```text
CPU Port 的对端不一定是普通 PHY。
它可能是交换芯片里的一个 SerDes + MAC 入口。
链路起来之后，真正的数据转发大多在 Switch ASIC 内部完成。
```

### 8.2 软件框架

常见是两条控制面：

```text
数据面：
    SoC MAC <-> SGMII <-> Switch CPU Port

管理面：
    SPI / I2C / MMIO / MDIO 用来配置 switch 芯片
```

不要把二者混成一条线。很多新手会误以为：

```text
“CPU 和 Switch 已经 SGMII 相连，所以不需要别的管理通道。”
```

这通常是错的。CPU Port 解决的是数据帧进出，Switch 配置往往还要另一条管理接口。

### 8.3 Linux DSA 视角

DSA 里最重要的关系是：

```text
conduit MAC：
    SoC 上真正和 CPU Port 相连的 MAC。

cpu port：
    Switch 面向 conduit 的管理口。

user ports：
    对外的 lan0/lan1/wan 等口。
```

一个固定千兆全双工 CPU Port 的常见写法：

```dts
&eth0 {
    status = "okay";
    phy-mode = "sgmii";
    fixed-link {
        speed = <1000>;
        full-duplex;
    };
};

&switch0 {
    ports {
        port@5 {
            reg = <5>;
            label = "cpu";
            ethernet = <&eth0>;
            phy-mode = "sgmii";
            fixed-link {
                speed = <1000>;
                full-duplex;
            };
        };
    };
};
```

如果硬件和驱动都支持在 CPU Port 使用 in-band status，常见思路是：

```text
不再强制 fixed-link，
而是让 MAC / PCS / switch CPU port 按 SGMII control word 交换状态。
```

但要注意：

```text
这依赖具体 switch 驱动和 MAC 驱动对 phylink / PCS 的支持。
不能因为电气上是 SGMII，就默认软件一定支持 in-band。
```

### 8.4 初始化主线

CPU Port 场景下，推荐顺序：

```text
1. 读取 switch chip ID
2. 初始化 switch PLL / SerDes
3. 配置 CPU Port 为 SGMII
4. 配置 SoC MAC / PCS 为 SGMII
5. 决定 CPU Port 用 fixed-link 还是 in-band
6. 建立默认 VLAN、CPU tag、trap 策略
7. 使能 user ports
8. 用 bridge / vlan / ethtool 观察链路和统计
```

SDK 伪代码：

```c
int switch_cpu_port_init(void)
{
    uint32_t id = sw_read_chip_id();
    if (!chip_supported(id)) {
        return -1;
    }

    sw_init_pll();
    sw_init_serdes();

    sw_cpu_port_set_mode(CPU_PORT, PORT_MODE_SGMII);
    sw_cpu_port_set_speed(CPU_PORT, SPEED_1000M);
    sw_cpu_port_set_duplex(CPU_PORT, FULL_DUPLEX);
    sw_cpu_port_enable(CPU_PORT, true);

    sw_enable_cpu_tag(CPU_PORT, true);
    sw_trap_lldp_to_cpu(true);
    sw_trap_stp_to_cpu(true);
    sw_trap_lacp_to_cpu(true);

    sw_vlan_create(1);
    sw_vlan_add_tagged(1, CPU_PORT);
    sw_vlan_add_untagged(1, PORT_LAN0 | PORT_LAN1);
    return 0;
}
```

### 8.5 数据路径

用户端口上来的普通数据：

```text
User Port
  |
Switch Parser / VLAN / FDB
  |
若命中硬件转发条件：
    直接从其他 user port 转发

若需要送 CPU：
    打 CPU tag 或按 DSA tag 规则送到 CPU Port
  |
SoC MAC 收到后交给 Linux
```

CPU 发出的管理流量：

```text
Linux bridge / IP / 控制协议
  |
SoC MAC
  |
SGMII CPU Port
  |
Switch ASIC
  |
按 VLAN / 端口 / tag 规则送到目标 user port
```

### 8.6 典型故障表

| 现象 | 先看什么 | 常见根因 |
| --- | --- | --- |
| CPU Port link up，但 lan0/lan1 都不通 | VLAN、CPU tag、PVID、默认转发表 | 不是 SGMII 电气问题，而是交换配置问题 |
| 用户端口能互通，CPU 看不到控制报文 | trap 到 CPU、DSA tag、MTU | 控制帧没上送，或 conduit 头尾空间不够 |
| CPU 能 ping switch，自定义业务流不通 | bridge/vlan 配置、tagged/untagged | 逻辑 VLAN 错误 |
| 开机偶发收不到端口流量 | fixed-link / in-band 状态机 | 两端启动顺序和模式不一致 |
| ethtool 看 CPU MAC 正常，但交换统计没涨 | switch 端口使能、CPU Port mode | switch 内部未真正启用该端口 |

## 9. 实际工程软硬件架构

### 9.1 单板硬件架构

一个可落地的 SGMII 千兆工程，硬件不要只画 `SGMII_TX/RX` 四根线。更完整的框图应该是：

```text
电源树
  |
  |-- SoC/FPGA Core / IO / SerDes AVCC
  |-- PHY Core / Analog / PLL / IO
  |-- Switch Core / SerDes / IO

时钟树
  |
  |-- SoC/FPGA 主时钟
  |-- SerDes REFCLK
  |-- PHY/Switch 参考时钟

控制信号
  |
  |-- RESET_N
  |-- INT_N
  |-- strap 电阻

管理总线
  |
  |-- MDIO/MDC 到 PHY
  |-- SPI/I2C/MDIO/MMIO 到 Switch

数据通道
  |
  |-- SGMII TXP/TXN
  |-- SGMII RXP/RXN
  |-- RJ45 / SFP / Switch user ports
```

原理图评审时按这个顺序看：

```text
1. 电源轨是否全，尤其是 SerDes/PLL 模拟电源。
2. 参考时钟频率、方向、抖动规格是否符合两端要求。
3. reset 释放顺序是否保证时钟和电源已经稳定。
4. strap 默认模式是否就是目标模式。
5. MDIO/SPI/I2C 管理通道是否能访问到目标芯片。
6. SGMII 差分对是否阻抗、极性、AC 耦合和长度控制正确。
7. RJ45/SFP/Switch user port 侧是否也满足各自物理层要求。
```

板级调试不要跳过电源和时钟。SGMII 的软件现象经常表现为 `link down`，但根因可能是 SerDes 参考时钟没有起、PLL 电源噪声大、reset 太早释放。

### 9.2 软件架构

实际项目里，SGMII 相关软件通常分四层：

```text
应用层
  |
  |-- ping / iperf / TCP server / Web / 业务协议

协议栈与网络设备层
  |
  |-- lwIP netif
  |-- Linux netdev / DSA / bridge / VLAN

MAC / DMA 驱动层
  |
  |-- MAC 寄存器
  |-- DMA descriptor
  |-- cache flush / invalidate
  |-- 中断 / NAPI / 轮询

链路与物理层管理
  |
  |-- PCS / SerDes
  |-- PHY driver / phylib
  |-- phylink
  |-- MDIO / Switch SDK
```

判断代码应该改哪里时，先按问题现象定位层级：

| 现象 | 通常改哪里 |
| --- | --- |
| PHY ID 读不到 | reset、MDIO 驱动、PHY 地址、GPIO 时序 |
| PCS 不 lock | SerDes/PCS 初始化、参考时钟、接口模式 |
| link up 但 MAC 无包 | phylink、MAC link_up、fixed/in-band 配置 |
| MAC 有包但 lwIP/Linux 无包 | DMA、descriptor、cache、中断/NAPI |
| Linux 有包但 VLAN 不通 | bridge、DSA、PVID、tag/untag、CPU tag |

### 9.3 工程目录建议

裸机或 SDK 工程可以按功能拆文件：

```text
board.c
    电源、reset、GPIO、时钟相关板级初始化。

mdio.c
    MDIO 读写、PHY 地址扫描、PHY ID 读取。

phy_sgmii.c
    PHY 模式、auto-negotiation、link 状态读取。

pcs_serdes.c
    PCS/SerDes 模式、PLL lock、in-band status。

mac_dma.c
    MAC 收发、DMA descriptor、cache 处理。

net_app.c
    ping/echo/UDP/TCP 业务测试。
```

Linux 工程里则通常落在这些位置：

```text
device tree：
    描述 phy-mode、phy-handle、fixed-link、managed、MDIO 节点。

MAC driver：
    配置 MAC/PCS，响应 phylink 回调，打开或关闭收发。

PHY driver：
    通过 MDIO 读取状态，配置 PHY 厂商寄存器。

Switch driver / DSA：
    配置端口、CPU tag、VLAN、FDB、trap、统计计数。
```

## 10. 代码级详细流程分析

这一节用“能迁移到真实工程”的方式看代码。代码不是某个厂商 SDK 的原样实现，而是把真实工程里的职责拆出来。

### 10.1 上电初始化入口

```c
int board_network_init(void)
{
    board_power_check();
    board_clock_enable();
    board_reset_sequence();

    if (mdio_bus_init() != 0) {
        return -1;
    }

    if (sgmii_port_init() != 0) {
        return -1;
    }

    if (mac_dma_init() != 0) {
        return -1;
    }

    return net_stack_init();
}
```

逐段看：

```text
board_power_check()
    不一定真的由软件测电源，但 bring-up 日志里应该确认电源和 reset 状态。

board_clock_enable()
    保证 SerDes REFCLK、PHY/Switch 时钟已经稳定。

board_reset_sequence()
    reset 释放不能早于时钟稳定，否则 strap 采样和 PLL 初始化可能出错。

mdio_bus_init()
    没有管理通道，后面的 PHY 状态都没有证据。

sgmii_port_init()
    建立 MAC-PCS-SerDes-PHY/Switch 之间的链路。

mac_dma_init()
    链路起来后，还要准备 descriptor、buffer 和中断。

net_stack_init()
    最后才是 lwIP 或 Linux 网络栈进入工作。
```

### 10.2 MDIO 扫描与 PHY ID

```c
static int mdio_find_phy(void)
{
    for (int addr = 0; addr < 32; addr++) {
        uint16_t id1 = mdio_read(addr, 0x02);
        uint16_t id2 = mdio_read(addr, 0x03);

        if (id1 == 0xffff || id1 == 0x0000) {
            continue;
        }

        log_info("phy addr=%d id=%04x:%04x", addr, id1, id2);
        return addr;
    }

    return -1;
}
```

这段代码解决的问题：

```text
确认 MDIO/MDC 电气和时序是通的。
确认 PHY 地址没有和 strap 预期冲突。
确认软件后续写寄存器不会写到错误地址。
```

常见误判：

```text
读到 0xffff：
    经常是 MDIO 上拉、PHY 未释放 reset、地址不存在。

读到 0x0000：
    经常是总线被拉低、PHY 电源异常、MDC/MDIO 连接错误。

读到一个 ID：
    只说明管理通道通，不说明 SGMII 或铜口已经通。
```

### 10.3 SGMII PHY 初始化

```c
static int phy_config_for_sgmii(int phy_addr)
{
    phy_soft_reset(phy_addr);
    phy_wait_reset_done(phy_addr);

    phy_select_page(phy_addr, PHY_VENDOR_PAGE);
    phy_write(phy_addr, PHY_MAC_IF_REG, PHY_MAC_IF_SGMII);
    phy_write(phy_addr, PHY_SGMII_CTRL_REG, PHY_SGMII_AN_ENABLE);
    phy_select_page(phy_addr, PHY_STANDARD_PAGE);

    phy_restart_autoneg(phy_addr);
    return 0;
}
```

逐段看：

```text
phy_soft_reset()
    把 PHY 拉回已知状态，避免 bootloader 或 strap 留下不可控配置。

phy_select_page()
    很多 PHY 的 SGMII 模式寄存器在厂商扩展页，不在标准 0x00~0x1f。

PHY_MAC_IF_SGMII
    明确告诉 PHY：MAC 侧不是 RGMII，也不是 1000BASE-X。

PHY_SGMII_AN_ENABLE
    决定 PHY 是否通过 SGMII control word 向 MAC 传递状态。

phy_restart_autoneg()
    重新启动铜口侧协商，让 link/speed/duplex 状态重新生成。
```

真实项目要注意：

```text
寄存器名和页号必须以具体 PHY datasheet 为准。
不要把 RGMII delay、LED、EEE、低功耗配置照搬到 SGMII 项目里。
```

### 10.4 PCS/SerDes 初始化

```c
static int pcs_serdes_config(int port, bool inband)
{
    serdes_power_on(port);
    serdes_set_refclk(port, REFCLK_125M);
    serdes_set_line_rate(port, LINE_RATE_1G25);
    serdes_set_polarity(port, NORMAL_POLARITY);

    if (!serdes_wait_pll_lock(port, 100)) {
        return -1;
    }

    pcs_set_protocol(port, PCS_PROTO_SGMII);
    pcs_enable_autoneg(port, inband);
    pcs_restart(port);

    return pcs_wait_link_timer(port, 1000);
}
```

逐段看：

```text
serdes_power_on()
    SerDes 常有独立电源域或电源门控。

serdes_set_refclk()
    频率不对时，后面所有 PCS 状态都不可信。

serdes_set_line_rate()
    SGMII 常见是 1.25 Gbaud，不是 125 MHz 低速并行线。

serdes_set_polarity()
    某些板子 TX/RX 极性接反后，可以由 SerDes 寄存器修正。

serdes_wait_pll_lock()
    这是电气层证据，但不是协议层 link。

pcs_set_protocol()
    明确 PCS 语义是 SGMII，不是 1000BASE-X。

pcs_enable_autoneg()
    对应 in-band status 是否启用。
```

调试时建议打印这些状态：

```text
SerDes PLL lock
RX signal detect
PCS block lock / code group sync
SGMII autoneg complete
resolved speed / duplex
```

### 10.5 MAC link_up 处理

```c
static void mac_link_up(int port, int speed, bool full_duplex)
{
    mac_disable_rx_tx(port);

    mac_set_speed(port, speed);
    mac_set_duplex(port, full_duplex);
    mac_clear_stats(port);

    mac_enable_rx_tx(port);
}
```

为什么要这样写：

```text
先关 RX/TX：
    避免改速率和双工时收发状态机处于不一致状态。

设置 speed/duplex：
    MAC 必须知道当前速率，否则 inter-packet gap、流控和时钟域处理可能错误。

清统计可选：
    bring-up 阶段清一次，后面看到的错误计数更干净。

最后开 RX/TX：
    链路参数稳定后再放行数据。
```

如果使用 Linux phylink，这类动作通常在 MAC 驱动回调里：

```c
static void mymac_mac_link_up(struct phylink_config *config,
                              struct phy_device *phy,
                              unsigned int mode,
                              phy_interface_t interface,
                              int speed,
                              int duplex,
                              bool tx_pause,
                              bool rx_pause)
{
    struct mymac *priv = container_of(config, struct mymac, phylink_config);

    mymac_stop(priv);
    mymac_set_interface(priv, interface);
    mymac_set_speed(priv, speed);
    mymac_set_duplex(priv, duplex);
    mymac_set_pause(priv, tx_pause, rx_pause);
    mymac_start(priv);
}
```

这段代码要读懂的是：

```text
interface：
    告诉 MAC 当前是 SGMII、RGMII、1000BASE-X 等哪种接口。

speed/duplex：
    可能来自 PHY、fixed-link 或 in-band status。

tx_pause/rx_pause：
    来自协商结果，影响 MAC pause frame 行为。
```

### 10.6 DMA 收包路径

```c
static int mac_rx_poll(void)
{
    struct dma_desc *desc = rx_ring_get_completed();
    if (!desc) {
        return 0;
    }

    cache_invalidate(desc->buf, desc->len);

    if (desc->status & DMA_RX_ERR) {
        mac_stat.rx_error++;
        rx_ring_recycle(desc);
        return -1;
    }

    netif_receive(desc->buf, desc->len);
    rx_ring_recycle(desc);
    return 1;
}
```

逐段看：

```text
rx_ring_get_completed()
    DMA 硬件已经把一帧写进内存，descriptor 状态归还给 CPU。

cache_invalidate()
    裸机和很多 SoC 场景必须做，否则 CPU 可能读到旧缓存。

DMA_RX_ERR
    如果这里错误增加，要看 MAC FCS、长度错误、FIFO overflow 等统计。

netif_receive()
    到这一步才进入 lwIP 或 Linux 网络栈。

rx_ring_recycle()
    descriptor 必须及时还给硬件，否则很快收包耗尽。
```

学习时可以按这个顺序打点：

```text
MAC RX frame counter
  ->
DMA completed descriptor
  ->
cache invalidate 后的 Ethernet header
  ->
ARP/IP 协议栈入口
```

### 10.7 Switch CPU Port 与 DSA 的代码关系

Switch 场景下，驱动初始化常见主线是：

```c
static int switch_setup(void)
{
    sw_read_chip_id();
    sw_reset_pipeline();
    sw_init_serdes();

    sw_port_set_mode(CPU_PORT, PORT_MODE_SGMII);
    sw_port_set_fixed_link(CPU_PORT, 1000, true);
    sw_cpu_tag_enable(CPU_PORT, true);

    sw_vlan_init_default();
    sw_trap_control_frames();
    sw_enable_learning();
    sw_enable_ports();

    return 0;
}
```

逐段看：

```text
sw_init_serdes()
    解决 CPU MAC 到 Switch CPU Port 的 SGMII 通道。

sw_port_set_fixed_link()
    固定 CPU Port 千兆全双工，避免等待不存在的外部 PHY。

sw_cpu_tag_enable()
    DSA 或厂商 tag 用来告诉 CPU：包来自哪个 user port，要发到哪个 user port。

sw_vlan_init_default()
    没有 VLAN/PVID，很多包会被交换芯片直接丢掉。

sw_trap_control_frames()
    STP、LLDP、LACP、管理 ARP 等控制帧必须按策略送 CPU。
```

对应 Linux 命令验证：

```bash
ip link set eth0 up
ip link set lan0 up
ip link set lan1 up
bridge link
bridge vlan show
bridge fdb show
ethtool -S lan0
ethtool -S eth0
```

判断依据：

```text
eth0 统计增长：
    CPU Port 和 conduit MAC 有数据。

lan0/lan1 统计增长：
    user port 有真实流量。

bridge fdb 有学习项：
    二层学习路径通。

tcpdump 在 eth0 能看到 DSA tag：
    CPU tag 路径通，但普通用户抓 lan0 时通常不直接看 tag。
```

### 10.8 推荐逐步学习路线

按下面 6 步学，最稳：

```text
第 1 步：只读 ID
    写 mdio_read()，读 PHY ID 或 switch chip ID。

第 2 步：只看 SerDes
    配 PCS/SerDes，确认 PLL lock、PCS lock。

第 3 步：只看 link
    铜口 PHY 看 link/speed/duplex；Switch CPU Port 看 fixed/in-band 状态。

第 4 步：只看 MAC 计数
    先不管协议，确认 RX/TX frame counter 增长。

第 5 步：只看二层
    抓 ARP、看 FDB、看 VLAN、看 CPU tag。

第 6 步：再看三四层
    ping、TCP echo、iperf、业务协议。
```

每一步都要有退出条件：

| 步骤 | 通过标准 |
| --- | --- |
| 读 ID | ID 稳定，不是 0xffff/0x0000 |
| SerDes | PLL lock、PCS 同步稳定 |
| Link | link up，speed/duplex 与预期一致 |
| MAC | RX/TX 计数增长，无持续 FCS/CRC 错误 |
| 二层 | ARP、FDB、VLAN 行为符合设计 |
| 协议 | ping、TCP/UDP、iperf 或业务测试通过 |

## 11. 一张总排障表

| 现象 | 证据 | 优先定位层 |
| --- | --- | --- |
| 扫不到 PHY ID | MDIO 读值全 0xffff 或 0x0000 | 电源 / reset / 地址 / MDIO |
| PCS 不 lock | PCS / SerDes 状态位不稳定 | 参考时钟 / 极性 / AC 耦合 / 模式 |
| PCS lock 但无外部 link | PHY 铜口状态为 down | 铜口 PHY / 网线 / 对端 |
| 外部 link up 但 MAC 无包 | MAC RX 计数不涨 | in-band / fixed-link / MAC 配置 |
| MAC 有包但协议不通 | 抓包有 ARP 无回复 | VLAN / FCS / IP 配置 |
| DSA user port 异常 | switch 统计、bridge vlan、FDB | CPU Port / tag / VLAN / trap |

真正排 SGMII 问题时，顺序应该是：

```text
硬件电气
  ->
PCS / SerDes
  ->
PHY / 铜口
  ->
MAC 计数
  ->
交换/VLAN
  ->
协议抓包
```

不要一上来就抓 TCP。

## 12. 本章总结

把 SGMII 讲透，关键不是记住多少缩写，而是把责任边界分清：

```text
SGMII：
    MAC 和 PHY / Switch CPU Port 之间的串行接口。

PCS / SerDes lock：
    只说明串行通道建立，不等于外部网线已通。

MDIO：
    仍然是读取 PHY ID、模式、link 状态的重要控制面。

in-band status：
    是 SGMII 上的状态传递方式，不是“只要写了 sgmii 就自动开启”。

fixed-link：
    适合固定速率的 CPU Port 或特定背板连接。
```

如果你后面继续看 `01_PHY芯片硬件与驱动详解.md`，重点会更偏 PHY 和 MDIO；如果继续看 `08_案例二_嵌入式Linux_Switch芯片网关项目.md`，重点会更偏 CPU Port、DSA、VLAN 和交换芯片配置。本章正好把这两条线在 `SGMII` 这个接口层连接起来。

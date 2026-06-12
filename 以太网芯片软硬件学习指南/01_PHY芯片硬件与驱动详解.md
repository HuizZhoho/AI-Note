# 01 PHY 芯片硬件与驱动详解

## 本章解决什么问题

PHY 是以太网板级 bring-up 最容易卡住的一层。很多软件看似没问题，最后发现是 PHY 地址、reset、参考时钟、RGMII delay 或 IO 电压不对。

PHY 的定位：

```text
RJ45 / 磁性器件 / 光模块
  |
PHY 芯片
  |
MII / RMII / RGMII / SGMII / USXGMII
  |
MAC 控制器
  |
DMA / CPU / 协议栈
```

通俗理解：PHY 像“翻译员”。网线侧是模拟/高速电气信号，MAC 侧是数字接口。PHY 负责链路训练、自动协商、编码解码、均衡、时钟恢复和状态上报。

## PHY 芯片硬件组成

典型铜口 PHY 包含：

```text
MDI 模拟前端：
    连接网络变压器和 RJ45，处理差分信号。

PMA/PMD：
    物理媒介相关逻辑，负责收发模拟信号、均衡、时钟恢复。

PCS：
    编码、解码、扰码、链路状态机。

Auto-Negotiation：
    和对端交换能力，决定 10/100/1000M、全双工/半双工。

MAC 接口：
    MII、RMII、RGMII、SGMII 等。

管理接口：
    MDIO/MDC，少数器件还带 I2C、SPI 或厂商私有调试口。

LED/Interrupt：
    输出 link、activity、speed LED，或向 CPU 报中断。
```

## 原理图检查清单

做 PHY bring-up，先从原理图确认这些项：

| 项目 | 要确认什么 | 错误现象 |
| --- | --- | --- |
| 电源轨 | AVDD/DVDD/IOVDD 是否符合 datasheet | PHY ID 读不到、发热、link 不亮 |
| IO 电压 | MAC-PHY IO Bank 和 PHY IOVDD 是否一致 | RGMII/RMII 波形异常 |
| Reset | 复位极性、上拉/下拉、释放时序 | PHY 一直不工作 |
| 参考时钟 | 25MHz、50MHz、125MHz 或晶振模式 | link 不 up、MDIO 读写异常 |
| Strap | PHY 地址、接口模式、delay、LED 模式 | 驱动找错地址、接口模式不匹配 |
| MDIO/MDC | 上拉、电平、是否共享总线 | 读不到 PHY ID |
| MAC 接口 | RGMII/RMII/MII/SGMII 接线和 Vivado/驱动一致 | link up 但 ping 不通 |
| 磁性器件 | 中心抽头、电容、Bob Smith 端接 | link 抖动、误码高 |
| LED | LED 模式和限流电阻 | 指示灯误导排障 |

## 常见 MAC-PHY 接口

### RGMII

RGMII 常用于千兆铜口 PHY。

```text
TXD[3:0], TX_CTL, TXC：MAC -> PHY
RXD[3:0], RX_CTL, RXC：PHY -> MAC
MDC, MDIO：管理接口
```

关键点：

```text
1000M 时 TXC/RXC 为 125 MHz。
4bit 数据双沿传输，相当于 8bit/周期。
RGMII v2.0 通常需要约 1.5ns 到 2ns 的时钟/数据相位关系。
delay 可以由 PHY 内部、MAC/FPGA 或 PCB 走线提供，但只能按一种方案一致设计。
```

典型错误：

```text
PHY link up，但 ping 不通。
小包偶尔通，大包丢包。
RX FCS 错误持续增加。
100M 正常，1000M 异常。
```

### RMII

RMII 常见于 10/100M 设计。

```text
2bit 数据线。
50 MHz 参考时钟。
管脚少，成本低。
```

要确认 50 MHz 参考时钟由谁提供：MAC 输出、PHY 输出，还是外部晶振同时喂给两边。

### SGMII

SGMII 是串行 MAC-PHY 接口，常用于交换芯片、FPGA、高速 PHY 或背板场景。

```text
串行差分信号。
常见线速 1.25 Gbps，用于承载 10/100/1000M。
需要 SerDes、CDR、可能还要参考时钟和均衡配置。
```

SGMII 比 RGMII 少线，但调试重点从并行时序变成 SerDes lock、极性、参考时钟和自协商模式。

## MDIO/MDC 管理接口

MDIO 是 PHY 的“控制台”。

```text
MDC：管理时钟，由 MAC/CPU 输出。
MDIO：双向数据线，通常需要上拉。
PHY Address：5bit 地址，通常由 strap 电阻决定。
```

软件通过 MDIO 读取：

```text
PHY ID。
Basic Mode Status Register，判断 link 和 auto-negotiation 完成。
Basic Mode Control Register，复位、启动协商、指定速率。
厂商扩展寄存器，配置 RGMII delay、LED、EEE、低功耗等。
```

通用伪代码：

```c
#define PHY_BMCR     0x00
#define PHY_BMSR     0x01
#define PHY_ID1      0x02
#define PHY_ID2      0x03

static int phy_read_id(int phy_addr)
{
    uint16_t id1 = mdio_read(phy_addr, PHY_ID1);
    uint16_t id2 = mdio_read(phy_addr, PHY_ID2);

    if (id1 == 0xffff || id1 == 0x0000) {
        return -1;
    }

    printf("PHY ID: %04x:%04x\n", id1, id2);
    return 0;
}

static int phy_wait_link(int phy_addr)
{
    for (int i = 0; i < 1000; i++) {
        uint16_t bmsr = mdio_read(phy_addr, PHY_BMSR);
        bmsr = mdio_read(phy_addr, PHY_BMSR);
        if (bmsr & (1u << 2)) {
            return 0;
        }
        delay_ms(10);
    }
    return -1;
}
```

`BMSR` 常常要读两次，是因为部分状态位是 latch-low，第一次读可能拿到历史状态。

## PHY 初始化流程

裸机或 bootloader 中常见流程：

```text
1. 释放 PHY reset。
2. 等待参考时钟稳定。
3. 扫描或指定 PHY 地址。
4. 读取 PHY ID。
5. 按板级设计配置 RGMII delay、LED、EEE 等厂商寄存器。
6. 启动 auto-negotiation。
7. 等待 link up。
8. 读取协商结果。
9. 按速率和双工配置 MAC。
10. 打开 MAC 收发。
```

对应 ZYNQ/lwIP 案例时，这些动作大多藏在 `xemac_add()` 和底层 `xemacpsif` 初始化里，但排障时必须知道它们实际发生过。

## Linux PHY 驱动视角

Linux 下 PHY 通常由 phylib 管理。

设备树描述重点：

```dts
ethernet@e000b000 {
    phy-mode = "rgmii-id";
    phy-handle = <&phy0>;
    mdio {
        phy0: ethernet-phy@1 {
            reg = <1>;
        };
    };
};
```

字段含义：

```text
phy-mode：
    MAC-PHY 接口模式，例如 rgmii、rgmii-id、sgmii。

reg：
    PHY 的 MDIO 地址。

phy-handle：
    把 MAC 和 PHY 绑定起来。
```

常用命令：

```bash
ethtool eth0
ethtool -S eth0
dmesg | grep -i phy
ip link set eth0 up
```

## Bring-up 最小验证

按这个顺序验证：

```text
1. 示波器看 PHY reset 是否释放。
2. 看参考时钟是否存在。
3. MDIO 读 PHY ID。
4. 插网线，看 link LED。
5. 读 link、speed、duplex。
6. MAC 计数器是否有 RX/TX。
7. 抓 ARP。
8. ping。
9. TCP/UDP 应用。
```

一句话记忆：

```text
PHY 问题先查电，再查时钟和 reset，再查 MDIO，最后才查协议栈。
```

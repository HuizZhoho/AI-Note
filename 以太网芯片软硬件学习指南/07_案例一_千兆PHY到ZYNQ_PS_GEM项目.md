# 07 案例一：千兆 PHY 到 ZYNQ PS GEM 项目

## 项目目标与硬件框图

目标：用 ZYNQ7010 PS GEM 通过外部千兆 PHY 接入以太网，跑通裸机 lwIP TCP Echo Server。

典型链路：

```text
PC
  |
RJ45 网线
  |
RJ45 + 网络变压器
  |
千兆铜口 PHY
  |
RGMII + MDIO/MDC
  |
ZYNQ7010 PS GEM
  |
GEM DMA
  |
DDR
  |
ARM Cortex-A9 + lwIP
  |
TCP Echo 应用
```

代表型号只作为学习锚点：常见千兆 PHY 如 RTL8211、KSZ9031、DP83867、YT8531 等都属于类似架构。实际寄存器、strap、delay 和电气参数必须以具体 datasheet 与原理图为准。

## 芯片与接口选择

ZYNQ7010 PS 内部有 GEM，也就是 MAC，但没有外部以太网 PHY。PHY 的作用是把 RJ45 侧的电气信号转换成 GEM 能处理的数字接口。

本案例选择：

```text
MAC：ZYNQ7010 PS GEM0 或 GEM1
PHY：外部千兆铜口 PHY
MAC-PHY 数据接口：RGMII
PHY 管理接口：MDIO/MDC
应用协议栈：lwIP RAW API
测试应用：TCP Echo Server，端口 7
```

ZYNQ7010 PS GEM 能做 10/100/1000M，不是 10GbE。10GbE 需要 FPGA GT、10G MAC/PCS/PMA 或外部 10G 方案。

## 原理图与 PCB 检查点

先看原理图，不要先看代码。

| 检查项 | 重点 |
| --- | --- |
| PHY 电源 | AVDD、DVDD、IOVDD 是否正确 |
| IO 电平 | PHY IOVDD 是否与 ZYNQ MIO/PL Bank 电压一致 |
| Reset | reset 极性、上拉/下拉、释放时序、是否由 GPIO 控制 |
| 参考时钟 | PHY 需要晶振、外部时钟，还是输出时钟给 MAC |
| PHY 地址 | strap 电阻决定 MDIO 地址，软件必须匹配 |
| RGMII delay | PHY 内部 delay、ZYNQ 侧 delay、PCB 走线只能整体一致 |
| MDIO/MDC | MDIO 是否上拉，MDC 频率是否满足 PHY 规格 |
| RJ45/磁性器件 | 中心抽头、ESD、LED、Bob Smith 端接 |

RGMII 最常见坑：

```text
100M 能通，1000M 不通。
PHY link up，但 ping 丢包。
FCS error 计数增加。
大包比小包更容易失败。
```

这类问题优先查 RGMII delay、IO 电压、时钟质量和约束。

## Vivado 硬件配置路径

PS GEM 走 MIO：

```text
Create Block Design
  |
Add ZYNQ7 Processing System
  |
Run Block Automation
  |
Enable Ethernet GEM0 或 GEM1
  |
Enable MDIO
  |
选择 MIO 管脚
  |
Enable UART 和 DDR
  |
Validate Design
  |
Generate Bitstream
  |
Export Hardware XSA
```

PS GEM 走 EMIO：

```text
Enable GEM
  |
选择 EMIO
  |
RGMII/MDIO 信号引到 PL
  |
XDC 约束 PL 管脚和 IOSTANDARD
  |
确认 PL Bank 电压与 PHY 一致
```

EMIO 只改变管脚出口，不改变应用层代码。GEM 仍是 PS 硬核 MAC。

## Vitis 与 BSP 路径

软件工程：

```text
导入 XSA
  |
创建 standalone Platform
  |
启用 lwIP
  |
创建 lwIP Echo Server Application
  |
检查 xparameters.h 中 GEM 基地址
  |
编译 ELF
  |
下载 bitstream 和 ELF
```

关键文件：

```text
xparameters.h：
    定义 GEM、UART、GPIO、中断等硬件基地址。

lwipopts.h：
    配置 lwIP 内存池、TCP/UDP、DHCP、校验等。

main.c：
    初始化平台、IP、lwIP、netif 和主循环。

echo.c：
    创建 TCP echo server，注册 callback。
```

## 底层初始化流程

软件主线：

```text
main()
  |
  |-- init_platform()
  |-- 设置 MAC/IP/netmask/gateway
  |-- lwip_init()
  |-- xemac_add()
  |-- netif_set_default()
  |-- netif_set_up()
  |-- start_application()
  |
  `-- while (1)
       |-- xemacif_input()
       `-- transfer_data()
```

`xemac_add()` 是核心桥梁：

```text
把 GEM 注册成 lwIP netif。
初始化 GEM 寄存器。
初始化 DMA descriptor。
通过 MDIO 查找和初始化 PHY。
读取 PHY link、speed、duplex。
配置 GEM 速率。
绑定底层发送函数。
```

PHY 初始化伪代码：

```c
int board_phy_init(void)
{
    phy_reset_assert();
    delay_ms(10);
    phy_reset_deassert();
    delay_ms(100);

    int addr = scan_mdio_for_phy();
    if (addr < 0) {
        return -1;
    }

    phy_config_rgmii_delay(addr);
    phy_restart_autoneg(addr);

    if (phy_wait_link_up(addr, 3000) != 0) {
        return -1;
    }

    struct link_state st = phy_read_link_state(addr);
    gem_set_speed(st.speed);
    gem_set_duplex(st.full_duplex);
    return 0;
}
```

真实 Xilinx 示例里这些动作通常被封装在底层适配层中。学习时要知道它们存在，排障时要能找到对应日志或寄存器。

## 收包与发包路径

RX：

```text
RJ45 收到电信号
  |
PHY 解码并通过 RGMII 输出
  |
GEM 接收以太网帧
  |
GEM DMA 写 DDR buffer
  |
xemacif_input() 轮询或处理中断
  |
cache invalidate
  |
构造 pbuf
  |
lwIP 解析 ARP/IP/ICMP/TCP
  |
recv_callback()
```

TX：

```text
recv_callback()
  |
tcp_write()
  |
lwIP 封装 TCP/IP/Ethernet
  |
netif->linkoutput()
  |
cache flush
  |
GEM DMA 从 DDR 取帧
  |
GEM 通过 RGMII 发给 PHY
  |
PHY 发送到 RJ45
```

## 应用验证流程

PC 侧配置：

```text
PC IP：192.168.1.100
ZYNQ IP：192.168.1.10
Mask：255.255.255.0
```

验证顺序：

```powershell
arp -d *
ping 192.168.1.10
telnet 192.168.1.10 7
```

或使用网络调试助手：

```text
TCP Client
目标 IP：192.168.1.10
目标端口：7
发送任意字符串
期望收到相同字符串
```

抓包应看到：

```text
ARP Request
ARP Reply
ICMP Echo Request
ICMP Echo Reply
TCP SYN
TCP SYN ACK
TCP ACK
TCP Data
TCP Echo Data
```

## 常见故障定位

| 现象 | 优先排查 |
| --- | --- |
| PHY ID 读不到 | MDIO/MDC、PHY 地址、reset、电源、参考时钟 |
| link 不 up | 网线、对端、PHY strap、自协商、时钟 |
| link up 但 ARP 不通 | GEM 是否开、RGMII 时序、MAC 地址、xemacif_input() |
| ARP 通但 ping 不通 | IP 配置、ICMP、防火墙、FCS error |
| ping 通但 TCP 不通 | start_application、tcp_bind、端口 7、防火墙 |
| TCP 数据异常 | cache flush/invalidate、pbuf 释放、tcp_recved、DMA buffer |

最重要的调试原则：

```text
先证明 PHY 和 GEM 能收发正确帧，再看 lwIP 和应用。
```

## 迁移到其他板卡

迁移 checklist：

```text
GEM0/GEM1 是否和原理图一致。
MIO/EMIO 是否和原理图一致。
PHY 地址是否一致。
phy-mode 是否正确：rgmii、rgmii-id、rgmii-rxid、rgmii-txid。
PHY reset GPIO 是否正确。
IOSTANDARD 和 Bank 电压是否匹配。
MAC 地址是否唯一。
IP 是否与 PC 同网段。
```

如果只改了 IP 地址就复制工程，很容易失败。ZYNQ 以太网迁移真正要改的是硬件描述、PHY 配置、管脚约束和底层初始化。

## 延伸阅读

- [ZYNQ7010 以太网案例 Flow 与代码详解](../ZYNQ7010_以太网案例/README.md)
- [ZYNQ7010 通信协议完整讲解](../ZYNQ7010_以太网案例/通信协议完整讲解.md)
- [ZYNQ PL/PS 以太网实现方案与代码级解析](../ZYNQ7010_以太网案例/ZYNQ_PL_PS以太网实现方案与代码级解析.md)

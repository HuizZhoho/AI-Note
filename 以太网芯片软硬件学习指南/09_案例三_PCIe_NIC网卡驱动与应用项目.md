# 09 案例三：PCIe NIC 网卡驱动与应用项目

## 项目目标与硬件框图

目标：理解一颗 PCIe 网卡芯片如何从主机内存收发网络包，以及 Linux 驱动、DMA、NAPI、ethtool 和应用测试如何串起来。

典型结构：

```text
应用程序
  |
socket API
  |
Linux TCP/IP 协议栈
  |
NIC Driver
  |
PCIe BAR / MMIO
  |
TX/RX Descriptor Ring
  |
NIC DMA Engine
  |
NIC MAC
  |
PHY / SerDes / RJ45 / SFP+
  |
网络
```

代表型号只作为学习锚点：Intel i210/i225、Realtek PCIe NIC、Broadcom/Marvell 网卡都可按类似模型学习。精确寄存器、descriptor 格式和 offload 位定义以具体 datasheet、驱动源码为准。

## NIC 项目和 PHY 项目的区别

PHY 项目重点是 MAC-PHY 链路；NIC 项目重点是主机和网卡芯片之间的高速数据搬运。

```text
PHY 项目：
    关注 MDIO、link、RGMII/SGMII、MAC 收发。

NIC 项目：
    关注 PCIe、BAR、DMA ring、interrupt、NAPI、offload。
```

NIC 不是只负责“网线电信号”。它同时包含主机总线接口、DMA、队列、MAC 和可能的 PHY/SerDes。

## 硬件检查点

| 模块 | 检查项 |
| --- | --- |
| PCIe 电源 | 3.3V、1.xV core、模拟电源、上电顺序 |
| PCIe 时钟 | 100MHz REFCLK、抖动、时钟方向 |
| PERST# | 复位释放时序、是否受主机控制 |
| PCIe lane | lane 宽度、极性、AC 耦合、电容位置 |
| EEPROM/Flash | MAC 地址、启动配置、子系统 ID |
| PHY/SerDes | RJ45 或 SFP+、参考时钟、模块控制脚 |
| 中断 | MSI/MSI-X 支持，不是传统外部中断线 |
| 散热 | 10G NIC 需要关注功耗和温升 |

PCIe NIC bring-up 第一关不是 ping，而是主机能否枚举到设备。

## PCIe 枚举与 BAR

Linux 检查：

```bash
lspci -nn | grep -i ethernet
lspci -vv -s <bus:dev.fn>
dmesg | grep -i -E "eth|nic|pcie|msi"
```

枚举成功说明：

```text
PCIe REFCLK 基本正常。
PERST# 已释放。
lane 训练成功。
主机读到了 vendor/device ID。
```

BAR 的作用：

```text
CPU 把 NIC 寄存器映射到内存地址空间。
驱动通过 readl()/writel() 配置 NIC。
```

驱动伪代码：

```c
static int nic_probe(struct pci_dev *pdev,
                     const struct pci_device_id *id)
{
    pci_enable_device_mem(pdev);
    pci_request_regions(pdev, "demo_nic");
    pci_set_master(pdev);

    void __iomem *bar0 = pci_iomap(pdev, 0, 0);
    u32 chip_id = readl(bar0 + REG_CHIP_ID);

    nic_reset(bar0);
    nic_alloc_rings();
    nic_init_interrupts(pdev);
    nic_register_netdev();
    return 0;
}
```

## DMA Descriptor Ring

NIC 收发包的核心是 ring。

RX ring：

```text
驱动分配一批 RX buffer。
驱动把 buffer DMA 地址写入 RX descriptor。
NIC 收到包后 DMA 写入 buffer。
NIC 设置 done 位和实际长度。
驱动 poll descriptor，构造 skb 交给协议栈。
驱动重新补充 RX buffer。
```

TX ring：

```text
协议栈给驱动 skb。
驱动把 skb 映射成 DMA 地址。
驱动填写 TX descriptor。
驱动写 doorbell 通知 NIC。
NIC DMA 读取 buffer 并发送。
NIC 回写完成状态。
驱动释放 skb。
```

Doorbell 是“通知硬件 ring 里有新 descriptor”的 MMIO 寄存器。

## RX 驱动调用链

```text
NIC 收到包
  |
DMA 写 RX buffer
  |
写回 descriptor done
  |
触发 MSI-X
  |
驱动 ISR
  |
关闭该队列中断
  |
napi_schedule()
  |
poll()
  |
读取 RX descriptor
  |
构造 skb
  |
napi_gro_receive()
  |
Linux TCP/IP 协议栈
  |
socket 应用
```

伪代码：

```c
static int demo_nic_poll(struct napi_struct *napi, int budget)
{
    int done = 0;

    while (done < budget) {
        struct rx_desc *desc = rx_next_done();
        if (!desc) {
            break;
        }

        dma_sync_single_for_cpu(dev, desc->dma, desc->len,
                                DMA_FROM_DEVICE);

        struct sk_buff *skb = build_skb(desc->buf, desc->len);
        skb->protocol = eth_type_trans(skb, netdev);
        napi_gro_receive(napi, skb);

        rx_refill(desc);
        done++;
    }

    if (done < budget) {
        napi_complete_done(napi, done);
        nic_enable_rx_irq();
    }

    return done;
}
```

## TX 驱动调用链

```text
应用 send()
  |
Linux TCP/IP 封装 skb
  |
ndo_start_xmit()
  |
dma_map_single()
  |
填写 TX descriptor
  |
writel(TX_DOORBELL)
  |
NIC DMA 读取主机内存
  |
MAC/PHY 发出
  |
TX complete
  |
释放 skb 和 DMA 映射
```

伪代码：

```c
static netdev_tx_t demo_start_xmit(struct sk_buff *skb,
                                   struct net_device *netdev)
{
    struct tx_desc *desc = tx_alloc_desc();
    if (!desc) {
        netif_stop_queue(netdev);
        return NETDEV_TX_BUSY;
    }

    dma_addr_t dma = dma_map_single(dev, skb->data, skb->len,
                                    DMA_TO_DEVICE);

    desc->addr = dma;
    desc->len = skb->len;
    desc->flags = TX_FIRST | TX_LAST | TX_CSUM_OFFLOAD;

    tx_push(desc);
    writel(tx_tail, nic_bar + REG_TX_DOORBELL);
    return NETDEV_TX_OK;
}
```

## Offload 与抓包误区

常见 offload：

```text
TX checksum offload：
    协议栈不提前算 checksum，由 NIC 发送时计算。

RX checksum offload：
    NIC 校验 checksum，把结果写进 descriptor。

TSO/GSO：
    大 TCP 数据延后分段，降低 CPU 开销。

RSS：
    多队列按 flow hash 分流到多个 CPU core。

VLAN offload：
    NIC 硬件插入或剥离 VLAN tag。
```

抓包误区：

```text
在本机抓发送包看到 checksum bad，不一定是真的错。
可能是 TX checksum offload 尚未由硬件填写。
```

验证命令：

```bash
ethtool -k eth0
ethtool -K eth0 tx off rx off
tcpdump -i eth0 -nn -e
```

## 应用测试

基础：

```bash
ip link set eth0 up
ip addr add 192.168.1.100/24 dev eth0
ping 192.168.1.10
```

吞吐：

```bash
iperf3 -s
iperf3 -c 192.168.1.10 -P 4
```

统计：

```bash
ethtool eth0
ethtool -S eth0
ethtool -g eth0
cat /proc/interrupts | grep eth0
```

重点看：

```text
rx_packets / tx_packets。
rx_crc_errors。
rx_missed_errors。
rx_no_buffer_count。
tx_timeout。
interrupt 分布。
队列是否被 stop。
```

## 常见故障定位

| 现象 | 排查 |
| --- | --- |
| lspci 看不到 | PCIe REFCLK、PERST#、lane、供电、BIOS/设备树 |
| lspci 有但无 ethX | 驱动匹配、vendor/device ID、firmware、内核日志 |
| ethX 有但 link down | PHY/SFP、网线、速率协商、模块兼容 |
| link up 但 ping 不通 | IP、ARP、交换机 VLAN、RX/TX 计数 |
| 吞吐低 | ring size、RSS、多队列、offload、CPU 亲和性 |
| 丢包 | RX buffer 不足、中断合并、队列溢出、FCS error |
| TX timeout | DMA 卡住、descriptor owner 位、doorbell、PCIe 错误 |

## 迁移到其他 NIC

迁移时重点看：

```text
PCIe vendor/device ID。
BAR 寄存器布局。
descriptor 格式。
DMA 地址宽度。
中断模式：INTx、MSI、MSI-X。
队列数量。
offload 能力。
ethtool 统计项含义。
```

NIC 学习的底层闭环：

```text
PCIe 枚举 -> BAR 寄存器 -> DMA ring -> 中断/NAPI -> 协议栈 -> 应用吞吐。
```

## 延伸阅读

- [TCP/IP 协议详解](../TCP_IP协议详解/README.md)
- [抓包与排查速查](../TCP_IP协议详解/抓包与排查速查.md)
- [项目排障实战清单](./12_项目排障实战清单.md)

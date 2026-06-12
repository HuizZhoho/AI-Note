# 03 NIC 网卡芯片架构与驱动

## 本章解决什么问题

NIC 是主机侧以太网控制器。PC、服务器或嵌入式 Linux 设备通过网卡芯片接入网络。理解 NIC，要把三件事连起来：

```text
软件协议栈：socket、TCP/IP、网络驱动。
主机总线：PCIe 或 SoC 内部总线。
硬件数据面：DMA ring、descriptor、MAC、PHY/SerDes。
```

通俗理解：NIC 是 CPU 和网络之间的高速搬运设备。CPU 不会亲自把每个字节从内存搬到网线，驱动把 buffer 描述给 NIC，NIC 用 DMA 自己搬。

## NIC 芯片内部结构

典型 PCIe 网卡：

```text
PCIe Endpoint
  |
BAR Registers
  |
DMA Engine
  |
TX/RX Descriptor Rings
  |
Packet Buffer
  |
Checksum / RSS / VLAN / Offload Engines
  |
MAC
  |
PHY / SerDes / SFP+
```

关键模块：

```text
PCIe：
    主机和网卡之间的高速总线。

BAR：
    CPU 通过 MMIO 访问网卡寄存器。

DMA：
    网卡直接读写主机内存。

Descriptor Ring：
    驱动和网卡共享的一圈描述符，描述 buffer 地址、长度、状态。

Interrupt/MSI-X：
    网卡通知 CPU 有包到达或发送完成。

Offload：
    checksum、TSO、LRO、RSS、VLAN tag 等硬件加速。
```

## RX 接收路径

从网线到应用：

```text
1. PHY/SerDes 收到比特流。
2. MAC 校验以太网帧和 FCS。
3. NIC 根据过滤规则决定是否接收。
4. DMA 把数据写入 RX buffer。
5. NIC 更新 RX descriptor 状态。
6. NIC 触发中断或等待轮询。
7. 驱动读取 descriptor。
8. 驱动构造 skb 或 pbuf。
9. 内核 TCP/IP 协议栈处理。
10. 应用 socket 读到数据。
```

Linux 驱动常见逻辑：

```c
static int nic_poll_rx(struct napi_struct *napi, int budget)
{
    int work = 0;

    while (work < budget) {
        struct rx_desc *desc = rx_ring_next_done();
        struct sk_buff *skb;

        if (!desc) {
            break;
        }

        dma_sync_single_for_cpu(dev, desc->dma, desc->len, DMA_FROM_DEVICE);

        skb = build_skb(desc->buffer, desc->len);
        skb->protocol = eth_type_trans(skb, netdev);
        napi_gro_receive(napi, skb);

        rx_refill_descriptor(desc);
        work++;
    }

    if (work < budget) {
        napi_complete_done(napi, work);
        nic_enable_rx_irq();
    }

    return work;
}
```

这段伪代码要抓住三点：

```text
descriptor 告诉驱动哪个 buffer 收到了包。
DMA 后 CPU 读 buffer 前要处理 cache/一致性。
NAPI 用轮询批量处理包，避免高包速下中断过多。
```

## TX 发送路径

从应用到网线：

```text
1. 应用调用 send/write。
2. 内核 TCP/IP 协议栈生成 skb。
3. 驱动把 skb 映射成 DMA 地址。
4. 驱动填写 TX descriptor。
5. 驱动 doorbell 通知 NIC 有新包。
6. NIC DMA 读取内存数据。
7. NIC MAC 添加/校验必要字段并发送。
8. PHY/SerDes 发到网络。
9. NIC 写回发送完成状态。
10. 驱动释放 skb 和 DMA 映射。
```

发送伪代码：

```c
static netdev_tx_t nic_start_xmit(struct sk_buff *skb,
                                  struct net_device *netdev)
{
    struct tx_desc *desc = tx_ring_alloc();
    dma_addr_t dma;

    if (!desc) {
        netif_stop_queue(netdev);
        return NETDEV_TX_BUSY;
    }

    dma = dma_map_single(dev, skb->data, skb->len, DMA_TO_DEVICE);

    desc->addr = dma;
    desc->len = skb->len;
    desc->flags = TX_DESC_FIRST | TX_DESC_LAST | TX_DESC_CSUM;

    tx_ring_push(desc);
    writel(tx_ring_tail, nic->bar + TX_DOORBELL);

    return NETDEV_TX_OK;
}
```

Doorbell 是关键概念：驱动写一个寄存器告诉 NIC，TX ring 有新的 descriptor 可以处理。

## Descriptor Ring

Descriptor 是网卡驱动的核心。

RX descriptor 常包含：

```text
buffer DMA 地址。
buffer 长度。
包实际长度。
checksum 状态。
VLAN tag。
错误标志。
owner 位或 done 位。
```

TX descriptor 常包含：

```text
待发送 buffer DMA 地址。
长度。
是否一个包的开始/结束。
是否需要 checksum offload。
是否需要 VLAN 插入。
发送完成回写状态。
```

ring 的本质：

```text
驱动生产 descriptor。
硬件消费 descriptor。
硬件生产完成状态。
驱动消费完成状态。
```

## 中断、NAPI 和性能

低速时，每个包触发中断还能接受；高速时，中断会拖垮 CPU。

Linux NAPI 思路：

```text
第一次包到达触发中断。
驱动关闭该队列中断。
内核轮询批量处理多个包。
处理完再打开中断。
```

性能优化方向：

```text
增大 RX/TX ring。
使用多队列和 RSS。
启用 checksum offload。
合理设置 interrupt coalescing。
使用 jumbo frame 降低包率。
避免小包高 PPS 超出 CPU 能力。
```

## Offload 功能

常见硬件加速：

| 功能 | 作用 | 注意 |
| --- | --- | --- |
| RX checksum offload | 网卡校验 IP/TCP/UDP checksum | 抓包显示 checksum bad 可能是假象 |
| TX checksum offload | 协议栈留空 checksum，网卡发送时计算 | 抓本机发送包可能看到未完成 checksum |
| TSO/GSO | 大 TCP 数据由网卡分段 | 降低 CPU 开销 |
| RSS | 按 hash 分流到多队列 | 多核并行处理 |
| VLAN offload | 硬件插入/剥离 VLAN tag | 抓包时要注意 tag 是否被驱动隐藏 |

## 常用命令

Windows：

```powershell
ipconfig /all
Get-NetAdapter
Get-NetAdapterAdvancedProperty
Get-NetAdapterStatistics
```

Linux：

```bash
ip link show
ethtool eth0
ethtool -k eth0
ethtool -S eth0
ethtool -g eth0
lspci -nn | grep -i ethernet
lspci -vv -s <bus:dev.fn>
```

抓包：

```bash
tcpdump -i eth0 -nn -e
tcpdump -i eth0 -nn -e vlan
```

## NIC 常见问题

```text
PCIe 枚举不到：
    查 PERST#、REFCLK、lane 极性、供电、设备树或 BIOS。

link 不 up：
    查 PHY/SFP、网线、速率自协商、模块兼容性。

能 ping 但吞吐低：
    查 ring size、offload、MTU、CPU 负载、中断亲和性。

丢包：
    查 rx_missed、rx_no_buffer、FCS error、pause frame、队列溢出。

抓包 checksum bad：
    先确认是否 TX checksum offload 导致的抓包假象。

VLAN 不通：
    查 VLAN offload、交换机 trunk/access、PCP/VID、驱动是否剥离 tag。
```

## NIC 学习重点

```text
先理解 PCIe/BAR/MMIO。
再理解 DMA descriptor ring。
再理解 RX/TX 收发路径。
再理解中断、NAPI、多队列。
最后学习 offload 和性能调优。
```

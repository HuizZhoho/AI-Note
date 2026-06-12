# 04 Firmware 控制与代码级调用链

## 本章解决什么问题

10GbE 的高速数据面由硬件完成，但系统能否稳定运行，很大程度取决于 Firmware 控制面。

Firmware 负责：

```text
初始化 clock/reset
配置 GT/PCS/MAC/DMA
检测 link 状态
管理 DMA descriptor
分配和回收 buffer
处理中断
维护统计计数
处理错误恢复
```

## 初始化顺序

推荐顺序：

```text
1. 初始化板级电源、GPIO、I2C、时钟芯片。
2. 检查 SFP+ 模块是否存在。
3. 读取 SFP+ LOS/TX_FAULT 状态。
4. 配置 GT reference clock。
5. 释放 GT reset，等待 PLL lock。
6. 等待 TX/RX reset done。
7. 释放 PCS/PMA reset，等待 block lock。
8. 配置 MAC 地址、帧长、FCS、PAUSE、RX/TX enable。
9. 初始化 DMA descriptor ring。
10. 投递 RX buffer。
11. 开启中断。
12. 确认 link up。
13. 允许应用发送数据。
```

这条顺序很重要。不要在 GT/PCS 没稳定时就启动 MAC 和 DMA。

## 寄存器抽象

通用寄存器模型：

```text
0x0000 control
0x0004 status
0x0008 reset_control
0x000C interrupt_status
0x0010 interrupt_enable
0x0020 local_mac_low
0x0024 local_mac_high
0x0030 max_frame_length
0x0040 pcs_status
0x0044 gt_status
0x0050 rx_packet_count
0x0054 tx_packet_count
0x0058 rx_error_count
0x005C tx_error_count
```

Firmware 访问：

```c
#define ETH10G_BASE 0xA0000000U

static inline void reg_write(u32 offset, u32 value)
{
    Xil_Out32(ETH10G_BASE + offset, value);
}

static inline u32 reg_read(u32 offset)
{
    return Xil_In32(ETH10G_BASE + offset);
}
```

## 初始化伪代码

```c
int eth10g_init(void)
{
    board_clock_init();
    sfp_i2c_init();

    if (!sfp_present()) {
        return -ERR_NO_MODULE;
    }

    if (sfp_rx_los()) {
        return -ERR_NO_SIGNAL;
    }

    reg_write(REG_RESET_CONTROL, RESET_ALL_ASSERT);
    delay_ms(10);

    reg_write(REG_RESET_CONTROL, RESET_GT_RELEASE);
    if (wait_bit(REG_GT_STATUS, GT_PLL_LOCK, 100) != 0) {
        return -ERR_GT_PLL;
    }

    if (wait_bit(REG_GT_STATUS, GT_RX_RESET_DONE, 100) != 0) {
        return -ERR_GT_RESET;
    }

    reg_write(REG_RESET_CONTROL, RESET_PCS_RELEASE);
    if (wait_bit(REG_PCS_STATUS, PCS_BLOCK_LOCK, 100) != 0) {
        return -ERR_BLOCK_LOCK;
    }

    mac_configure(local_mac, 9000);
    dma_init_rings();
    rx_submit_all_buffers();

    reg_write(REG_INTERRUPT_ENABLE, INT_RX | INT_TX | INT_ERROR);
    reg_write(REG_CONTROL, CTRL_RX_EN | CTRL_TX_EN);

    return 0;
}
```

## Link 检测

10GbE link up 不能只看一个信号。建议综合判断：

```text
SFP+ present
RX_LOS = 0
GT PLL lock
RX reset done
PCS block lock
local fault = 0
remote fault = 0
MAC RX enabled
```

伪代码：

```c
bool eth10g_link_up(void)
{
    u32 gt = reg_read(REG_GT_STATUS);
    u32 pcs = reg_read(REG_PCS_STATUS);

    return sfp_present() &&
           !sfp_rx_los() &&
           (gt & GT_PLL_LOCK) &&
           (gt & GT_RX_RESET_DONE) &&
           (pcs & PCS_BLOCK_LOCK) &&
           !(pcs & PCS_LOCAL_FAULT) &&
           !(pcs & PCS_REMOTE_FAULT);
}
```

## DMA descriptor ring

Descriptor ring 是 Firmware 和 DMA 协作的核心。

RX descriptor：

```text
buffer address
buffer capacity
actual length
status
owner
```

TX descriptor：

```text
buffer address
frame length
SOP/EOP
status
owner
```

owner 字段表示：

```text
CPU owned：
    Firmware 可以修改。

DMA owned：
    DMA 正在使用，Firmware 不应修改。
```

## RX 中断处理

```c
void eth10g_rx_isr(void)
{
    while (rx_desc_completed()) {
        struct dma_desc *d = rx_get_done_desc();

        cache_invalidate(d->addr, d->actual_len);

        if (d->status & DESC_ERR) {
            stats.rx_desc_error++;
        } else {
            eth10g_process_rx_frame(d->addr, d->actual_len);
        }

        d->actual_len = 0;
        d->status = 0;
        d->owner = DMA_OWNED;
        rx_refill_desc(d);
    }
}
```

注意：

```text
invalidate 必须在 CPU 读取 DMA 写入的数据之前。
处理完必须重新投递 RX buffer。
如果 RX buffer 不够，硬件会丢包。
```

## TX 发送调用链

```c
int eth10g_send_frame(void *frame, u32 len)
{
    struct dma_desc *d;

    if (!eth10g_link_up()) {
        return -ERR_LINK_DOWN;
    }

    d = tx_alloc_desc();
    if (d == NULL) {
        return -ERR_NO_TX_DESC;
    }

    cache_flush(frame, len);

    d->addr = frame;
    d->len = len;
    d->flags = DESC_SOP | DESC_EOP;
    d->owner = DMA_OWNED;

    dma_kick_tx();

    return 0;
}
```

TX 完成中断：

```c
void eth10g_tx_isr(void)
{
    while (tx_desc_completed()) {
        struct dma_desc *d = tx_get_done_desc();

        if (d->status & DESC_ERR) {
            stats.tx_desc_error++;
        }

        free_tx_buffer(d->addr);
        tx_release_desc(d);
    }
}
```

## cache 规则

RX：

```text
DMA 写 DDR
  |
CPU 读之前 invalidate
```

TX：

```text
CPU 写 DDR
  |
DMA 读之前 flush
```

错误表现：

```text
RX 看到旧数据
TX 发出旧包
包头正确但 payload 异常
偶发错误，难复现
```

## 统计计数

Firmware 应维护：

```text
rx_packets
tx_packets
rx_bytes
tx_bytes
rx_fcs_errors
rx_length_errors
rx_drops
tx_drops
dma_errors
block_lock_loss_count
link_down_count
```

调试时，计数器比肉眼观察更可靠。

## 错误恢复

错误恢复要分级：

```text
轻微丢包：
    只记录计数。

DMA ring 卡死：
    重置 DMA，保留 MAC/PCS。

PCS block lock 丢失：
    重置 PCS/GT RX。

GT PLL unlock：
    重置 GT，检查参考时钟。

SFP+ 拔出：
    停止 TX，释放资源，等待重新插入。
```

不要遇到所有错误都全系统 reset。分级恢复能减少业务中断。

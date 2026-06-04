/*
 * ZYNQ7010 lwIP Echo Server 主流程注释版
 *
 * 说明：
 * 1. 该文件用于学习整个以太网案例的调用顺序。
 * 2. 实际 Vitis 工程中的头文件路径、宏名、定时器处理方式可能因版本不同略有差异。
 * 3. 重点不是逐行照抄，而是理解 main() 如何把平台、网卡、lwIP 和应用串起来。
 */

#include "platform.h"
#include "xil_printf.h"

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"

#include "netif/xadapter.h"

/* start_application() 和 transfer_data() 通常由 echo.c 提供。 */
void start_application(void);
void transfer_data(void);

/*
 * 某些 Xilinx lwIP 裸机版本会使用 TCP 定时器标志。
 * 如果你的 BSP 使用 sys_check_timeouts()，则不需要这两个变量。
 */
extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;

/*
 * 全局 netif 表示一块网卡。
 * 对本案例来说，它就代表 ZYNQ7010 PS GEM 这一个以太网接口。
 */
static struct netif server_netif;

int main(void)
{
    struct netif *netif = &server_netif;

    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gw;

    /*
     * MAC 地址必须在同一局域网内唯一。
     * Xilinx 示例常用 00:0A:35 开头作为演示地址。
     */
    unsigned char mac_ethernet_address[] = {
        0x00, 0x0A, 0x35, 0x00, 0x01, 0x02
    };

    /*
     * 1. 初始化平台环境。
     *
     * 常见动作：
     * - 初始化串口，保证 xil_printf() 可以输出调试信息
     * - 打开 I-cache / D-cache
     * - 初始化定时器或中断控制器
     */
    init_platform();

    /*
     * 2. 配置静态 IP。
     *
     * PC 端需要和 ZYNQ 在同一网段，例如：
     * ZYNQ: 192.168.1.10
     * PC:   192.168.1.100
     */
    IP4_ADDR(&ipaddr,  192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    /*
     * 3. 初始化 lwIP 协议栈。
     *
     * 这里会初始化 ARP、IP、ICMP、TCP、UDP、pbuf、内存池等。
     * 但注意：此时还没有把 ZYNQ GEM 网卡注册进去。
     */
    lwip_init();

    /*
     * 4. 把 ZYNQ PS GEM 网卡添加到 lwIP。
     *
     * xemac_add() 是硬件和协议栈连接的关键函数。
     * 它会完成：
     * - 设置 IP / mask / gateway
     * - 设置 MAC 地址
     * - 初始化 GEM 控制器
     * - 初始化 PHY
     * - 初始化 DMA buffer
     * - 设置 lwIP 底层发送函数
     */
    if (!xemac_add(netif,
                   &ipaddr,
                   &netmask,
                   &gw,
                   mac_ethernet_address,
                   PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding network interface\r\n");
        cleanup_platform();
        return -1;
    }

    /*
     * 5. 设置默认网卡。
     *
     * 系统只有一块网卡时，默认所有网络流量都从这个 netif 发送。
     */
    netif_set_default(netif);

    /*
     * 6. 启动网卡的软件接口。
     *
     * netif up 表示 lwIP 允许这个接口工作。
     * 它不等于 PHY link up；PHY link up 还要看网线和自协商状态。
     */
    netif_set_up(netif);

    /*
     * 7. 启动应用层。
     *
     * Echo Server 中，这一步会创建 TCP 服务端：
     * tcp_new -> tcp_bind -> tcp_listen -> tcp_accept
     */
    start_application();

    /*
     * 8. 裸机主循环。
     *
     * 没有操作系统时，网络协议栈不会自动运行。
     * 必须在 while(1) 中持续处理收包、TCP 定时器和应用任务。
     */
    while (1) {
        /*
         * 处理 TCP 快定时器。
         * 该逻辑用于部分 Xilinx lwIP 裸机版本。
         */
        if (TcpFastTmrFlag) {
            tcp_fasttmr();
            TcpFastTmrFlag = 0;
        }

        /*
         * 处理 TCP 慢定时器。
         */
        if (TcpSlowTmrFlag) {
            tcp_slowtmr();
            TcpSlowTmrFlag = 0;
        }

        /*
         * 从 GEM/DMA 接收数据包，并交给 lwIP 解析。
         *
         * 如果没有持续调用它，ZYNQ 即使物理上收到包，
         * lwIP 也不会处理 ARP、ping 或 TCP 数据。
         */
        xemacif_input(netif);

        /*
         * 应用层周期任务。
         * Echo Server 中主要数据处理在 recv_callback()，所以这里可能为空。
         */
        transfer_data();
    }

    /*
     * 裸机网络程序通常不会运行到这里。
     */
    cleanup_platform();
    return 0;
}


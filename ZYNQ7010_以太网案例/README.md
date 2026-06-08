# ZYNQ7010 以太网案例 Flow 与代码详解

本文以常见的 ZYNQ7010 裸机 lwIP Echo Server 案例为主线，完整说明从 Vivado 硬件配置、Vitis BSP 生成、lwIP 初始化、GEM/PHY 驱动，到 TCP echo 应用运行的整体过程。

## 推荐阅读顺序

```text
1. 先读本文，理解整体工程 flow 和代码主线
2. 再读 通信协议完整讲解.md，理解协议、电气特性和完整通信交互过程
3. 再读 code/main_annotated.c，理解 main() 如何串起平台、lwIP、网卡和应用
4. 再读 code/echo_annotated.c，理解 TCP Echo Server 的回调逻辑
5. 最后按调试检查表验证 PHY、ARP、ping、TCP echo
```

相关文档：

```text
通信协议完整讲解.md
code/main_annotated.c
code/echo_annotated.c
```

## 1. 整体框架

ZYNQ7010 以太网案例可以分成五层：

```text
PC / 上位机
  |
RJ45 / 网线
  |
PHY 芯片
  |
RGMII / RMII / MII + MDIO
  |
ZYNQ7010 PS GEM
  |
DMA / DDR
  |
Xilinx GEM 驱动与 lwIP 适配层
  |
lwIP 协议栈
  |
用户应用：ping / TCP echo / UDP / WebServer
```

核心关系：

```text
PHY 负责物理链路
GEM 负责 MAC 收发
DMA 负责搬运数据
DDR 存放网络缓冲区
lwIP 负责 TCP/IP 协议
应用代码负责具体业务逻辑
```

## 2. 必要概念

### 2.1 MAC

MAC 是以太网媒体访问控制层。ZYNQ7010 PS 内部集成了 Gigabit Ethernet MAC，Xilinx 通常称为 GEM。

GEM 主要负责：

```text
以太网帧收发
MAC 地址过滤
发送和接收 FIFO
DMA 描述符管理
与 PHY 进行数据交互
```

### 2.2 PHY

ZYNQ7010 内部没有 PHY，因此板子上通常会外接 PHY 芯片。

PHY 负责：

```text
网线电气信号转换
链路检测
10M / 100M / 1000M 自协商
全双工 / 半双工协商
与 RJ45 和网络变压器连接
```

### 2.3 MDIO / MDC

MDIO 是 MAC 管理 PHY 的接口。

```text
MDC  管理时钟，由 MAC 输出
MDIO 管理数据线，双向
```

软件通过 MDIO 可以：

```text
读取 PHY ID
读取 link 状态
读取协商速率
配置 PHY 复位
配置自动协商
```

### 2.4 RGMII / RMII / MII

这是 MAC 和 PHY 之间的数据接口。

```text
RGMII 常用于千兆以太网
RMII 常用于百兆以太网
MII  是较早的并行接口
```

具体选择取决于板级原理图。

### 2.5 lwIP

lwIP 是轻量级 TCP/IP 协议栈，裸机 ZYNQ 案例中经常使用 RAW API。

lwIP 负责：

```text
ARP
IP
ICMP
TCP
UDP
DHCP
网络缓冲区管理
网络接口抽象
```

### 2.6 netif

`struct netif` 是 lwIP 中的网络接口对象。

它包含：

```text
IP 地址
子网掩码
网关
MAC 地址
底层发送函数
协议栈输入函数
链路状态
接口状态
```

### 2.7 pbuf

`pbuf` 是 lwIP 的网络包缓冲结构。

接收到的数据包会被包装成 `pbuf`，然后交给 lwIP 协议栈解析。

### 2.8 TCP PCB

PCB 是 Protocol Control Block。

TCP 服务端会使用 PCB 保存：

```text
监听端口
连接状态
接收回调
发送缓冲
远端 IP 和端口
TCP 窗口信息
```

## 3. 工程生成 Flow

完整工程流程：

```text
1. Vivado 创建工程
2. 添加 ZYNQ7 Processing System
3. 配置 DDR
4. 配置 UART
5. 配置 Ethernet GEM0 或 GEM1
6. 配置 MDIO
7. 配置 MIO 或 EMIO
8. Validate Design
9. Generate Output Products
10. Generate Bitstream
11. Export Hardware，生成 .xsa
12. Vitis 导入 .xsa
13. 创建 Platform Project
14. 生成 BSP
15. 创建 lwIP Echo Server Application
16. 编译生成 .elf
17. 下载 bitstream
18. 下载 elf
19. 串口观察输出
20. PC 配置同网段 IP
21. ping 或 TCP echo 测试
```

Vivado 负责描述硬件，Vitis 根据 XSA 生成软件工程和 BSP。

## 4. Vivado 硬件配置框架

典型 Block Design：

```text
+----------------------------+
| ZYNQ7 Processing System    |
|                            |
| DDR                        |
| UART                       |
| GEM0 Ethernet              |
| MDIO                       |
| MIO / EMIO                 |
+----------------------------+
```

关键配置项：

```text
DDR       程序运行和 DMA buffer 使用
UART      串口打印调试信息
GEM0/1    PS 内部以太网 MAC
MDIO      读取和配置 PHY
MIO       PS 直接引脚连接 PHY
EMIO      通过 PL 引脚连接 PHY
Clock     CPU、GEM、总线和外设时钟
```

常见硬件连接：

```text
PS GEM0 -> MIO -> PHY -> RJ45
```

如果板子使用 PL 引脚连接 PHY，则是：

```text
PS GEM0 -> EMIO -> PL IO -> PHY -> RJ45
```

EMIO 方式需要额外 XDC 管脚约束。

## 5. Vitis BSP 框架

XSA 导入 Vitis 后，BSP 会根据硬件自动生成驱动和参数。

常见文件：

```text
xparameters.h
xil_printf.h
xil_cache.h
xscugic.h
xemacps.h
lwipopts.h
```

`xparameters.h` 中会出现类似定义：

```c
#define XPAR_XEMACPS_0_BASEADDR 0xE000B000
#define XPAR_XUARTPS_0_BASEADDR 0xE0000000
```

软件通过这些宏找到对应硬件外设。

## 6. 软件主流程

典型裸机 lwIP 工程主流程：

```c
int main(void)
{
    struct netif server_netif;
    struct netif *netif;
    ip_addr_t ipaddr, netmask, gw;

    unsigned char mac_ethernet_address[] = {
        0x00, 0x0A, 0x35, 0x00, 0x01, 0x02
    };

    netif = &server_netif;

    init_platform();

    IP4_ADDR(&ipaddr,  192, 168, 1, 10);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 1, 1);

    lwip_init();

    if (!xemac_add(netif, &ipaddr, &netmask, &gw,
                   mac_ethernet_address,
                   PLATFORM_EMAC_BASEADDR)) {
        xil_printf("Error adding network interface\r\n");
        return -1;
    }

    netif_set_default(netif);
    netif_set_up(netif);

    start_application();

    while (1) {
        xemacif_input(netif);
        transfer_data();
    }

    cleanup_platform();
    return 0;
}
```

整体调用链：

```text
main()
  |
  |-- init_platform()
  |
  |-- IP4_ADDR()
  |
  |-- lwip_init()
  |
  |-- xemac_add()
  |
  |-- netif_set_default()
  |
  |-- netif_set_up()
  |
  |-- start_application()
  |
  `-- while (1)
       |
       |-- xemacif_input()
       |
       `-- transfer_data()
```

## 7. main.c 函数说明

### 7.1 init_platform()

功能：

```text
初始化板级运行环境。
```

通常包括：

```text
初始化 UART
打开 I-cache
打开 D-cache
初始化定时器
初始化中断控制器
```

没有它，串口打印、cache、timer、中断等基础功能可能无法正常工作。

### 7.2 IP4_ADDR()

功能：

```text
设置 IP 地址、子网掩码和网关。
```

示例：

```c
IP4_ADDR(&ipaddr,  192, 168, 1, 10);
IP4_ADDR(&netmask, 255, 255, 255, 0);
IP4_ADDR(&gw,      192, 168, 1, 1);
```

含义：

```text
ZYNQ IP: 192.168.1.10
Mask:    255.255.255.0
Gateway: 192.168.1.1
```

PC 端应配置在同一网段，例如：

```text
PC IP: 192.168.1.100
Mask:  255.255.255.0
```

### 7.3 lwip_init()

功能：

```text
初始化 lwIP 协议栈。
```

内部会初始化：

```text
内存池
pbuf 系统
ARP
IP
ICMP
TCP
UDP
网络接口链表
定时器管理
```

注意：

```text
lwip_init() 只初始化协议栈，不会自动初始化 ZYNQ 网卡。
```

### 7.4 xemac_add()

功能：

```text
把 ZYNQ GEM 网卡注册到 lwIP 协议栈。
```

调用形式：

```c
xemac_add(netif,
          &ipaddr,
          &netmask,
          &gw,
          mac_ethernet_address,
          PLATFORM_EMAC_BASEADDR);
```

参数说明：

```text
netif                  lwIP 网卡对象
ipaddr                 本机 IP
netmask                子网掩码
gw                     网关
mac_ethernet_address   MAC 地址
PLATFORM_EMAC_BASEADDR GEM 基地址
```

内部主要完成：

```text
创建 netif
设置 IP 参数
设置 MAC 地址
查找 GEM 硬件
初始化 GEM
初始化 DMA 描述符
初始化收发 buffer
初始化 PHY
配置链路速率
绑定底层发送函数
把 netif 加入 lwIP
```

它是软件协议栈和 ZYNQ 硬件网卡之间的桥梁。

### 7.5 netif_set_default()

功能：

```text
把当前 netif 设置为默认网络接口。
```

如果系统只有一个网卡，所有默认发送流量都会从这个接口出去。

### 7.6 netif_set_up()

功能：

```text
启用 lwIP 网络接口。
```

注意：

```text
netif up 表示软件接口启用。
PHY link up 表示物理链路建立。
两者不是同一件事。
```

### 7.7 platform_enable_interrupts()

部分 Xilinx 示例会显式调用：

```c
platform_enable_interrupts();
```

功能：

```text
打开平台中断。
```

常用于：

```text
GEM 接收中断
定时器中断
TCP fast timer
TCP slow timer
```

### 7.8 start_application()

功能：

```text
启动用户网络应用。
```

在 Echo Server 中，它会：

```text
创建 TCP PCB
绑定 TCP 端口
进入监听状态
注册连接回调函数
```

### 7.9 xemacif_input()

功能：

```text
从 GEM/DMA 获取收到的网络包，并交给 lwIP 处理。
```

内部流程：

```text
检查是否有接收包
读取 DMA buffer
cache invalidate
分配或构造 pbuf
把数据填入 pbuf
调用 netif->input()
lwIP 解析 ARP/IP/ICMP/TCP/UDP
```

裸机轮询模式下，它必须在主循环中持续调用。

### 7.10 transfer_data()

功能：

```text
应用层周期性处理函数。
```

在 TCP echo 示例中，它可能为空，因为主要数据处理发生在 TCP 接收回调中。

### 7.11 cleanup_platform()

功能：

```text
清理平台资源。
```

通常包括：

```text
关闭 cache
释放平台相关资源
```

裸机网络程序一般不会执行到这里，因为主循环不会退出。

## 8. TCP Echo Server 代码说明

典型 `echo.c`：

```c
void start_application(void)
{
    struct tcp_pcb *pcb;
    err_t err;
    unsigned port = 7;

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        xil_printf("Error creating PCB\r\n");
        return;
    }

    err = tcp_bind(pcb, IP_ANY_TYPE, port);
    if (err != ERR_OK) {
        xil_printf("Unable to bind to port %d\r\n", port);
        return;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        xil_printf("Out of memory while tcp_listen\r\n");
        return;
    }

    tcp_accept(pcb, accept_callback);

    xil_printf("TCP echo server started @ port %d\r\n", port);
}
```

### 8.1 tcp_new_ip_type()

功能：

```text
创建 TCP PCB。
```

PCB 用来记录 TCP 服务或 TCP 连接状态。

### 8.2 tcp_bind()

功能：

```text
把 TCP PCB 绑定到指定端口。
```

Echo Server 常用端口是 `7`。

### 8.3 tcp_listen()

功能：

```text
让 TCP PCB 进入监听状态。
```

此后，ZYNQ 可以接受 PC 发起的 TCP 连接。

### 8.4 tcp_accept()

功能：

```text
注册 TCP 连接建立回调。
```

当 PC 连接 ZYNQ 时，lwIP 会调用 `accept_callback()`。

### 8.5 accept_callback()

示例：

```c
static err_t accept_callback(void *arg,
                             struct tcp_pcb *newpcb,
                             err_t err)
{
    tcp_recv(newpcb, recv_callback);
    return ERR_OK;
}
```

功能：

```text
处理新的 TCP 连接。
```

主要做：

```text
获取新连接 PCB
注册接收回调 recv_callback()
```

### 8.6 tcp_recv()

功能：

```text
注册 TCP 数据接收回调。
```

PC 发数据后，lwIP 会调用该回调函数。

### 8.7 recv_callback()

示例：

```c
static err_t recv_callback(void *arg,
                           struct tcp_pcb *tpcb,
                           struct pbuf *p,
                           err_t err)
{
    if (!p) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->len);
    tcp_write(tpcb, p->payload, p->len, 1);

    pbuf_free(p);

    return ERR_OK;
}
```

功能：

```text
收到 TCP 数据后原样返回。
```

代码解释：

```text
if (!p)
    对端关闭连接，本地也关闭连接。

tcp_recved()
    告诉 lwIP 这些数据已经被应用层处理。

tcp_write()
    把接收到的数据重新写回发送缓冲区。

pbuf_free()
    释放接收数据包 buffer。
```

Echo Server 的核心逻辑就是：

```text
收到什么，返回什么。
```

## 9. 底层收包 Flow

从 PC 到 ZYNQ 应用：

```text
PC 发出以太网帧
  |
RJ45 接收
  |
PHY 解码电气信号
  |
RGMII/RMII 传给 GEM
  |
GEM 接收以太网帧
  |
DMA 写入 DDR buffer
  |
xemacif_input() 轮询到新包
  |
cache invalidate
  |
构造 lwIP pbuf
  |
netif->input()
  |
lwIP 解析协议
  |
ARP / ICMP / TCP / UDP 分发
  |
TCP 数据进入 recv_callback()
```

## 10. 底层发包 Flow

从 ZYNQ 应用到 PC：

```text
recv_callback()
  |
tcp_write()
  |
lwIP 封装 TCP
  |
lwIP 封装 IP
  |
lwIP 封装 Ethernet
  |
调用 netif->linkoutput()
  |
low_level_output()
  |
cache flush
  |
GEM DMA 读取 DDR buffer
  |
GEM 发送以太网帧
  |
PHY 转换成电气信号
  |
RJ45 / 网线
  |
PC 收到数据
```

## 11. PHY 初始化 Flow

PHY 初始化一般由底层驱动完成，逻辑如下：

```text
扫描或指定 PHY 地址
  |
MDIO 读取 PHY ID
  |
确认 PHY 存在
  |
复位 PHY
  |
启动自动协商
  |
等待 link up
  |
读取协商结果
  |
判断速率和双工
  |
配置 GEM speed
```

常见速率：

```text
10 Mbps
100 Mbps
1000 Mbps
```

如果 PHY 协商出的速率和 GEM 配置不一致，会导致无法正常收发。

## 12. ARP、Ping、TCP 过程

第一次 ping 时，PC 通常先发 ARP。

```text
PC: 谁是 192.168.1.10？
ZYNQ: 我是 192.168.1.10，我的 MAC 是 00:0A:35:00:01:02
PC: 发送 ICMP Echo Request
ZYNQ: 返回 ICMP Echo Reply
```

TCP echo 测试：

```text
PC -> ZYNQ: SYN
ZYNQ -> PC: SYN ACK
PC -> ZYNQ: ACK

TCP 连接建立

PC -> ZYNQ: data
ZYNQ -> PC: same data
```

测试结论：

```text
ping 通：
    ARP、IP、ICMP、MAC、PHY 基本正常。

TCP echo 通：
    TCP 协议栈、回调函数、应用逻辑也正常。
```

## 13. Cache 与 DMA

ZYNQ GEM 使用 DMA 访问 DDR，ARM CPU 也访问 DDR。

问题是：

```text
CPU 可能读写 cache
DMA 直接读写 DDR
cache 和 DDR 可能不一致
```

发送前需要：

```text
cache flush
```

接收后需要：

```text
cache invalidate
```

否则可能出现：

```text
ping 偶尔通
TCP 数据错误
收到旧数据
发送数据没有更新
```

Xilinx 的底层适配层通常已经处理大部分 cache 操作，但修改 buffer 或做零拷贝时要特别小心。

## 14. 常见调试路径

建议按层次排查：

```text
1. 串口是否打印
2. PHY 灯是否亮
3. PHY reset 是否释放
4. PHY 时钟是否存在
5. PHY ID 是否能读到
6. Link 是否 up
7. 速率是否协商成功
8. GEM0/GEM1 是否选对
9. MIO/EMIO 是否和原理图一致
10. IP 是否同网段
11. ARP 是否有回应
12. ping 是否通
13. TCP 是否能连接
14. recv_callback() 是否进入
15. tcp_write() 是否成功
```

## 15. 最容易出错的点

```text
PHY 地址错误
PHY reset 没有释放
PHY 时钟缺失
RGMII delay 配置错误
GEM0/GEM1 选错
MIO/EMIO 管脚配置错误
PC 和 ZYNQ 不在同一网段
防火墙阻止 ping 或 TCP 测试
主循环没有持续调用 xemacif_input()
cache flush/invalidate 处理不当
lwIP pbuf 没有释放
TCP 窗口没有通过 tcp_recved() 更新
```

## 16. 学习顺序建议

高效掌握这个案例，可以按下面顺序学习：

```text
1. 先理解硬件链路
   PC -> RJ45 -> PHY -> GEM -> DMA -> DDR -> CPU

2. 再理解工程 flow
   Vivado -> XSA -> Vitis -> BSP -> Application

3. 再看 main.c
   初始化平台 -> 初始化 lwIP -> 注册网卡 -> 启动应用 -> 主循环

4. 再看 echo.c
   tcp_new -> tcp_bind -> tcp_listen -> tcp_accept -> tcp_recv -> tcp_write

5. 再看底层适配
   xemacif_input -> pbuf -> lwIP -> low_level_output

6. 最后结合抓包
   ARP -> ICMP -> TCP
```

## 17. 一句话总结

ZYNQ7010 以太网案例的本质是：

```text
Vivado 配好 PS GEM、MIO/EMIO、DDR、UART；
Vitis BSP 生成硬件驱动和 lwIP 支持；
main.c 初始化平台、协议栈和网卡；
xemacif 把 GEM 收到的数据交给 lwIP；
echo.c 通过 TCP 回调实现应用逻辑；
主循环不断处理收包、定时器和应用任务。
```

## 18. 通俗理解版框架

可以把整个以太网案例想成一个快递系统。

```text
PC
    相当于寄件人或收件人。

网线 / RJ45
    相当于道路。

PHY
    相当于快递站门口的装卸工。
    它负责把网线上的电信号转换成芯片能理解的数据，也负责判断路通不通。

GEM
    相当于 ZYNQ 内部的快递分拣中心。
    它知道以太网帧怎么收、怎么发、MAC 地址怎么匹配。

DMA / DDR
    相当于仓库和搬运车。
    数据包来了以后，不靠 CPU 一点点搬，而是 DMA 直接搬到 DDR。

lwIP
    相当于规则管理员。
    它知道 ARP、IP、ICMP、TCP、UDP 这些网络协议该怎么处理。

echo.c
    相当于具体业务员。
    TCP echo 的业务很简单：收到什么，就原样返回什么。
```

所以学习时不要一上来就陷入驱动细节，先抓住这条主线：

```text
PHY 让线通
GEM 让帧能收发
DMA 把包搬到内存
lwIP 看懂协议
应用代码处理业务
```

## 19. 从零建立工程步骤

下面按实际做工程的顺序说明。

### 19.1 Vivado 硬件工程

第一步，创建 Vivado 工程。

```text
Create Project
选择器件或开发板
创建 Block Design
添加 ZYNQ7 Processing System
```

第二步，配置 ZYNQ PS。

```text
Run Block Automation
打开 ZYNQ7 Processing System 配置界面
确认 DDR 配置正确
打开 UART
打开 Ethernet GEM0 或 GEM1
打开 MDIO
确认 MIO 或 EMIO 选择和原理图一致
```

这里最关键的是看板级原理图：

```text
PHY 接在 GEM0 还是 GEM1
PHY 数据线走 MIO 还是 EMIO
PHY 地址是多少
PHY reset 由谁控制
PHY 需要什么参考时钟
```

第三步，检查 MIO/EMIO。

```text
如果 PHY 直接接 PS MIO：
    Vivado 中配置 GEM 使用 MIO。

如果 PHY 经过 PL 管脚：
    Vivado 中配置 GEM 使用 EMIO。
    还需要在 XDC 中约束 RGMII/RMII/MDIO/reset 等管脚。
```

第四步，生成硬件。

```text
Validate Design
Generate Output Products
Create HDL Wrapper
Generate Bitstream
Export Hardware，勾选 Include Bitstream
得到 .xsa 文件
```

`.xsa` 的作用：

```text
告诉 Vitis：
有哪些硬件外设
外设基地址是多少
中断号是多少
DDR 地址范围是多少
GEM 使用哪个实例
```

### 19.2 Vitis 软件工程

第一步，创建 Platform。

```text
New Platform Project
选择 Vivado 导出的 .xsa
生成 standalone BSP
```

第二步，检查 BSP。

```text
确认操作系统是 standalone
确认 CPU 是 ps7_cortexa9_0
确认 lwIP 库已启用
确认 stdin/stdout 使用正确 UART
```

第三步，创建应用。

```text
New Application Project
选择刚才的平台
模板选择 lwIP Echo Server
生成 main.c、echo.c、platform.c、xemacif 相关文件
```

第四步，配置 lwIP。

常见配置方向：

```text
静态 IP：
    关闭 DHCP，手动配置 IP。

动态 IP：
    打开 DHCP，等待路由器分配地址。

裸机 RAW API：
    适合资源少、流程清晰的入门案例。
```

第五步，编译运行。

```text
Build Project
连接 JTAG
Program FPGA
Run Application
打开串口终端
观察 IP、link、server started 等打印
```

### 19.3 PC 端测试

PC 设置同网段 IP：

```text
ZYNQ IP: 192.168.1.10
PC IP:   192.168.1.100
Mask:    255.255.255.0
```

测试 ping：

```powershell
ping 192.168.1.10
```

测试 TCP echo：

```powershell
telnet 192.168.1.10 7
```

或使用网络调试助手连接：

```text
协议：TCP Client
目标 IP：192.168.1.10
目标端口：7
```

## 20. 带注释代码阅读路径

本目录提供了两个注释版代码文件：

```text
code/main_annotated.c
code/echo_annotated.c
```

阅读顺序建议：

```text
先读 main_annotated.c
    目标：理解整个程序怎么启动。

再读 echo_annotated.c
    目标：理解 TCP 服务端怎么建立、怎么收包、怎么回包。

最后回到 Xilinx 原始工程
    对照真实 main.c、echo.c、xemacif.c、xemacpsif.c。
```

`main_annotated.c` 重点看这些函数：

```text
init_platform()
    准备串口、cache、timer、中断等基础环境。

lwip_init()
    初始化 TCP/IP 协议栈。

xemac_add()
    把 ZYNQ GEM 网卡注册进 lwIP。

netif_set_default()
    设置默认网卡。

netif_set_up()
    启动 lwIP 网络接口。

start_application()
    启动 TCP echo 应用。

xemacif_input()
    持续从 GEM/DMA 收包，并交给 lwIP。
```

`echo_annotated.c` 重点看这些函数：

```text
tcp_new_ip_type()
    创建 TCP 控制块。

tcp_bind()
    绑定 TCP 服务端端口。

tcp_listen()
    进入监听状态。

tcp_accept()
    注册新连接回调。

tcp_recv()
    注册数据接收回调。

tcp_recved()
    告诉 lwIP 已处理收到的数据。

tcp_write()
    把收到的数据写回发送缓冲区。

tcp_output()
    推动待发送数据尽快发出。

pbuf_free()
    释放接收缓冲区，避免内存池耗尽。
```

## 21. 把整个案例串起来

完整理解时，可以按下面这条线记忆：

```text
Vivado 中打开 GEM
    |
导出 XSA
    |
Vitis BSP 生成 xemacps 驱动和 lwIP 支持
    |
main.c 调用 init_platform()
    |
main.c 设置 IP / MAC
    |
main.c 调用 lwip_init()
    |
main.c 调用 xemac_add()
    |
xemac_add() 初始化 GEM、PHY、DMA、netif
    |
main.c 调用 start_application()
    |
echo.c 创建 TCP 监听服务
    |
PC 连接 ZYNQ
    |
lwIP 调用 accept_callback()
    |
PC 发送 TCP 数据
    |
lwIP 调用 recv_callback()
    |
recv_callback() 调用 tcp_write()
    |
ZYNQ 原样返回数据
```

这就是从工程建立到程序运行，再到网络数据真正收发的完整闭环。


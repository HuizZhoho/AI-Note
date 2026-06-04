/*
 * ZYNQ7010 lwIP TCP Echo Server 应用层注释版
 *
 * 这个文件展示 TCP echo 的核心思想：
 * - 创建 TCP 服务端
 * - 监听端口
 * - 接受连接
 * - 收到数据后原样发回
 */

#include "xil_printf.h"

#include "lwip/err.h"
#include "lwip/tcp.h"

/*
 * TCP echo 常用端口是 7。
 * PC 端可以使用网络调试助手或 telnet/nc 连接该端口。
 */
#define ECHO_PORT 7

static err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);

void start_application(void)
{
    struct tcp_pcb *pcb;
    err_t err;

    /*
     * 1. 创建 TCP 控制块。
     *
     * PCB 是 lwIP 管理 TCP 状态的核心对象。
     * 服务端监听需要一个 PCB，每条连接建立后也会有自己的 PCB。
     */
    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL) {
        xil_printf("Error creating TCP PCB\r\n");
        return;
    }

    /*
     * 2. 绑定端口。
     *
     * IP_ANY_TYPE 表示绑定到本机所有 IP。
     * ECHO_PORT 是服务端监听端口。
     */
    err = tcp_bind(pcb, IP_ANY_TYPE, ECHO_PORT);
    if (err != ERR_OK) {
        xil_printf("Unable to bind TCP port %d, error=%d\r\n", ECHO_PORT, err);
        tcp_close(pcb);
        return;
    }

    /*
     * 3. 进入监听状态。
     *
     * tcp_listen() 会把普通 PCB 转成监听 PCB。
     * 返回值可能和原 pcb 不同，所以必须使用返回值。
     */
    pcb = tcp_listen(pcb);
    if (pcb == NULL) {
        xil_printf("Out of memory while entering listen state\r\n");
        return;
    }

    /*
     * 4. 注册连接回调。
     *
     * PC 发起 TCP 连接并完成三次握手后，
     * lwIP 会调用 accept_callback()。
     */
    tcp_accept(pcb, accept_callback);

    xil_printf("TCP echo server started at port %d\r\n", ECHO_PORT);
}

static err_t accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }

    /*
     * 给新连接注册接收回调。
     *
     * 注意：
     * 监听 PCB 只负责接受连接；
     * newpcb 才代表这条已经建立的 TCP 连接。
     */
    tcp_recv(newpcb, recv_callback);

    return ERR_OK;
}

static err_t recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK) {
        if (p != NULL) {
            pbuf_free(p);
        }
        return err;
    }

    /*
     * p == NULL 表示对端关闭连接。
     *
     * TCP 中，对端关闭连接后，lwIP 会用空 pbuf 通知应用层。
     */
    if (p == NULL) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    /*
     * 通知 lwIP：应用层已经处理了 p->len 字节。
     *
     * 这会更新 TCP 接收窗口。
     * 如果不调用，长时间通信可能因为窗口不释放而卡住。
     */
    tcp_recved(tpcb, p->len);

    /*
     * Echo 的核心：把收到的数据原样写回发送缓冲区。
     *
     * 第四个参数 TCP_WRITE_FLAG_COPY 表示 lwIP 拷贝数据。
     * 这样 pbuf 释放后，发送数据仍然有效。
     */
    err = tcp_write(tpcb, p->payload, p->len, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        xil_printf("tcp_write failed, error=%d\r\n", err);
    }

    /*
     * 主动推动发送。
     *
     * 有些场景下只调用 tcp_write() 不一定立刻发出，
     * tcp_output() 可以请求 lwIP 尽快发送当前连接的待发数据。
     */
    tcp_output(tpcb);

    /*
     * 释放接收 pbuf。
     *
     * 这是必须动作，否则 pbuf 池会逐渐耗尽，后续无法收包。
     */
    pbuf_free(p);

    return ERR_OK;
}

void transfer_data(void)
{
    /*
     * Echo Server 的数据收发由 recv_callback() 驱动。
     * 这里保留为空，方便以后加入周期性任务。
     */
}


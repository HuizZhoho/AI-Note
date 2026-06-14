# 11 案例五：工业以太网控制与 Modbus TCP 项目

## 项目目标与硬件框图

目标：构建一个工业以太网控制设备，通过以太网 PHY 或 Switch 接入现场网络，用 Modbus TCP 提供寄存器读写、状态监控和控制命令。

典型结构：

```text
上位机 / PLC / SCADA
  |
工业交换机
  |
RJ45
  |
PHY 或 Switch Port
  |
MCU / ZYNQ / 嵌入式 Linux
  |
TCP/IP 协议栈
  |
Modbus TCP Server
  |
控制寄存器 / 传感器 / 执行器
```

代表芯片形态：

```text
单网口设备：
    MCU/SoC + PHY。

多网口设备：
    CPU + Switch ASIC + 多 PHY。

高速采集设备：
    FPGA/ZYNQ + PHY/10G + 自定义 UDP/TCP 控制。
```

## 工业现场和普通网络的区别

工业项目更关注：

```text
长时间稳定运行。
静态 IP 和可维护性。
断线重连。
心跳和超时。
电磁干扰。
接地、浪涌、ESD。
交换机环路和 VLAN 隔离。
现场人员可定位问题。
```

协议跑通只是第一步，现场可靠性和可诊断性更重要。

## 硬件设计检查点

| 项目 | 重点 |
| --- | --- |
| 工业 PHY | 工业温度、ESD、浪涌、隔离、线缆质量 |
| 电源 | 宽压输入、浪涌保护、电源隔离、掉电恢复 |
| Reset | 看门狗、PHY reset、系统复位策略 |
| 指示灯 | Power、Link、Activity、Run、Error |
| 多网口 | Switch VLAN、环网、端口隔离 |
| EMC | 共模电感、TVS、屏蔽、接地 |
| 维护接口 | 串口日志、Web 页面、恢复出厂设置 |

工业设备的 LED 很重要。现场没有示波器时，LED、串口日志和端口统计就是第一诊断入口。

## 网络配置策略

常见配置：

```text
静态 IP：
    工业现场最常见，便于 PLC/SCADA 固定连接。

DHCP：
    适合办公网络或自动部署，但现场可控性差。

默认网关：
    同网段控制可不需要；跨网段访问才需要。

DNS：
    Modbus TCP 通常不依赖 DNS。
```

建议设备提供：

```text
默认 IP。
可配置 IP/netmask/gateway。
恢复出厂设置。
IP 冲突检测。
网络参数保存到 flash。
```

## Modbus TCP 基础

Modbus TCP 使用 TCP 端口 502。

报文结构：

```text
MBAP Header
  Transaction ID
  Protocol ID = 0
  Length
  Unit ID

PDU
  Function Code
  Data
```

常见功能码：

```text
0x03：Read Holding Registers
0x04：Read Input Registers
0x06：Write Single Register
0x10：Write Multiple Registers
```

Modbus TCP 不再使用 RTU CRC，因为 TCP/IP 和以太网已有各层校验。应用层仍应检查长度、功能码和地址范围。

## 软件架构

裸机 lwIP：

```text
main()
  |
lwip_init()
  |
netif 初始化
  |
tcp_bind(port 502)
  |
tcp_accept()
  |
tcp_recv(modbus_recv_callback)
```

Linux：

```text
socket()
bind(0.0.0.0:502)
listen()
accept()
recv()
parse_modbus()
send()
```

Modbus server 伪代码：

```c
static int handle_modbus_request(uint8_t *req, int req_len,
                                 uint8_t *rsp, int rsp_max)
{
    struct mbap mbap;
    if (parse_mbap(req, req_len, &mbap) != 0) {
        return -1;
    }

    uint8_t function = req[7];

    switch (function) {
    case 0x03:
        return build_read_holding_response(req, req_len, rsp, rsp_max);
    case 0x06:
        return build_write_single_response(req, req_len, rsp, rsp_max);
    case 0x10:
        return build_write_multiple_response(req, req_len, rsp, rsp_max);
    default:
        return build_exception_response(req, rsp, function, 0x01);
    }
}
```

## 数据流

读寄存器：

```text
上位机发送 TCP 数据
  |
设备 TCP 协议栈收包
  |
Modbus 解析 MBAP 和功能码
  |
检查寄存器地址和数量
  |
读取内部状态表
  |
生成 response
  |
TCP 发送给上位机
```

写寄存器：

```text
上位机写控制寄存器
  |
设备检查权限、范围、状态
  |
更新 shadow register
  |
控制任务按周期应用到硬件
  |
返回写成功或异常码
```

建议不要在 TCP 回调里直接操作危险硬件动作。更稳的方式：

```text
网络任务写 shadow register。
控制任务按状态机执行动作。
安全检查失败则拒绝或进入错误状态。
```

## 心跳、超时和重连

工业现场常见问题是链路抖动和上位机断开。

建议设计：

```text
TCP keepalive 或应用层心跳。
命令超时。
连接断开后释放资源。
长时间无控制命令进入安全状态。
关键控制命令需要序号或确认。
```

状态机：

```text
INIT
  |
NETWORK_UP
  |
MODBUS_LISTEN
  |
CLIENT_CONNECTED
  |
CONTROL_ACTIVE
  |
TIMEOUT / DISCONNECT / ERROR
  |
SAFE_STATE
```

## Switch 和 VLAN 应用

工业设备带多网口时，内部可能有 Switch ASIC。

常见设计：

```text
端口 1：上位机网络。
端口 2：现场设备链路。
CPU Port：管理 CPU。
VLAN 10：控制网络。
VLAN 20：维护网络。
```

好处：

```text
隔离控制流量和维护流量。
避免广播风暴影响控制。
限制未知设备访问。
便于现场抓包和镜像。
```

但 VLAN 不是绝对安全边界。真正安全还需要 ACL、防火墙、认证和物理管理。

## 现场验证

PC 命令：

```powershell
ping 192.168.1.10
Test-NetConnection 192.168.1.10 -Port 502
```

Linux：

```bash
ping 192.168.1.10
nc -vz 192.168.1.10 502
tcpdump -i eth0 -nn -e tcp port 502
```

抓包判断：

```text
SYN/SYN ACK/ACK：
    TCP 连接建立。

PSH/ACK with payload：
    Modbus request/response。

RST：
    服务未监听、异常关闭或防火墙拒绝。

Retransmission：
    丢包、拥塞、链路质量或设备处理慢。
```

## 常见故障定位

| 现象 | 排查 |
| --- | --- |
| link 灯不亮 | PHY、电源、网线、工业交换机端口 |
| ping 不通 | IP、mask、VLAN、防火墙、ARP |
| 502 端口连不上 | Modbus server 是否启动、bind、listen、防火墙 |
| 能连接但读失败 | MBAP 长度、功能码、寄存器地址范围 |
| 偶发超时 | EMI、线缆、交换机拥塞、任务优先级、TCP 重传 |
| 多网口互相干扰 | VLAN/PVID、广播风暴、环路、STP |
| 现场恢复困难 | 缺少默认 IP、恢复出厂、日志和 LED 状态 |

## 项目交付建议

工业以太网项目应交付：

```text
默认网络参数说明。
寄存器表。
Modbus 功能码说明。
异常码说明。
LED 状态说明。
抓包示例。
现场排障流程。
固件版本和配置导出方法。
```

最终目标不是只让实验室 ping 通，而是让现场人员能定位“线不通、IP 不通、端口不通、协议不通、业务不通”分别是哪一层。

## 延伸阅读

- [TCP/IP 协议详解](../TCP_IP协议详解/README.md)
- [ZYNQ7010 以太网通信协议扩展详解](../ZYNQ7010_以太网案例/以太网通信协议扩展详解.md)
- [调试排障与学习路线](./05_调试排障与学习路线.md)

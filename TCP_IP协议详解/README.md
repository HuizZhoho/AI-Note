# TCP/IP + IPv6 协议完整详细解读

本文是一份从入门到熟悉掌握的 TCP/IP 协议族学习笔记。它不是 RFC 翻译，而是按工程学习路径组织：先建立直觉，再理解分层和报文，最后能用命令和抓包定位问题。

配套文档：

```text
抓包与排查速查.md
    常用命令、Wireshark 过滤表达式、典型故障定位流程。
```

相关案例：

[ZYNQ7010 以太网案例通信协议与电气特性完整讲解](../ZYNQ7010_以太网案例/通信协议完整讲解.md)

你学完后应该能回答这些问题：

```text
为什么 ping 通不代表网页一定能打开？
为什么知道 IP 还要 ARP 或 NDP？
为什么 TCP 要三次握手和四次挥手？
为什么 TCP 是字节流，不是消息包？
为什么 IPv4 要子网掩码，IPv6 要前缀长度？
一次访问 https://example.com 背后到底发生了哪些协议交互？
```

## 1. 学习路线

建议按下面顺序学，不要一开始就背报文格式。

```text
第一阶段：能看懂现象
    ping、IP 地址、网关、DNS、端口、TCP/UDP。

第二阶段：能解释一次通信
    DHCP 获取地址 -> ARP/NDP 找网关 -> DNS 解析 -> TCP 连接 -> HTTP 请求。

第三阶段：能看懂抓包
    Ethernet、ARP、IPv4、IPv6、ICMP、TCP、UDP、DNS、HTTP。

第四阶段：能排查问题
    IP 不通、DNS 不通、TCP 连不上、能 ping 不能访问网页、丢包和延迟。

第五阶段：能联系工程
    嵌入式 lwIP、Linux 网络栈、交换机、路由器、防火墙、NAT。
```

一个最小但完整的网络认知：

```text
MAC 地址解决局域网内把帧送给谁。
IP 地址解决网络层把包送到哪个主机。
端口解决主机内把数据交给哪个进程。
DNS 解决域名到 IP 的转换。
TCP 解决可靠传输。
UDP 解决简单快速的数据报传输。
```

## 2. 网络分层

### 2.1 为什么要分层

网络通信很复杂，如果所有事情混在一起，会很难设计、实现和排错。分层的目的就是让每一层只关心自己的职责。

通俗理解：

```text
应用层：我要访问网页、下载文件、同步时间。
传输层：这段数据交给哪个应用？要不要可靠？
网络层：目标主机在哪里？下一跳是谁？
链路层：在当前局域网内，下一跳的 MAC 是谁？
物理层：电信号、光信号、无线信号怎么传？
```

### 2.2 OSI 七层和 TCP/IP 四层

| OSI 七层 | TCP/IP 常见分层 | 典型协议/概念 |
| --- | --- | --- |
| 应用层 | 应用层 | HTTP、DNS、DHCP、NTP、TLS |
| 表示层 | 应用层 | 编码、压缩、加密 |
| 会话层 | 应用层 | 会话管理 |
| 传输层 | 传输层 | TCP、UDP、端口 |
| 网络层 | 网络层 | IPv4、IPv6、ICMP、ICMPv6、路由 |
| 数据链路层 | 网络接口层 | Ethernet、ARP、NDP、MAC、交换机 |
| 物理层 | 网络接口层 | 网线、光纤、PHY、电平、时钟 |

实际工程里常用 TCP/IP 四层：

```text
应用层
传输层
网络层
网络接口层
```

### 2.3 数据封装

发送数据时，每层都会加自己的头。

```text
应用数据
  |
TCP/UDP 头 + 应用数据
  |
IP 头 + TCP/UDP 头 + 应用数据
  |
Ethernet 头 + IP 头 + TCP/UDP 头 + 应用数据 + FCS
  |
变成电信号/光信号/无线信号发出去
```

接收时反过来拆：

```text
Ethernet -> IP -> TCP/UDP -> 应用数据
```

这叫封装和解封装。

## 3. 链路层：Ethernet、MAC、MTU、交换机

### 3.1 它解决什么问题

链路层解决的是“当前这一个局域网内，把一帧数据送给谁”。

关键概念：

```text
MAC 地址：
    网卡的链路层地址。

Ethernet Frame：
    以太网帧，局域网内传输的基本单位。

交换机：
    根据 MAC 地址转发以太网帧。

MTU：
    一帧里能承载的最大网络层数据长度，常见以太网 MTU 是 1500 字节。
```

### 3.2 以太网帧长什么样

简化结构：

```text
目的 MAC
源 MAC
类型 EtherType
载荷 Payload
FCS 校验
```

常见 EtherType：

```text
0x0800  IPv4
0x0806  ARP
0x86DD  IPv6
```

看到 EtherType，接收方才知道后面的载荷交给 IPv4、ARP 还是 IPv6 处理。

### 3.3 MAC 地址

MAC 地址通常写成：

```text
00:0A:35:00:01:02
```

常见类型：

```text
单播 MAC：
    发给某一块网卡。

广播 MAC：
    ff:ff:ff:ff:ff:ff，发给当前局域网所有设备。

组播 MAC：
    发给一组设备。
```

### 3.4 交换机怎么工作

交换机会学习“哪个 MAC 在哪个端口”。

```text
收到一帧
  |
记录源 MAC 来自哪个端口
  |
查看目的 MAC
  |
如果知道目的端口，就只转发到那个端口
  |
如果不知道，就泛洪到其他端口
```

交换机工作在二层，不看 TCP 端口，也不负责 IP 路由。

### 3.5 MTU

MTU 是 Maximum Transmission Unit。

常见以太网 MTU：

```text
1500 bytes
```

如果 IP 包超过链路 MTU：

```text
IPv4 可能分片
IPv6 路由器不分片，由发送端根据路径 MTU 调整
```

常见问题：

```text
小包 ping 通，大包不通：
    可能是 MTU、分片、防火墙或隧道问题。
```

### 3.6 抓包验证

Wireshark 里看 Ethernet 层：

```text
eth
eth.addr == 00:0a:35:00:01:02
eth.type == 0x0800
eth.type == 0x86dd
```

## 4. IPv4 地址解析：ARP

### 4.1 它解决什么问题

IPv4 通信中，主机知道目标 IP 还不够。以太网发送帧时需要目标 MAC。

ARP 解决：

```text
已知 IPv4 地址，如何找到对应 MAC 地址？
```

### 4.2 ARP 过程

主机 A 要找 `192.168.1.10`：

```text
A 广播：谁是 192.168.1.10？
目标主机回复：我是 192.168.1.10，我的 MAC 是 xx:xx:xx:xx:xx:xx。
A 把这个关系写入 ARP 缓存。
```

ARP Request 是广播，以太网目的 MAC：

```text
ff:ff:ff:ff:ff:ff
```

ARP Reply 通常是单播。

### 4.3 同网段和跨网段

如果目标 IP 和本机同网段：

```text
ARP 查询目标主机的 MAC。
```

如果目标 IP 不同网段：

```text
ARP 查询默认网关的 MAC。
把 IP 包交给网关。
网关再负责路由到远端网络。
```

这是很多新手容易误解的地方：跨网段通信时，以太网帧的目的 MAC 是网关 MAC，不是最终服务器的 MAC。

### 4.4 命令和抓包

Windows：

```powershell
arp -a
ping 192.168.1.10
```

Wireshark：

```text
arp
arp.opcode == 1
arp.opcode == 2
```

常见问题：

```text
ARP 没有回应：
    IP 不在同网段、目标不在线、网线/交换机问题、防火墙或虚拟网卡选错。

ARP 缓存错误：
    可能导致访问到错误设备，可清空 ARP 缓存后重试。
```

## 5. IPv6 地址解析：NDP

### 5.1 IPv6 为什么不用 ARP

IPv6 不使用 ARP。IPv6 使用 NDP，也就是 Neighbor Discovery Protocol。

NDP 基于 ICMPv6，负责：

```text
邻居地址解析
路由器发现
前缀发现
地址自动配置
重复地址检测
```

### 5.2 NDP 常见消息

| 消息 | 作用 |
| --- | --- |
| Neighbor Solicitation | 询问某个 IPv6 地址对应的链路层地址 |
| Neighbor Advertisement | 回复自己的链路层地址 |
| Router Solicitation | 主机寻找路由器 |
| Router Advertisement | 路由器通告前缀、网关等信息 |

### 5.3 IPv6 地址解析过程

类似 IPv4 ARP，但不是广播，而是使用组播：

```text
主机想找某个 IPv6 邻居
  |
发送 Neighbor Solicitation
  |
目标主机回复 Neighbor Advertisement
  |
本机记录 IPv6 地址到 MAC 的映射
```

### 5.4 抓包验证

Wireshark：

```text
icmpv6
icmpv6.type == 135
icmpv6.type == 136
```

注意：

```text
IPv4：
    ARP 负责邻居解析。

IPv6：
    NDP/ICMPv6 负责邻居解析。
```

## 6. IPv4：地址、子网、网关、路由

### 6.1 它解决什么问题

IPv4 解决“数据包要送到哪台主机，以及下一跳往哪走”。

IPv4 地址例子：

```text
192.168.1.100
```

配合子网掩码：

```text
255.255.255.0
```

表示本机所在网段：

```text
192.168.1.0/24
```

### 6.2 子网掩码和 CIDR

`192.168.1.100/24` 表示：

```text
前 24 bit 是网络部分
后 8 bit 是主机部分
```

常见写法：

| CIDR | 子网掩码 | 可用主机数量约 |
| --- | --- | --- |
| /24 | 255.255.255.0 | 254 |
| /16 | 255.255.0.0 | 65534 |
| /8 | 255.0.0.0 | 很多 |

判断是否同网段：

```text
本机 IP 与掩码做与运算
目标 IP 与掩码做与运算
结果相同就是同网段
```

### 6.3 默认网关

默认网关是“去其他网段时交给谁”。

```text
目标同网段：
    直接 ARP 目标主机。

目标不同网段：
    ARP 默认网关。
    把 IP 包交给网关。
```

### 6.4 路由

路由表决定下一跳。

Windows 查看：

```powershell
route print
```

Linux 查看：

```bash
ip route
```

路由器每转发一次，IPv4 的 TTL 会减 1。TTL 变成 0 时，路由器丢弃包并返回 ICMP 超时。

### 6.5 IPv4 分片

当 IP 包大于链路 MTU 时，IPv4 可能分片。

问题：

```text
分片丢失会导致整个包失败。
很多网络会阻止分片或 ICMP，造成大包异常。
```

## 7. IPv6：地址、前缀、SLAAC、Hop Limit

### 7.1 它解决什么问题

IPv6 主要解决 IPv4 地址不足，并改进地址配置、邻居发现、扩展头等机制。

IPv6 地址例子：

```text
2001:db8:1234:5678::1
fe80::1
```

### 7.2 IPv6 地址类型

| 类型 | 示例 | 作用 |
| --- | --- | --- |
| Link-local | fe80::/10 | 同一链路内通信，不能跨路由 |
| Global Unicast | 2000::/3 | 全球单播地址 |
| Multicast | ff00::/8 | 组播 |
| Loopback | ::1 | 本机回环 |
| Unspecified | :: | 未指定地址 |

### 7.3 前缀长度

IPv6 不使用 IPv4 那种点分十进制掩码，常用前缀长度：

```text
2001:db8:1234:5678::/64
```

很多局域网 IPv6 子网使用 `/64`。

### 7.4 SLAAC

SLAAC 是 Stateless Address Autoconfiguration。

过程：

```text
主机生成 link-local 地址
  |
进行重复地址检测
  |
发送 Router Solicitation
  |
收到 Router Advertisement
  |
根据前缀自动生成 IPv6 地址
```

### 7.5 Hop Limit

IPv6 使用 Hop Limit，对应 IPv4 的 TTL。

```text
每过一个路由器减 1
变成 0 则丢弃
```

### 7.6 IPv6 不由路由器分片

IPv6 路由器不做中途分片。发送端需要通过路径 MTU 发现决定合适包长。

这点和 IPv4 不同。

## 8. ICMP 和 ICMPv6

### 8.1 ICMP 解决什么问题

ICMP 是网络层控制和诊断协议。

常见用途：

```text
ping
traceroute/tracert
目标不可达
TTL 超时
路径 MTU 发现
```

### 8.2 ping

ping 使用：

```text
ICMP Echo Request
ICMP Echo Reply
```

ping 通说明：

```text
目标 IP 可达
链路层、网络层基本正常
ICMP 没被阻止
```

ping 不通不一定代表服务不可用，因为很多服务器或防火墙会禁 ICMP。

### 8.3 traceroute / tracert

traceroute 利用 TTL/Hop Limit 逐步增加的方式探测路径。

Windows：

```powershell
tracert example.com
```

Linux/macOS：

```bash
traceroute example.com
```

### 8.4 ICMPv6 更重要

IPv6 中 ICMPv6 不只是 ping，还承担 NDP、SLAAC 等基础功能。

不要随意完全阻断 ICMPv6，否则 IPv6 网络可能异常。

## 9. UDP：简单快速的数据报

### 9.1 它解决什么问题

UDP 只提供最基本的传输层封装：

```text
源端口
目的端口
长度
校验和
数据
```

它不保证：

```text
不保证到达
不保证顺序
不自动重传
不建立连接
```

### 9.2 适用场景

```text
DNS 查询
DHCP
实时音视频
游戏
设备发现
日志上报
自定义轻量协议
```

### 9.3 UDP 的通俗理解

UDP 像寄明信片：

```text
写上目的地址和内容
直接投出去
不确认对方是否收到
```

应用如果需要可靠性，要自己设计确认、重传、序号和超时。

### 9.4 抓包

Wireshark：

```text
udp
udp.port == 53
```

## 10. TCP：可靠字节流

### 10.1 它解决什么问题

TCP 提供面向连接、可靠、有序、基于字节流的传输。

它负责：

```text
连接建立
数据确认
丢包重传
顺序恢复
流量控制
拥塞控制
连接关闭
```

### 10.2 端口

IP 定位主机，端口定位进程。

```text
192.168.1.10:80
```

表示访问 `192.168.1.10` 主机上的 80 端口。

常见端口：

```text
80    HTTP
443   HTTPS
53    DNS
67/68 DHCP
123   NTP
22    SSH
```

### 10.3 三次握手

TCP 建立连接：

```text
客户端 -> 服务器：SYN
服务器 -> 客户端：SYN + ACK
客户端 -> 服务器：ACK
```

通俗理解：

```text
客户端：我想建立连接，我的起始序号是 x。
服务器：可以，我收到了 x；我的起始序号是 y。
客户端：我收到了 y，连接开始。
```

为什么不是两次？

```text
因为双方都要确认：
我能发出去
我能收回来
对方也能发出去
对方也能收回来
```

### 10.4 序列号和确认号

TCP 是按字节编号的。

```text
Sequence Number：
    当前发送数据的第一个字节编号。

Acknowledgment Number：
    我期望收到的下一个字节编号。
```

如果 ACK 是 1001，意思是：

```text
1000 及以前的字节我都收到了，请从 1001 开始继续发。
```

### 10.5 TCP 是字节流，不是消息包

这是必须掌握的重点。

应用调用两次 `send()`：

```text
send("hello")
send("world")
```

接收端可能看到：

```text
"helloworld"
```

也可能看到：

```text
"hel"
"lowor"
"ld"
```

TCP 只保证字节顺序，不保留应用消息边界。应用层如果需要消息边界，需要自己设计：

```text
固定长度
长度字段 + 数据
分隔符
TLV
JSON 行协议
```

### 10.6 滑动窗口

TCP 不能每发一个字节就停下来等 ACK，那样效率太低。

滑动窗口允许发送方连续发送一批数据。

```text
窗口大小：
    接收方告诉发送方自己还能接收多少数据。
```

如果接收方处理慢，窗口会变小；如果窗口为 0，发送方会暂停大量发送。

### 10.7 超时重传和快速重传

超时重传：

```text
发出数据后长时间没收到 ACK，就重发。
```

快速重传：

```text
连续收到多个重复 ACK，说明中间可能丢了一段，提前重传。
```

### 10.8 拥塞控制

流量控制保护接收方，拥塞控制保护网络。

几个核心概念：

```text
cwnd：
    拥塞窗口，发送方根据网络情况自己控制。

慢启动：
    刚开始小心发送，确认正常后快速增加。

拥塞避免：
    增长变慢，避免把网络打满。

丢包：
    通常被 TCP 视为网络拥塞信号。
```

通俗理解：

```text
刚上高速先慢慢开。
发现路况很好，再逐步提速。
如果发现堵车或事故，就降速。
```

### 10.9 四次挥手

TCP 关闭连接：

```text
主动方 -> 被动方：FIN
被动方 -> 主动方：ACK
被动方 -> 主动方：FIN
主动方 -> 被动方：ACK
```

为什么通常是四次？

```text
TCP 是全双工。
一方说我不发了，不代表另一方也立刻不发。
双方的发送方向要分别关闭。
```

### 10.10 抓包

Wireshark：

```text
tcp
tcp.flags.syn == 1
tcp.flags.fin == 1
tcp.analysis.retransmission
tcp.port == 443
```

## 11. DNS：域名解析

### 11.1 它解决什么问题

人记域名，机器用 IP。DNS 负责：

```text
www.example.com -> IP 地址
```

### 11.2 常见记录

| 记录 | 作用 |
| --- | --- |
| A | 域名到 IPv4 |
| AAAA | 域名到 IPv6 |
| CNAME | 域名别名 |
| MX | 邮件服务器 |
| NS | 权威 DNS 服务器 |
| TXT | 文本记录，常用于验证 |

### 11.3 DNS 查询过程

简化过程：

```text
浏览器/系统先查缓存
  |
查本机 hosts
  |
问本地 DNS 服务器
  |
必要时递归查询根、顶级域、权威 DNS
  |
返回 A/AAAA 记录
```

### 11.4 命令

Windows：

```powershell
nslookup example.com
ipconfig /displaydns
ipconfig /flushdns
```

Wireshark：

```text
dns
dns.qry.name == "example.com"
```

## 12. DHCP：自动获取网络配置

### 12.1 它解决什么问题

DHCP 自动给主机分配：

```text
IP 地址
子网掩码
默认网关
DNS 服务器
租约时间
```

### 12.2 IPv4 DHCP 过程

经典四步：

```text
Discover：
    客户端广播，我需要地址。

Offer：
    服务器提供一个地址。

Request：
    客户端请求使用这个地址。

ACK：
    服务器确认租约。
```

### 12.3 DHCP 和 ARP 的关系

刚开机时，主机可能还没有 IP，所以 DHCP 使用广播完成初始配置。拿到 IP 后，后续通信仍然需要 ARP 查 MAC。

### 12.4 IPv6 中的地址配置

IPv6 常见两种方式：

```text
SLAAC：
    根据路由器通告自动生成地址。

DHCPv6：
    由 DHCPv6 服务器分配或补充配置。
```

## 13. HTTP、HTTPS、TLS

### 13.1 HTTP

HTTP 是应用层协议，用于请求和响应资源。

请求示例：

```http
GET / HTTP/1.1
Host: example.com
```

响应示例：

```http
HTTP/1.1 200 OK
Content-Type: text/html
```

### 13.2 HTTPS

HTTPS = HTTP + TLS。

```text
TCP 先建立连接
  |
TLS 握手协商加密参数
  |
在加密通道里传 HTTP
```

### 13.3 TLS 概念

TLS 解决：

```text
身份认证：
    通过证书确认对方是谁。

加密：
    防止内容被窃听。

完整性：
    防止内容被篡改。
```

抓包时，如果是 HTTPS，通常能看到 TCP 和 TLS，但看不到明文 HTTP 内容。

## 14. NTP 和常见应用层协议

NTP 用于时间同步，常用 UDP 123。

常见协议速查：

| 协议 | 层级 | 常见端口 | 用途 |
| --- | --- | --- | --- |
| DNS | 应用层 | UDP/TCP 53 | 域名解析 |
| DHCP | 应用层 | UDP 67/68 | 自动获取 IP |
| HTTP | 应用层 | TCP 80 | Web 明文访问 |
| HTTPS | 应用层 | TCP 443 | Web 加密访问 |
| NTP | 应用层 | UDP 123 | 时间同步 |
| SSH | 应用层 | TCP 22 | 远程登录 |

## 15. 一次访问网页的完整过程

以访问 `https://example.com` 为例。

### 15.1 电脑接入网络

```text
网卡 link up
  |
系统发现网络连接
  |
准备获取 IP 配置
```

### 15.2 DHCP 获取配置

IPv4 常见过程：

```text
DHCP Discover
DHCP Offer
DHCP Request
DHCP ACK
```

主机获得：

```text
IP 地址
子网掩码
默认网关
DNS 服务器
```

IPv6 可能通过 RA/SLAAC 或 DHCPv6 获得配置。

### 15.3 找到网关 MAC

如果目标服务器不在本地网段，主机先把包交给默认网关。

IPv4：

```text
ARP 查询默认网关 MAC
```

IPv6：

```text
NDP 查询默认路由器的链路层地址
```

### 15.4 DNS 查询域名

```text
example.com -> A/AAAA 记录
```

系统可能得到：

```text
IPv4 地址
IPv6 地址
```

现代系统可能优先尝试 IPv6，也可能使用 Happy Eyeballs 机制并行选择更快路径。

### 15.5 TCP 三次握手

浏览器连接服务器：

```text
客户端 -> 服务器：SYN
服务器 -> 客户端：SYN ACK
客户端 -> 服务器：ACK
```

连接建立后，TCP 字节流可用。

### 15.6 TLS 握手

HTTPS 需要 TLS：

```text
协商 TLS 版本和加密套件
服务器发送证书
客户端验证证书
双方生成会话密钥
```

### 15.7 HTTP 请求和响应

```text
浏览器发送 HTTP 请求
服务器返回 HTTP 响应
浏览器解析 HTML/CSS/JS
继续请求图片、脚本、接口数据
```

### 15.8 TCP 关闭连接

可能发生：

```text
连接复用，暂时不关闭
主动关闭，四次挥手
异常关闭，RST
```

## 16. 抓包实践

### 16.1 推荐观察顺序

```text
1. arp 或 icmpv6，确认邻居解析
2. dns，确认域名解析
3. tcp.flags.syn == 1，确认 TCP 建连
4. tls，确认 HTTPS 加密握手
5. http，观察明文 HTTP
```

### 16.2 常用过滤

```text
arp
icmp
icmpv6
dns
tcp
udp
http
tls
ip.addr == 192.168.1.100
ipv6.addr == fe80::1
tcp.flags.syn == 1
tcp.analysis.retransmission
```

### 16.3 抓包判断思路

```text
没有 ARP/NDP：
    可能没发起通信、网卡选错、目标不在预期网络。

只有 DNS 请求没有响应：
    DNS 服务器不可达或被拦截。

TCP 只有 SYN 没有 SYN ACK：
    对端不可达、端口未开、防火墙阻断。

TCP 建立后没有应用响应：
    应用服务异常、协议不匹配、TLS/HTTP 层失败。
```

## 17. 常见故障排查路径

### 17.1 IP 不通

检查顺序：

```text
网卡是否启用
IP/掩码/网关是否正确
是否同网段
ARP/NDP 是否成功
网关是否可 ping
路由表是否正确
防火墙是否阻止
```

命令：

```powershell
ipconfig /all
ping 网关IP
arp -a
route print
```

### 17.2 DNS 不通

现象：

```text
ping 8.8.8.8 通
ping example.com 不通
```

重点查：

```text
DNS 服务器地址
DNS 查询是否有响应
本机 hosts
DNS 缓存
```

命令：

```powershell
nslookup example.com
ipconfig /displaydns
ipconfig /flushdns
```

### 17.3 TCP 连不上

现象：

```text
ping 通，但应用连不上。
```

重点查：

```text
服务端端口是否监听
防火墙是否放行
SYN 是否发出
是否收到 SYN ACK
是否被 RST
```

命令：

```powershell
netstat -ano
```

Wireshark：

```text
tcp.flags.syn == 1
tcp.flags.reset == 1
```

### 17.4 能 ping 不能访问网页

可能原因：

```text
DNS 问题
TCP 80/443 被阻止
代理配置问题
TLS 证书问题
HTTP 服务异常
浏览器缓存或安全策略
```

排查顺序：

```text
ping 目标 IP
nslookup 域名
检查 TCP 443 握手
检查 TLS 握手
检查 HTTP 状态码
```

## 18. 和 ZYNQ7010 以太网案例的关系

本目录讲的是通用 TCP/IP 协议族。ZYNQ7010 文档讲的是这些协议在嵌入式以太网案例中的落地。

建议结合阅读：

```text
先读本目录：
    建立 TCP/IP、IPv4、IPv6、TCP、UDP、DNS、DHCP 的通用理解。

再读 ZYNQ7010 案例：
    理解 PHY、GEM、DMA、lwIP 如何把这些协议跑在板子上。
```

相关文档：

[ZYNQ7010 以太网案例通信协议与电气特性完整讲解](../ZYNQ7010_以太网案例/通信协议完整讲解.md)

## 19. 最小掌握清单

如果你能清楚解释下面这些点，就算已经从新手走到熟悉阶段：

```text
1. MAC、IP、端口分别解决什么问题。
2. 同网段通信和跨网段通信的区别。
3. IPv4 为什么用 ARP，IPv6 为什么用 NDP。
4. DNS 查询在 TCP 连接之前发生。
5. TCP 三次握手和四次挥手每一步的意义。
6. TCP 为什么是字节流，不是消息包。
7. UDP 为什么快，但需要应用自己保证可靠性。
8. DHCP 如何让主机自动获得 IP、网关、DNS。
9. HTTPS 为什么先有 TCP，再有 TLS，再有 HTTP。
10. Wireshark 中如何按 ARP、DNS、TCP、HTTP 分层定位问题。
```

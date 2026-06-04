# 注释代码说明

本目录中的代码是根据 Xilinx / Vitis lwIP Echo Server 案例整理的学习版注释代码。

## 文件说明

```text
main_annotated.c
    说明裸机 lwIP 工程的主流程：
    平台初始化 -> IP 配置 -> lwIP 初始化 -> 网卡注册 -> 应用启动 -> 主循环收包。

echo_annotated.c
    说明 TCP Echo Server 的应用层流程：
    tcp_new -> tcp_bind -> tcp_listen -> tcp_accept -> tcp_recv -> tcp_write。
```

## 使用方式

这些文件主要用于阅读和理解，不建议直接覆盖 Vitis 自动生成的工程文件。

实际工程中应根据自己的 Vitis 版本、BSP 配置和板级 PHY 驱动，对头文件、宏名、定时器处理方式进行匹配。


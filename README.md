# FNOS Remote UPS Server

解决飞牛NAS（fnOS）系统 UPS 支持局限性的工具。

![](post/setup0.jpg)

## 背景与用途

飞牛NAS（fnOS）的 UPS 管理功能仅支持直接连接本机的特定 USB UPS 硬件设备，无法识别通过网络共享的 UPS。

对于已有 NUT（Network UPS Tools）服务器共享 UPS 状态的场景——例如 UPS 通过 USB 接在路由器、服务器或其他 NAS 上，fnOS 系统本身无法直接接入。

本工具通过以下方式解决该问题：

1. 连接到局域网中已有的 NUT UPS 服务器，实时读取 UPS 状态
2. 在本机启动一个 USB/IP 服务端，将 UPS 模拟为标准 USB HID UPS 设备
3. 通过 `usbip attach` 将模拟设备挂载到 fnOS 系统
4. fnOS 识别到挂载的虚拟 USB UPS 设备，正常接管 UPS 监控和断电保护

```
[实体UPS] ──USB──> [NUT服务器] ──网络──> [本工具 fnos-remote-ups]
                                                    │
                                               USB/IP 模拟
                                                    │
                                              [fnOS 系统]
                                          识别为本地 USB UPS
```

## 项目信息

- **项目地址**: https://github.com/iwinmin/fnos-remote-ups
- **安装说明**: [飞牛系统安装说明](docs/install.md)
- **开发者**: Winmin

## 功能特性

- 通过 USB/IP 协议将远程 NUT UPS 模拟为本地 USB HID 设备
- 实时同步远程 UPS 状态到 USB HID 报告
- 支持标准 USB HID UPS 协议，兼容 fnOS 内置 UPS 识别
- 兼容 NUT 系统的 usbhid-ups 驱动
- 支持 `-m` 参数自动执行 usbip attach，简化部署
- 支持调试模式输出详细信息

## 系统要求

**服务端（运行本工具的机器，通常是 fnOS 本机）**

- Linux 操作系统（fnOS 基于 Linux）
- `usbip` 工具及 `vhci-hcd` 内核模块
- 可访问局域网中的 NUT 服务器

**NUT 服务端（已有的 UPS 共享服务器）**

- 已安装并运行 NUT，UPS 可通过 `upsc` 正常查询

## 安装和编译

### 1. 准备编译环境

```bash
# Ubuntu/Debian / fnOS
sudo apt-get install build-essential libuv1-dev
```

### 2. 编译程序

```bash
cd fnos-remote-ups
make
```

编译产物为当前目录下的 `fnos-remote-ups` 可执行文件。

也可以使用 Docker 构建，适合交叉编译或不想污染宿主环境：

```bash
docker build -f docker/Dockerfile -t fnos-remote-ups .
```

## 使用方法

### 基本用法

```bash
# 连接到远程NUT服务器上的UPS设备
./fnos-remote-ups -u ups_name@server_ip

# 指定端口并启用调试模式
./fnos-remote-ups -p 3240 -u ups_name@server_ip -d
```

### 参数说明

```
用法: fnos-remote-ups [-p port] [-u ups_name@host[:port]] [-m [host:port@bus-id]] [-d] [-h]

  -p port    设置 USB/IP 监听端口 (默认: 3240)
  -u ups     远程 NUT UPS 标识符，格式: ups_name@host[:nut_port] (必填)
  -m mount   启用 usbip 自动挂载，格式: [host:usbip_port@bus-id]
             默认 host: 127.0.0.1，port: 同 -p，bus-id: 1-1
  -d         启用调试输出
  -h         显示帮助信息
```

### 示例

```bash
# 连接到 192.168.1.100 上的 myups，使用默认 NUT 端口 3493
./fnos-remote-ups -u myups@192.168.1.100

# 连接到指定 NUT 端口
./fnos-remote-ups -u myups@192.168.1.100:5000

# 启动后自动在本机执行 usbip attach（bus-id 默认 1-1）
./fnos-remote-ups -u myups@192.168.1.100 -m

# 指定远程 USB/IP 服务器和 bus-id 进行自动挂载
./fnos-remote-ups -u myups@192.168.1.100 -m 192.168.1.200:3240@1-1

# 使用自定义端口并启用调试
./fnos-remote-ups -p 8080 -u myups@192.168.1.100 -d
```

## Docker 部署

### 从 GitHub Release 导入镜像（推荐）

从 [Releases](https://github.com/iwinmin/fnos-remote-ups/releases) 页面下载对应版本的 `.docker.img.gz` 文件，然后导入到本地 Docker：

```bash
# 下载镜像文件（以 v1.0.0 为例）
wget https://github.com/iwinmin/fnos-remote-ups/releases/download/v1.0.0/fnos-remote-ups-v1.0.0.docker.img.gz

# 导入镜像
docker image load -i fnos-remote-ups-v1.0.0.docker.img.gz

# 确认镜像已导入
docker images | grep fnos-remote-ups
```

### 构建镜像

```bash
docker build -f docker/Dockerfile -t fnos-remote-ups .
```

### Host 网络模式（推荐）

使用 host 网络模式时，容器与宿主机共享网络栈，`AUTO_MOUNT=true` 即可通过 `127.0.0.1` 自动完成 `usbip attach`。

```bash
docker run -d \
  --name fnos-remote-ups \
  --privileged \
  --network host \
  --restart unless-stopped \
  -e REMOTE_UPS="myups@192.168.1.100" \
  -e AUTO_MOUNT="true" \
  fnos-remote-ups
```

> `--privileged` 是必须的，容器内执行 `usbip attach` 需要操作内核 vhci-hcd 模块。

### 桥接网络模式

桥接模式下容器 IP 与宿主机不同，`AUTO_MOUNT` 需要显式指定宿主机的 Docker 网关地址（通常为 `172.17.0.1`）并映射 USB/IP 端口：

```bash
docker run -d \
  --name fnos-remote-ups \
  --privileged \
  -p 3240:3240 \
  --restart unless-stopped \
  -e REMOTE_UPS="myups@192.168.1.100" \
  -e AUTO_MOUNT="172.17.0.1:3240@1-1" \
  fnos-remote-ups
```

> `AUTO_MOUNT` 的值格式为 `host:usbip_port@bus-id`，此处 `172.17.0.1` 是宿主机在 Docker 桥接网络中的地址，可通过 `ip route | grep docker` 确认。

### 环境变量说明

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `REMOTE_UPS` | 空（必填） | NUT UPS 标识符，格式：`ups_name@host[:port]` |
| `SERVER_PORT` | `3240` | USB/IP 服务监听端口 |
| `AUTO_MOUNT` | `true` | `true`：自动挂载到本机；`host:port@bus-id`：挂载到指定地址；空：不自动挂载 |

### 查看运行日志

```bash
docker logs -f fnos-remote-ups
```

## 在 fnOS 上的完整部署流程

### 1. 安装 usbip 工具并加载内核模块

```bash
sudo apt-get install usbip
sudo modprobe vhci-hcd

# 设置开机自动加载
echo "vhci-hcd" | sudo tee -a /etc/modules
```

### 2. 启动本工具

```bash
# 使用 -m 参数，程序启动后自动完成 usbip attach
./fnos-remote-ups -u myups@192.168.1.100 -m
```

或手动分两步操作：

```bash
# 步骤一：后台启动本工具
./fnos-remote-ups -u myups@192.168.1.100 &

# 步骤二：挂载虚拟 USB 设备到本机
usbip attach --remote 127.0.0.1 --busid 1-1
```

### 3. 验证 fnOS 识别 UPS

挂载成功后，fnOS 系统的 UPS 管理页面应能检测到新接入的 USB UPS 设备。可通过以下命令确认设备已出现：

```bash
lsusb | grep 0764:0501
```

## 程序架构

```
main.cpp
 ├── usbip_server      处理 USB/IP 协议通信，模拟 USB HID UPS 设备
 ├── nut_client        连接远程 NUT 服务器，定期拉取 UPS 变量
 ├── ups_device        维护 UPS 状态缓存，响应 HID 报告请求
 ├── hid_report        将 NUT 变量转换为标准 USB HID 报告格式
 └── device_monitor    可选，自动执行 usbip attach/detach
```

### 模拟的 USB 设备信息

| 属性 | 值 |
|------|-----|
| 厂商 ID | 0x0764 (CyberPower) |
| 产品 ID | 0x0501 |
| USB 速度 | Full-speed |
| 总线 ID | 1-1 |
| USB/IP 默认端口 | 3240 |

### 同步的 UPS 状态参数

从 NUT 获取并映射到 HID 报告的字段：

- 输入/输出电压、电池电压
- 电池电量百分比、负载百分比
- 剩余运行时间、输入频率
- 电源状态标志（AC 在线、充电中、放电中）
- 警告标志（低电量、过载）

## 故障排除

**无法连接到 NUT 服务器**

```bash
# 确认 NUT 服务可访问
upsc myups@192.168.1.100
telnet 192.168.1.100 3493
```

**usbip attach 失败**

```bash
# 确认本工具正在运行且端口可访问
usbip list --remote 127.0.0.1

# 确认 vhci-hcd 模块已加载
lsmod | grep vhci
```

**防火墙端口**

```bash
# 确保 USB/IP 端口放通
sudo ufw allow 3240
```

**启用调试输出**

```bash
./fnos-remote-ups -u myups@192.168.1.100 -d
```

## 许可证

本项目采用 MIT 许可证。
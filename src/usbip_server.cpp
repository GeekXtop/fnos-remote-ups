#include <iostream>
#include <cstring>
#include <vector>
#include <chrono>
#include <atomic>
#include <memory>
#include <map>
#include <ctime>
#include <algorithm>
#include <string>

#include "usbip_server.h"
#include "usb_structures.h"
#include "ups_device.h"

// 引入调试宏
extern bool g_debug;
#define DEBUG_PRINT(fmt, ...) \
    do { if (g_debug) fprintf(stderr, fmt, ##__VA_ARGS__); } while(0)

#define RED "\033[0;31m"
#define GREEN "\033[0;32m"
#define YELLOW "\033[1;33m"
#define NC "\033[0m" // No Color

// 缓冲区大小
#define BUFFER_SIZE 65536

// Bug3 fix: uv_write 是异步的，需要把数据拷贝到堆上，回调里再释放
struct WriteReq {
    uv_write_t req;
    char* data;
};

void on_free_handler(uv_handle_t* handle) {
    delete handle;
}

// USB/IP 服务器实现
USBIPServer::USBIPServer(uv_loop_t* loop,
                         const std::string& ups_identifier,
                         const std::string& manufacturer,
                         const std::string& product,
                         uint16_t vendor_id,
                         uint16_t product_id)
    : loop(loop),
      running(false),
      ups_device(ups_identifier, manufacturer, product, vendor_id, product_id),
      ups_identifier(ups_identifier) {
    if (!loop) {
        DEBUG_PRINT("Error: loop cannot be null\n");
    }
    global_devinfo.idVendor = htons(vendor_id);
    global_devinfo.idProduct = htons(product_id);
}

USBIPServer::~USBIPServer() {
    stop();
}

bool USBIPServer::start(int port) {
    if (!loop) {
        DEBUG_PRINT("Error: event loop is null\n");
        return false;
    }

    // 初始化 TCP 服务器句柄
    int r = uv_tcp_init(loop, &server_handle);
    if (r < 0) {
        DEBUG_PRINT("Failed to initialize TCP server: %s\n", uv_strerror(r));
        return false;
    }
    server_handle.data = this;  // Bug1 fix: on_connection reads server->data to get USBIPServer*

    // 绑定地址
    r = uv_ip4_addr("0.0.0.0", port, &bind_addr);
    if (r < 0) {
        DEBUG_PRINT("Failed to parse IP address: %s\n", uv_strerror(r));
        return false;
    }

    r = uv_tcp_bind(&server_handle, (const struct sockaddr*)&bind_addr, 0);
    if (r < 0) {
        DEBUG_PRINT("Failed to bind socket: %s\n", uv_strerror(r));
        return false;
    }

    // 开始监听
    r = uv_listen((uv_stream_t*)&server_handle, 128, on_connection);
    if (r < 0) {
        DEBUG_PRINT("Failed to listen on socket: %s\n", uv_strerror(r));
        return false;
    }

    running = true;
    ups_device.start();

    DEBUG_PRINT("USB/IP UPS Server started on port %d (libuv event-driven)\n", port);
    return true;
}

void USBIPServer::stop() {
    if (!running) return;

    running = false;
    ups_device.stop();

    // 先收集所有客户端，再清空 map，避免 remove_client 内 erase 导致迭代器失效
    std::vector<ClientContext*> to_close;
    to_close.reserve(clients.size());
    for (auto& pair : clients) {
        to_close.push_back(pair.second);
    }
    for (auto* ctx : to_close) {
        remove_client(ctx);
    }

    // 关闭服务器监听
    uv_close((uv_handle_t*)&server_handle, nullptr);
}

void USBIPServer::add_client(ClientContext* ctx) {
    clients[&ctx->handle] = ctx;
    DEBUG_PRINT("Client added, total clients: %zu\n", clients.size());
}

void USBIPServer::remove_client(ClientContext* ctx) {
    if (ctx) {
        DEBUG_PRINT("Client removed\n");
        clients.erase(&ctx->handle);
        uv_close((uv_handle_t*)&ctx->handle, on_client_close);
    }
}

// 静态回调：新连接
void USBIPServer::on_connection(uv_stream_t* server, int status) {
    if (status < 0) {
        DEBUG_PRINT("Connection error: %s\n", uv_strerror(status));
        return;
    }

    USBIPServer* self = reinterpret_cast<USBIPServer*>(server->data);

    // 创建新的客户端上下文
    ClientContext* ctx = new ClientContext();
    ctx->server = self;
    ctx->state = ClientState::WAITING_COMMAND;

    int r = uv_tcp_init(self->loop, &ctx->handle);
    if (r < 0) {
        DEBUG_PRINT("Failed to initialize client handle: %s\n", uv_strerror(r));
        delete ctx;
        return;
    }

    // 设置数据指针
    ctx->handle.data = ctx;

    // 接受连接
    r = uv_accept(server, (uv_stream_t*)&ctx->handle);
    if (r < 0) {
        DEBUG_PRINT("Failed to accept connection: %s\n", uv_strerror(r));
        uv_close((uv_handle_t*)&ctx->handle, on_client_close);
        return;
    }

    // 获取客户端地址信息
    struct sockaddr_in addr;
    int namelen = sizeof(addr);
    if (uv_tcp_getpeername(&ctx->handle, (struct sockaddr*)&addr, &namelen) == 0) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        ctx->client_ip = ip;
        printf("Client connected from %s\n", ip);
    }

    // 添加到客户端列表
    self->add_client(ctx);

    // 开始读取数据
    uv_read_start((uv_stream_t*)&ctx->handle, on_alloc_buffer, on_client_read);
}

// 静态回调：分配缓冲区
void USBIPServer::on_alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)handle;
    // 按 libuv 请求的大小动态分配内存
    char* slab = (char*)malloc(suggested_size);
    buf->base = slab;
    buf->len = suggested_size;
}

// 静态回调：读取数据
void USBIPServer::on_client_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
    ClientContext* ctx = reinterpret_cast<ClientContext*>(client->data);
    USBIPServer* self = ctx->server;

    if (nread < 0) {
        // 错误或连接关闭
        if (nread != UV_EOF) {
            DEBUG_PRINT("Read error: %s\n", uv_err_name(nread));
        }

        // 释放缓冲区
        free(buf->base);

        // 关闭连接
        self->remove_client(ctx);
        return;
    }

    if (nread == 0) {
        // 空读，释放缓冲区
        free(buf->base);
        return;
    }

    // 将新数据追加到接收缓存
    ctx->recv_buffer.insert(ctx->recv_buffer.end(), buf->base, buf->base + nread);

    // 释放 libuv 分配的缓冲区
    free(buf->base);

    // 循环处理缓存中的数据，直到数据耗尽或需要等待更多数据
    while (!ctx->recv_buffer.empty()) {
        ssize_t consumed = self->handle_client_data(ctx, ctx->recv_buffer.data(), ctx->recv_buffer.size());

        if (consumed <= 0) {
            // 数据不完整或需要等待更多数据
            break;
        }

        // 移除已处理的数据
        if (static_cast<size_t>(consumed) < ctx->recv_buffer.size()) {
            ctx->recv_buffer.erase(ctx->recv_buffer.begin(), ctx->recv_buffer.begin() + consumed);
        } else {
            // 所有数据都已处理
            ctx->recv_buffer.clear();
        }
    }
    // 如果缓存为空，等待下一次数据；如果缓存有剩余，说明数据不完整等待补充
}

// 静态回调：写入完成
void USBIPServer::on_client_write(uv_write_t* req, int status) {
    if (status < 0) {
        DEBUG_PRINT("Write error: %s\n", uv_strerror(status));
    }
    WriteReq* wr = reinterpret_cast<WriteReq*>(req);
    free(wr->data);
    delete wr;
}

// 静态回调：关闭连接
void USBIPServer::on_client_close(uv_handle_t* handle) {
    ClientContext* ctx = reinterpret_cast<ClientContext*>(handle->data);
    if (ctx) {
        // 标记客户端已断开，定时器回调时会检查
        ctx->handle.data = nullptr;

        // 取消所有pending的中断定时器
        if (ctx->server) {
            ctx->server->cancel_all_interrupt_timers(ctx);
        }

        if (!ctx->client_ip.empty()) {
            DEBUG_PRINT("Client connection closed from %s\n", ctx->client_ip.c_str());
            printf("Client disconnected from %s\n", ctx->client_ip.c_str());
        } else {
            DEBUG_PRINT("Client connection closed\n");
        }

        delete ctx;
    }
}

// 实例方法：处理客户端数据
ssize_t USBIPServer::handle_client_data(ClientContext* ctx, const char* data, ssize_t nread) {
    switch (ctx->state) {
        case ClientState::WAITING_COMMAND: {
            // 读取 USB/IP 头部
            if (nread < static_cast<ssize_t>(sizeof(usbip_message_header))) {
                DEBUG_PRINT("Incomplete header received: %zd bytes, waiting for more data\n", nread);
                return 0;  // 数据不完整，等待更多数据
            }

            struct usbip_message_header usbip_basic;
            memcpy(&usbip_basic, data, sizeof(usbip_basic));

            // 转换字节序
            uint16_t version = ntohs(usbip_basic.version);
            uint16_t command = ntohs(usbip_basic.command);

            DEBUG_PRINT("Received USB/IP packet: version=0x%x, command=0x%x\n", version, command);

            // 检查协议版本
            if (version != USBIP_VERSION) {
                DEBUG_PRINT("Unsupported USB/IP version: 0x%x from %s\n", version, ctx->client_ip.c_str());
                ctx->server->remove_client(ctx);
                return nread;  // 消耗所有数据
            }

            switch (command) {
                case OP_REQ_DEVLIST:
                    DEBUG_PRINT("Processing OP_REQ_DEVLIST\n");
                    if (handle_devlist_request(ctx) < 0) {
                        DEBUG_PRINT("Failed to handle devlist request\n");
                        ctx->server->remove_client(ctx);
                        return nread;
                    }
                    // 设备列表请求处理完成，消耗了头部数据
                    return sizeof(usbip_message_header);

                case OP_REQ_IMPORT: {
                    // 需要接收 busid (32 字节)
                    ssize_t total_size = sizeof(usbip_message_header) + 32;
                    if (nread < total_size) {
                        DEBUG_PRINT("Incomplete import request: %zd bytes, need %zd bytes\n", nread, total_size);
                        return 0;  // 数据不完整，等待更多数据
                    }
                    char busid[32];
                    memcpy(busid, data + sizeof(usbip_message_header), 32);
                    DEBUG_PRINT("Import request for busid: %s\n", busid);

                    if (handle_import_request(ctx, busid) < 0) {
                        DEBUG_PRINT("Failed to handle import request\n");
                        ctx->server->remove_client(ctx);
                        return nread;
                    }

                    // 导入成功后，状态变为 IMPORTED，等待 USB 命令
                    ctx->state = ClientState::IMPORTED;
                    return total_size;  // 消耗了头部 + busid
                }

                default:
                    DEBUG_PRINT("Unknown command: 0x%x from %s\n", command, ctx->client_ip.c_str());
                    ctx->server->remove_client(ctx);
                    return nread;
            }
        }

        case ClientState::IMPORTED: {
            // 处理 USB 命令
            if (nread < static_cast<ssize_t>(sizeof(union usbip_header))) {
                DEBUG_PRINT("Incomplete USB command header: %zd bytes, waiting for more data\n", nread);
                return 0;  // 数据不完整，等待更多数据
            }

            union usbip_header header;
            memcpy(&header, data, sizeof(header));

            // 转换字节序
            union usbip_header hdr = header;
            hdr.base.command = ntohl(hdr.base.command);

            // 计算此命令的总长度（头部 + 数据）
            ssize_t total_size = sizeof(union usbip_header);
            if (hdr.base.command == USBIP_CMD_SUBMIT && hdr.base.direction == 0) {
                // OUT 方向，有数据要发送
                hdr.cmd_submit.cmd_submit.transfer_buffer_length = ntohl(hdr.cmd_submit.cmd_submit.transfer_buffer_length);
                total_size += hdr.cmd_submit.cmd_submit.transfer_buffer_length;
            }

            if (nread < total_size) {
                DEBUG_PRINT("Incomplete USB command data: %zd bytes, need %zd bytes\n", nread, total_size);
                return 0;  // 数据不完整，等待更多数据
            }

            if (handle_usb_command(ctx, &header) < 0) {
                DEBUG_PRINT("Failed to handle USB command\n");
                ctx->server->remove_client(ctx);
                return nread;
            }

            return total_size;  // 消耗了完整的命令数据
        }

        default:
            DEBUG_PRINT("Unknown client state: %d\n", (int)ctx->state);
            ctx->server->remove_client(ctx);
            return nread;
    }
}

int USBIPServer::handle_devlist_request(ClientContext* ctx) {
    DEBUG_PRINT("Handling device list request\n");

    // 发送设备列表响应头部
    struct usbip_message_header reply_header;
    reply_header.version = htons(USBIP_VERSION);
    reply_header.command = htons(OP_REP_DEVLIST);
    reply_header.status = htonl(0);  // 成功

    // 设备数量
    uint32_t ndev = htonl(1);

    // 构建缓冲区数组
    ResponseData buffers[4] = {
        { reinterpret_cast<const char*>(&reply_header), sizeof(reply_header) },
        { reinterpret_cast<const char*>(&ndev), sizeof(ndev) },
        { reinterpret_cast<const char*>(&global_devinfo), sizeof(global_devinfo) },
        { reinterpret_cast<const char*>(&global_intf), sizeof(global_intf) }
    };

    // 发送响应
    send_response(ctx, buffers, 4);

    DEBUG_PRINT("Device list response sent successfully\n");
    return 0;
}

int USBIPServer::handle_import_request(ClientContext* ctx, const char* busid) {
    DEBUG_PRINT("Handling import request for busid: %s\n", busid);

    // 检查 busid
    if (strcmp(busid, "1-1") != 0) {
        DEBUG_PRINT("Unknown busid: %s\n", busid);

        struct usbip_message_header reply_header;
        reply_header.version = htons(USBIP_VERSION);
        reply_header.command = htons(OP_REP_IMPORT);
        reply_header.status = htonl(1);  // 错误

        // 构建缓冲区数组
        ResponseData buffers[] = {
            { reinterpret_cast<const char*>(&reply_header), sizeof(reply_header) }
        };
        // 发送响应
        send_response(ctx, buffers, 1);
        return -1;
    }

    // 发送导入响应
    struct usbip_message_header reply_header;
    reply_header.version = htons(USBIP_VERSION);
    reply_header.command = htons(OP_REP_IMPORT);
    reply_header.status = htonl(0);  // 成功

    // 构建缓冲区数组
    ResponseData buffers[] = {
        { reinterpret_cast<const char*>(&reply_header), sizeof(reply_header) },
        { reinterpret_cast<const char*>(&global_devinfo), sizeof(global_devinfo) }
    };
    // 发送响应
    send_response(ctx, buffers, 2);

    DEBUG_PRINT("Device imported successfully\n");
    return 0;
}

int USBIPServer::handle_usb_command(ClientContext* ctx, const union usbip_header* header) {
    DEBUG_PRINT("Processing USB command\n");

    // 转换字节序
    union usbip_header hdr = *header;
    hdr.base.command = ntohl(hdr.base.command);
    hdr.base.seqnum = ntohl(hdr.base.seqnum);
    hdr.base.devid = ntohl(hdr.base.devid);
    hdr.base.direction = ntohl(hdr.base.direction);
    hdr.base.ep = ntohl(hdr.base.ep);

    DEBUG_PRINT(GREEN "USB command: 0x%x, devid=0x%x, seq=%u, ep=%d, direction=%d" NC "\n",
                hdr.base.command, hdr.base.devid, hdr.base.seqnum, hdr.base.ep, hdr.base.direction);

    // 准备响应
    union usbip_header response;
    uint8_t response_data[4096];
    int response_length = 0;
    memset(&response, 0, sizeof(response));
    response.base.seqnum = htonl(hdr.base.seqnum);

    switch (hdr.base.command) {
        case USBIP_CMD_SUBMIT: {
            // 转换提交参数
            hdr.cmd_submit.cmd_submit.transfer_flags = ntohl(hdr.cmd_submit.cmd_submit.transfer_flags);
            hdr.cmd_submit.cmd_submit.transfer_buffer_length = ntohl(hdr.cmd_submit.cmd_submit.transfer_buffer_length);
            hdr.cmd_submit.cmd_submit.start_frame = ntohl(hdr.cmd_submit.cmd_submit.start_frame);
            hdr.cmd_submit.cmd_submit.number_of_packets = ntohl(hdr.cmd_submit.cmd_submit.number_of_packets);
            hdr.cmd_submit.cmd_submit.interval = ntohl(hdr.cmd_submit.cmd_submit.interval);

            // 创建 setup 包结构体
            struct usb_setup_packet setup_packet;
            setup_packet.bmRequestType = hdr.cmd_submit.cmd_submit.setup[0];
            setup_packet.bRequest = hdr.cmd_submit.cmd_submit.setup[1];
            setup_packet.wValue = (hdr.cmd_submit.cmd_submit.setup[3] << 8) | hdr.cmd_submit.cmd_submit.setup[2];
            setup_packet.wIndex = (hdr.cmd_submit.cmd_submit.setup[5] << 8) | hdr.cmd_submit.cmd_submit.setup[4];
            setup_packet.wLength = (hdr.cmd_submit.cmd_submit.setup[7] << 8) | hdr.cmd_submit.cmd_submit.setup[6];

            DEBUG_PRINT("Transfer flags: 0x%x, buffer length: %d, interval: %d\n",
                        hdr.cmd_submit.cmd_submit.transfer_flags,
                        hdr.cmd_submit.cmd_submit.transfer_buffer_length,
                        hdr.cmd_submit.cmd_submit.interval);

            DEBUG_PRINT(YELLOW "Setup packet: type=0x%x, request=0x%x, value=0x%x, index=0x%x, length=%d" NC "\n",
                        setup_packet.bmRequestType, setup_packet.bRequest,
                        setup_packet.wValue, setup_packet.wIndex, setup_packet.wLength);

            if (hdr.base.ep == 0) {  // 控制端点
                // 处理控制请求
                response_length = ups_device.handle_control_request(&setup_packet, response_data);
                DEBUG_PRINT("Control request response length: %d\n", response_length);

            } else if (hdr.base.ep == 1 || hdr.base.ep == 0x81) {  // 中断端点
                // 中断请求暂不返回，等待 500ms 后返回 NAK 响应
                DEBUG_PRINT("Interrupt request received, scheduling NAK response after 500ms (seqnum=%u)\n", hdr.base.seqnum);

                // 创建定时器
                uv_timer_t* timer = new uv_timer_t;
                uv_timer_init(loop, timer);

                // 将定时器与 seqnum 关联，存储到 ctx 中
                ctx->interrupt_timers[hdr.base.seqnum] = timer;

                // 定时器数据保存 ctx 和 seqnum
                timer->data = new TimerData{ctx, hdr.base.seqnum};

                // 500ms 后触发响应
                uv_timer_start(timer, on_interrupt_timer, 500, 0);

                return 0;
            } else if (hdr.base.ep == 2 || hdr.base.ep == 0x02) {
                DEBUG_PRINT("Interrupt OUT request received, acknowledging with no data\n");
                response_length = 0;
            } else {
                DEBUG_PRINT("Unknown endpoint: %d\n", hdr.base.ep);
                response_length = 0;
            }

            // 确保响应数据长度正确
            if (response_length < 0) {
                response_length = 0;
            } else if (response_length > setup_packet.wLength) {
                DEBUG_PRINT("Response length exceeds transfer buffer length %d > %d\n",
                            response_length, setup_packet.wLength);
                response_length = setup_packet.wLength;
            }

            // 发送响应头部
            response.base.command = htonl(USBIP_RET_SUBMIT);
            response.ret_submit.ret_submit.actual_length = htonl(response_length);
            response.ret_submit.ret_submit.start_frame = htonl(hdr.cmd_submit.cmd_submit.start_frame);

            // 构建缓冲区数组
            if (response_length > 0) {
                // 构建缓冲区数组
                ResponseData buffers[] = {
                    { reinterpret_cast<const char*>(&response), sizeof(response) },
                    { reinterpret_cast<const char*>(response_data), (size_t)response_length }
                };
                send_response(ctx, buffers, 2);
            } else {
                // 构建缓冲区数组
                ResponseData buffers[] = {
                    { reinterpret_cast<const char*>(&response), sizeof(response) }
                };
                send_response(ctx, buffers, 1);
            }
            break;
        }

        case USBIP_CMD_UNLINK: {
            DEBUG_PRINT("Processing USBIP_CMD_UNLINK\n");
            uint32_t unlink_seqnum = ntohl(hdr.cmd_unlink.data.seqnum);

            // 检查客户端上下文中的中断请求定时器是否已执行
            auto it = ctx->interrupt_timers.find(unlink_seqnum);
            if (it != ctx->interrupt_timers.end()) {
                // 定时器仍在等待中，取消它
                DEBUG_PRINT("Found pending interrupt timer for seqnum %u, cancelling it\n", unlink_seqnum);
                cancel_interrupt_timer(ctx, unlink_seqnum);

                // 返回中断响应状态，表示请求已被取消
                response.ret_unlink.data.status = htonl(-ECONNRESET);  // 消息已取消, 返回 RESET
            } else {
                // 定时器已执行或不存在，返回UNLINK响应
                DEBUG_PRINT("No pending interrupt timer for seqnum %u, sending UNLINK response\n", unlink_seqnum);

                response.ret_unlink.data.status = htonl(0);  // 消息已响应, 返回成功
            }

            response.base.command = htonl(USBIP_RET_UNLINK);
            // 构建缓冲区数组
            ResponseData buffers[] = {
                { reinterpret_cast<const char*>(&response), sizeof(response) }
            };
            send_response(ctx, buffers, 1);
            break;
        }

        default:
            DEBUG_PRINT("Unknown USB command: 0x%x\n", hdr.base.command);
            response.ret_submit.ret_submit.status = htonl(1);
            // 构建缓冲区数组
            ResponseData buffers[] = {
                { reinterpret_cast<const char*>(&response), sizeof(response) }
            };
            send_response(ctx, buffers, 1);
            break;
    }

    return 0;
}

void USBIPServer::send_response(ClientContext* ctx, const ResponseData* buffers, size_t count) {
    if (count == 0) return;

    // Bug3 fix: 把所有分散的缓冲区合并到一块堆内存，异步写完后在 on_client_write 里释放
    size_t total = 0;
    for (size_t i = 0; i < count; i++) total += buffers[i].len;

    char* data = static_cast<char*>(malloc(total));
    if (!data) {
        DEBUG_PRINT("Failed to allocate write buffer\n");
        return;
    }
    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        memcpy(data + offset, buffers[i].buf, buffers[i].len);
        offset += buffers[i].len;
    }

    WriteReq* req = new WriteReq();
    req->data = data;
    uv_buf_t buf = uv_buf_init(data, total);

    int r = uv_write(&req->req, (uv_stream_t*)&ctx->handle, &buf, 1, on_client_write);
    if (r < 0) {
        DEBUG_PRINT("Failed to send response: %s\n", uv_strerror(r));
        free(data);
        delete req;
    }
}

// 定时器回调：中断请求超时，发送 NAK 响应
void USBIPServer::on_interrupt_timer(uv_timer_t* timer) {

    TimerData* data = reinterpret_cast<TimerData*>(timer->data);
    timer->data = nullptr;
    uv_close(reinterpret_cast<uv_handle_t*>(timer), on_free_handler);

    if (!data) {
        return;
    }

    ClientContext* ctx = data->ctx;
    uint32_t seqnum = data->seqnum;

    // 从映射中移除定时器
    ctx->interrupt_timers.erase(seqnum);

    // 检查客户端是否已断开
    if (ctx->handle.data == nullptr) {
        DEBUG_PRINT("Client disconnected, skipping interrupt NAK response (seqnum=%u)\n", seqnum);
        delete data;
        return;
    }

    // 构建 NAK 响应
    union usbip_header response;
    memset(&response, 0, sizeof(response));
    response.base.seqnum = htonl(seqnum);
    response.base.command = htonl(USBIP_RET_SUBMIT);

    // 构建缓冲区数组
    ResponseData buffers[] = {
        { reinterpret_cast<const char*>(&response), sizeof(response) }
    };
    ctx->server->send_response(ctx, buffers, 1);

    DEBUG_PRINT("Interrupt NAK response sent (seqnum=%u) [ip:%s]\n", seqnum, ctx->client_ip.c_str());

    delete data;
}

// 取消指定的中断定时器
void USBIPServer::cancel_interrupt_timer(ClientContext* ctx, uint32_t seqnum) {
    auto it = ctx->interrupt_timers.find(seqnum);
    if (it != ctx->interrupt_timers.end()) {
        uv_timer_t* timer = it->second;

        // 停止定时器
        uv_timer_stop(timer);

        // 清理定时器数据
        TimerData* data = reinterpret_cast<TimerData*>(timer->data);
        delete data;

        // 关闭定时器句柄（异步）
        uv_close(reinterpret_cast<uv_handle_t*>(timer), on_free_handler);

        // 从映射中移除
        ctx->interrupt_timers.erase(it);

        DEBUG_PRINT("Interrupt timer cancelled (seqnum=%u)\n", seqnum);
    }
}

// 取消所有中断定时器
void USBIPServer::cancel_all_interrupt_timers(ClientContext* ctx) {
    for (auto& pair : ctx->interrupt_timers) {
        uv_timer_t* timer = pair.second;

        // 停止定时器
        uv_timer_stop(timer);

        // 清理定时器数据
        TimerData* data = reinterpret_cast<TimerData*>(timer->data);
        delete data;

        // 关闭定时器句柄（异步）
        uv_close(reinterpret_cast<uv_handle_t*>(timer), on_free_handler);
    }
    ctx->interrupt_timers.clear();
    DEBUG_PRINT("All interrupt timers cancelled for client\n");
}

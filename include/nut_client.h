#ifndef NUT_CLIENT_H
#define NUT_CLIENT_H

#include <string>
#include <map>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <netdb.h>

class NUTClient {
private:
    std::string server_host;
    int server_port;
    std::string ups_name;
    int socket_fd;
    bool connected;

public:
    NUTClient(const std::string& ups_identifier);
    ~NUTClient();

    bool connect();
    void disconnect();

    // 发送命令并接收响应
    std::string send_command(const std::string& command);

    // 获取UPS变量列表
    std::map<std::string, std::string> get_ups_vars();

    // 获取特定UPS变量
    std::string get_ups_var(const std::string& var_name);

    // 检查响应是否为错误
    static bool is_error_response(const std::string& response);

    // 检查错误返回并打印错误信息到控制台
    static void check_and_print_error(const std::string& response);

    // 检查是否连接
    bool is_connected() const;

    // 获取服务器信息
    std::string get_server_host() const;
    int get_server_port() const;
    std::string get_ups_name() const;
};

#endif // NUT_CLIENT_H
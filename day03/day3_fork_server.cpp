#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <sys/wait.h>
using  namespace std;

void handle_client(int fd) {
    //子进程处理函数
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) return;

    // 构造合法 HTTP 响应
    const char *response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    write(fd, response, strlen(response));

    // //子进程处理函数
    // char buf[1024];
    // memset(buf, 0, sizeof(buf));

    // ssize_t n = 0;
    // while((n = read(fd, buf, sizeof(buf))) > 0) {
    //     //客户端在终端上写 -> 服务端read
    //     //read到把read的内容write回服务端
    //     write(fd, buf, n);  // 修正：应该是write(fd, buf, n)，而不是sizeof(buf)
    //     // 这样才能只写入实际读取的字节数
    // }
    // // 当read返回0时，表示客户端关闭连接
    // // 当read返回-1时，表示读取出错
}

//最小阻塞 echo-server
//客户端输入 -> 服务端read -> 服务端write回客户端
//socket -> bind -> listen -> accept -> read -> write -> close
int main(int argc, char const *argv[])
{
    // 创建监听套接字
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);  //socket是用来创建什么的  
    // Answer:创建 监听套接字（文件描述符），后续 bind/listen/accept 都围绕它
    // AF_INET: IPv4地址族
    // SOCK_STREAM: 流式套接字(TCP)
    // 0: 使用默认协议(TCP)
    if(listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));  
    //这句话是干什么的  
    //Answer:设置SO_REUSEADDR选项，允许端口在服务器重启后可立即复用，避免TIME_WAIT状态导致"Address already in use"错误
    // SOL_SOCKET: 套接字层级的选项
    // SO_REUSEADDR: 允许重用本地地址和端口

    //服务端地址结构
    sockaddr_in addr{};  
    // 声明 sockaddr_in 服务端对象  
    // sockaddr_in 是 IPv4 专用地址结构；sockaddr 是通用抽象结构，传参时需要强制转换
    addr.sin_family = AF_INET;  // ipv4
    addr.sin_port = htons(8080);  
    // 设置端口号，htons将主机字节序(通常是小端)转换为网络字节序(大端)
    addr.sin_addr.s_addr = INADDR_ANY;  
    // 这个不知道是啥东西  
    // Answer:INADDR_ANY = 0.0.0.0，表示监听本机所有网卡，外部任意IP都能连进来

    //绑定套接字到地址
    if(bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    //进入listen状态，开始监听连接请求
    // 5是backlog参数，表示全连接队列的最大长度
    if((listen(listen_fd, 5)) < 0) {
        perror("listen");
        return 1;
    }

    cout << "Blocking echo server listen on :8080\n" << endl;

    //主循环，接受客户端连接
    while(true) {
        sockaddr_in client{};  // 客户端地址结构
        socklen_t len = sizeof(client);

        int conn_fd = accept(listen_fd, (sockaddr*)&client, &len);  
        //accept是什么功能
        //Answer:accept 从全连接队列里取出一个已完成三次握手的连接，
        // 返回一个新的已连接套接字 conn_fd；后续读写都用它
        // 这是一个阻塞调用，如果没有连接请求，会一直等待
        
        if(conn_fd < 0) {
            perror("accept");
            continue;  // 接受失败，继续等待下一个连接
        }

        // 打印客户端信息（可选）
        // char client_ip[INET_ADDRSTRLEN];
        // inet_ntop(AF_INET, &client.sin_addr, client_ip, sizeof(client_ip));
        // cout << "Client connected from " << client_ip << ":" << ntohs(client.sin_port) << endl;

        // 创建子进程来处理客户端连接
        pid_t pid = fork();
        if(pid == 0) {
            //子进程
            close(listen_fd);  //子进程不需要监听套接字
            handle_client(conn_fd);  //处理客户端请求
            close(conn_fd);  //处理完成后关闭连接套接字
            exit(0);  //子进程退出
        }
        else if(pid > 0) {
            //父进程
            close(conn_fd);  //父进程不需要已连接套接字
            //非阻塞回收僵尸进程
            // WNOHANG: 如果没有子进程结束立即返回，不阻塞
            // -1: 等待任意子进程
            while(waitpid(-1, nullptr, WNOHANG) > 0){}
        }
        else {
            // fork失败
            perror("fork");
            close(conn_fd);
        }
    }

    close(listen_fd);  // 实际上这行代码不会被执行到，因为上面是无限循环

    return 0;
}
#include "thread_pool.h"

// void handle_client(int conn_fd) {
//     //子进程处理函数
//     char buf[1024];
//     ssize_t n;
    
//     //获取用户端信息
//     sockaddr_in client_addr;
//     socklen_t len = sizeof(client_addr);
//     getpeername(conn_fd, (sockaddr*)&client_addr, &len);
//     char client_ip[INET_ADDRSTRLEN];
//     inet_ntop(AF_INET, &client_addr.sin_addr.s_addr, client_ip, sizeof(client_ip));

//     cout << "Thread" << this_thread::get_id() << "handling client" << client_ip << ":" << ntohs(client_addr.sin_port) << endl;
    
//     // // 改进的回显逻辑
//     // while(true) {
//     //     memset(buf, 0, sizeof(buf));
        
//     //     // 读取客户端数据
//     //     ssize_t n = read(conn_fd, buf, sizeof(buf) - 1);  // 留一个位置给'\0'
        
//     //     if(n > 0) {
//     //         buf[n] = '\0';  // 确保字符串以null结尾
            
//     //         // 打印接收到的内容
//     //         cout << "Received from " << client_ip << ": " << buf;
            
//     //         // 确保完全写入（处理部分写入的情况）
//     //         ssize_t total_written = 0;
//     //         while(total_written < n) {
//     //             ssize_t written = write(conn_fd, buf + total_written, n - total_written);
//     //             if(written <= 0) {
//     //                 perror("write");
//     //                 break;
//     //             }
//     //             total_written += written;
//     //         }
            
//     //         if(total_written == n) {
//     //             cout << "Echoed back " << n << " bytes" << endl;
//     //         }
//     //     } 
//     //     else if(n == 0) {
//     //         // 客户端关闭连接
//     //         cout << "Client " << client_ip << " disconnected" << endl;
//     //         break;
//     //     } 
//     //     else {
//     //         // 读取错误
//     //         perror("read");
//     //         break;
//     //     }
//     // }

//     // 读取HTTP请求
//     n = read(conn_fd, buf, sizeof(buf) - 1);
//     if(n > 0) {
//         buf[n] = '\0';
        
//         // 打印请求信息
//         cout << "HTTP Request from " << client_ip << ":" << endl;
//         cout << "=================================" << endl;
//         cout << buf;
//         cout << "=================================" << endl;
        
//         // 简单的HTTP响应
//         const char* response = 
//             "HTTP/1.1 200 OK\r\n"
//             "Content-Type: text/html; charset=utf-8\r\n"
//             "Connection: keep-alive\r\n"
//             "Content-Length: 48\r\n"
//             "\r\n"
//             "<html><body><h1>Hello from ThreadPool Server!</h1></body></html>";
        
//         // 发送HTTP响应
//         write(conn_fd, response, strlen(response));
        
//         cout << "Sent HTTP response to " << client_ip << endl;
//     }
//     close(conn_fd);
    
// }

void handle_client(int conn_fd) {
    char buf[1024];
    ssize_t n = read(conn_fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        // 回显 HTTP
        const char* response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        write(conn_fd, response, strlen(response));
    }
    close(conn_fd);
}

//最小阻塞 echo-server
//客户端输入 -> 服务端read -> 服务端write回客户端
//socket -> bind -> listen -> accept -> read -> write -> close
int main(int argc, char const *argv[])
{
    //创建线程池，4个工作线程
    ThreadPool pool(4);

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
    if((listen(listen_fd, 128)) < 0) {
        perror("listen");
        return 1;
    }

    cout << "ThreadPool echo server listening on :8080" << endl;
    cout << "Thread pool with 4 workers ready." << endl;

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

        //获取客户端信息
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, client_ip, sizeof(client_ip));
        cout << "New connect from" << client_ip << ":" << ntohs(client.sin_port) << endl;

        //客户端处理任务提交到线程池
        //使用lambda捕获conn_fd并传递给handle_client
        pool.enqueue([conn_fd](){
            handle_client(conn_fd);
        });

        // close(conn_fd);
        
    }

    close(listen_fd);

    return 0;
}
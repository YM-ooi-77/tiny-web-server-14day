#include "epoll_lt.h"

#define BUF_SIZE 1024
unordered_map<int, string> write_buffers;  //fd->代写数据

void handle_client(Epoll& epoll, int client_fd, uint32_t events) {
    //cout << "[DEBUG] handle_client fd=" << client_fd << " events=" << events << " EPOLLIN=" << (events & EPOLLIN) << endl << flush;

    //1.检查错误事件
    if(events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        //cout << "[DEBUG] error event on fd=" << client_fd << endl;
        close_client(epoll, client_fd);
        return;
    }

    //2.处理可读事件
    if(events & EPOLLIN) {
        if(!handle_read(epoll, client_fd)) {
            // 读取失败或对方关闭连接
            //cout << "[DEBUG] handle_read failed for fd=" << client_fd << endl;
            close_client(epoll, client_fd);
            return;
        }
        // 成功处理，保持连接
        //cout << "[DEBUG] handle_read succeeded for fd=" << client_fd << endl;
    }

    //3. 处理可写事件
    if(events & EPOLLOUT) {
        handle_write(epoll, client_fd);
    }
}

void close_client(Epoll& epoll, int client_fd) {
    // 1. 从 epoll 中删除
    epoll.del(client_fd);
    
    // 2. 关闭文件描述符
    close(client_fd);
    
    //3. 清理代写数据缓冲区
    write_buffers.erase(client_fd);
}
//正常的ET回显读取数据
// bool handle_read(Epoll& epoll, int client_fd) {
//     cout << "[DEBUG] handle_read fd=" << client_fd << endl;

//     //构造http响应，用来测压
//     string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!\n";
//     write(client_fd, response.c_str(), response.size());

//     char buffer[4096];
    
//     // 边缘触发：必须循环读取直到 EAGAIN
//     while(true) {
//         ssize_t n = read(client_fd, buffer, sizeof(buffer));
        
//         //用于测压，暂时注释掉回显功能
//         if(n > 0) {
//             write_buffers[client_fd].append(buffer, n);  //将buffer里的数据存到缓冲区

//             //监听可写事件
//             epoll.mod(client_fd, EPOLLIN | EPOLLOUT | EPOLLET);
//             return true;
//         } 
//         else if(n == 0) {
//             // 客户端关闭连接
//             cout << "[DEBUG] client " << client_fd << " closed connection" << endl;
//             return false;
//         } 
//         else {
//             // 错误处理
//             if(errno == EAGAIN || errno == EWOULDBLOCK) {
//                 // 所有数据都读完
//                 cout << "[DEBUG] all data read from fd=" << client_fd << endl;
//                 return true;  // 明确返回 true
//             } else {
//                 // 其他读错误
//                 cout << "[DEBUG] read error on fd=" << client_fd << ": " << strerror(errno) << endl;
//                 return false;
//             }
//         }
//     }
    
//     // 理论上不会到达这里，但为了编译器警告
//     return true;
// }

//用于wrk测压构建的http响应read
bool handle_read(Epoll& epoll, int client_fd) {
    char buffer[4096];
    int n = read(client_fd, buffer, sizeof(buffer));
    
    if (n > 0) {
        // 构造HTTP响应（关键改动！）
        string response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, World!";
        
        // 存入写缓冲
        write_buffers[client_fd].append(response);
        epoll.mod(client_fd, EPOLLIN | EPOLLOUT | EPOLLET);
    }
    else if(n == 0) {
        return false;
    } 
    else {
        // 错误处理
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;  // 明确返回 true
        }
    
        else {
            return false;
        }
    }
    return true;
}

void handle_write(Epoll& epoll, int client_fd) {
    string& buf = write_buffers[client_fd];

    while(!buf.empty()) {
        ssize_t w = write(client_fd, buf.data(), buf.size());

        if(w > 0) {
            buf.erase(0, w);  //删除已写数据
        }
        else if (w < 0 && errno == EAGAIN)
        {
            break;  //等下次EPOLLOUT
        }
        else {
            close_client(epoll, client_fd);
            return;
        }
    }
    // 如果写完了，取消EPOLLOUT监听（避免busy loop）
    if(buf.empty()) {
        epoll.mod(client_fd, EPOLLIN | EPOLLET);
    }
}

//设置非阻塞模式函数
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl F_GETFL");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl F_SETFL");
    }
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

    //如果listen_fd为阻塞，使用了 EPOLLET，新连接到来的时候：
    /*
        ET触发一次，你循环accept，循环调用accept()从listen_fd的"已完成连接队列"里取连接
        如果队列里没有更多连接，最后一次accept()会阻塞住整个线程
        导致epoll_wait无法再次调用，程序卡死
    */
    set_nonblocking(listen_fd);  //将listen_fd设置成非阻塞

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

    Epoll epoll(1024);
    // 将监听socket添加到epoll，监听读事件
    // 监听socket使用 ET 模式
    epoll.add(listen_fd, EPOLLIN | EPOLLET);

    //cout << "ThreadPool echo server listening on :8080" << endl;
    //cout << "Echo server (EPOLLET mode) listening on :8080" << endl;
    //cout << "Press Ctrl+C to stop" << endl;

    //主循环，接受客户端连接
    while(true) {
        //cout << "[DEBUG] === Waiting for epoll_wait...===" << endl << flush;
        try {
            int n = epoll.wait(); 
            //cout << "[DEBUG] epoll_wait return " << n << " events" << endl;
            
            for(int i = 0; i < n; ++i) {
                struct epoll_event& ev = epoll.get_event(i);
                int fd = ev.data.fd;
                uint32_t events = ev.events;
                
                //cout << "[DEBUG] Processing event on fd=" << fd 
                    //<< " events=" << events 
                    //<< " (listen_fd=" << listen_fd << ")" << endl;
                
                if(fd == listen_fd) {
                    //cout << "[DEBUG] Accepting on listen_fd=" << listen_fd << endl << flush;
                    // 边缘触发：必须循环 accept 直到 EAGAIN
                    while(true) {
                        sockaddr_in client_addr;
                        socklen_t addr_len = sizeof(client_addr);
                        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &addr_len);
                        
                        if(client_fd < 0) {
                            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                                //cout << "[DEBUG] No more connections to accept" << endl;
                                break;
                            }
                            perror("accept");
                            break;
                        }
                        
                        //cout << "[DEBUG] Accepted new connection, fd=" << client_fd << endl;
                        
                        set_nonblocking(client_fd);
                        //客户端client使用 ET 模式
                        epoll.add(client_fd, EPOLLIN | EPOLLET);
                        
                        //cout << "[DEBUG] Added client fd=" << client_fd << " to epoll with EPOLLIN|EPOLLET" << endl;
                        //cout << "New client connected (ET mode): " << client_fd << endl;
                    }
                } else {
                    // 处理客户端事件
                    handle_client(epoll, fd, events); 
                }
            }
        } catch(const exception& e) {
            cerr << "Error in event loop: " << e.what() << endl;
            break;
        }
    }

    close(listen_fd);
    return 0;
}
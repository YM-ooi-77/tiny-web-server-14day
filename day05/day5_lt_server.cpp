#include "epoll_lt.h"
#include "thread_pool.h"
#define BUF_SIZE 1024

void set_nonblocking(int fd);
bool handle_read(Epoll& epoll, int client_fd);
void close_client(Epoll& epoll, int client_fd);
void handle_client(Epoll& epoll, int client_fd, uint32_t events);

Epoll::Epoll(int max_events) {
    // 创建 epoll 实例
    // 参数 0 表示使用标准行为（旧版本的 epoll_create 需要指定大小，epoll_create1 更灵活）
    // 返回的是 epoll 文件描述符
    this->epfd_ = epoll_create1(0);
    
    // 注意：这里应该检查 epfd_ 是否为 -1（表示创建失败）
    if (this->epfd_ == -1) {
        // 处理错误，可以抛异常或记录日志
        throw std::runtime_error("Failed to create epoll instance");
    }
    
    // 为事件向量预留空间
    // epoll_wait 需要的是实际大小，不是容量
    // 所以你需要 resize() 而不是 reserve()
    
    this->ev_.resize(max_events);
}

Epoll::~Epoll() {
    // 只关闭有效的文件描述符
    // epoll 文件描述符通常应该总是有效的（除非被移动走）
    if(epfd_ >= 0) {
        int ret = close(epfd_);
    }
}

void Epoll::add(int fd, uint32_t events) {
    struct epoll_event event;
    //设置任务类型
    event.events = events;
    //设置用户数据
    event.data.fd = fd;
    //调用epoll_ctl添加监控
    int ret = epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event);
    
    if(ret == -1) {
        throw runtime_error("epoll_ctl add failed");
    }
}

void Epoll::mod(int fd, uint32_t events) {
    struct epoll_event event;
    //设置任务类型
    event.events = events;
    //设置用户数据
    event.data.fd = fd;
    //调用epoll_ctl修改监控
    int ret = epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event);
    
    if(ret == -1) {
        throw runtime_error(string("epoll_ctl mod failed for fd ") + to_string(fd));
    }
}

void Epoll::del(int fd) {
    //EPOLL_CTL_DEL 操作不需要有效的 epoll_event 结构

    // 基本检查
    if (fd < 0) {
        throw invalid_argument("Invalid file descriptor");
    }
    
    if (epfd_ < 0) {
        throw runtime_error("Epoll instance is not valid");
    }
    
    // 调用 epoll_ctl 删除监控
    int ret = epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    
    if (ret == -1) {
        // 包含错误码信息
        throw runtime_error(string("epoll_ctl del failed for fd ") + 
                            to_string(fd) + 
                            ": " + 
                            strerror(errno));
    }
}

int Epoll::wait(int timeout_ms) {
    //等待事件发生，返回就绪的事件数量，并将就绪的事件存储在ev_中
    // 1. 检查 epoll 实例有效性
    if (epfd_ < 0) {
        throw runtime_error("Epoll instance is not valid or has been moved");
    }
    
    // 2. 检查事件向量是否已初始化
    if (ev_.empty()) {
        throw runtime_error("Event vector is not initialized");
    }
    
    int ret;
    // 3. 处理信号中断
    do {
        ret = epoll_wait(epfd_, ev_.data(), ev_.size(), timeout_ms);
    } while (ret == -1 && errno == EINTR);
    
    // 4. 处理错误
    if (ret == -1) {
        // 提供更详细的错误信息
        throw runtime_error(string("epoll_wait failed: ") + strerror(errno));
    }
    
    return ret;
}

void handle_client(Epoll& epoll, int client_fd, uint32_t events) {
    //1.检查错误事件
    if(events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        close_client(epoll, client_fd);
        return;
    }

    //2.处理可读事件
    if(events & EPOLLIN) {
        if(!handle_read(epoll, client_fd)) {
            // 读取失败或对方关闭连接
            close_client(epoll, client_fd);
            return;
        }
    }
}

void close_client(Epoll& epoll, int client_fd) {
    // 1. 从 epoll 中删除
    epoll.del(client_fd);
    
    // 2. 关闭文件描述符
    close(client_fd);

}

bool handle_read(Epoll& epoll, int client_fd) {  // 改为引用
    char buffer[4096];
    ssize_t n = read(client_fd, buffer, sizeof(buffer));
    
    if(n > 0) {
        // // echo 回显
        // ssize_t written = 0;
        // while(written < n) {
        //     ssize_t ret = write(client_fd, buffer + written, n - written);
        //     if(ret <= 0) {
        //         if(errno == EAGAIN || errno == EWOULDBLOCK) {
        //             continue;
        //         }
        //         return false;  // 写失败
        //     }
        //     written += ret;
        // }
        // return true;

        // 对于HTTP请求，返回一个简单的HTTP响应
        const char* response = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, World!";
        
        write(client_fd, response, strlen(response));
        
        // 关闭连接（HTTP/1.0风格）
        close_client(epoll, client_fd);
        return true;
        
    } else if(n == 0) {
        // 客户端关闭连接
        return false;
    } else {
        // 错误处理
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;  // 非阻塞，没有数据可读
        }
        return false;
    }
}

//设置非阻塞模式函数
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    if((listen(listen_fd, 128)) < 0) {
        perror("listen");
        return 1;
    }

    Epoll epoll(1024);
    // 将监听socket添加到epoll，监听读事件
    epoll.add(listen_fd, EPOLLIN);

    cout << "ThreadPool echo server listening on :8080" << endl;
    cout << "Thread pool with 4 workers ready." << endl;

    //主循环，接受客户端连接
    while(true) {
        int n = epoll.wait(); 
        for(int i = 0; i < n; ++i) {
            struct epoll_event& ev = epoll.events()[i];
            int fd = ev.data.fd;
            uint32_t events = ev.events;
            
            if(fd == listen_fd) {
                // 处理新连接
                sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &addr_len);
                
                if(client_fd < 0) {
                    if(errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;  // 没有新连接，继续
                    }
                    perror("accept");
                    continue;
                }
                
                set_nonblocking(client_fd);
                epoll.add(client_fd, EPOLLIN);
            } else {
                // 处理客户端事件
                handle_client(epoll, fd, events); 
            }
        }
        
    }

    close(listen_fd);

    return 0;
}
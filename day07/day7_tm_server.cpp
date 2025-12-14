#include "mempool.h"
#include "timer.h"

#define BUF_SIZE 1024
//unordered_map<int, string> write_buffers;  //fd->代写数据

static Timer g_timer(30);  // 30秒超时的全局定时器
static MemoryPool g_mempool(4096, 1000);  // 全局内存池

// mutex g_write_buffers_mutex;
static thread_local unordered_map<int, string> write_buffers;

const char HTTP_RESPONSE[] = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";
const size_t HTTP_RESPONSE_LEN = sizeof(HTTP_RESPONSE) - 1;

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

    //3. 处理可写事件
    if(events & EPOLLOUT) {
        handle_write(epoll, client_fd);
    }
}

void close_client(Epoll& epoll, int client_fd) {
    // 1. 从定时器中删除（必须先做）
    g_timer.del(client_fd);  // O(1)操作
    
    // 2. 从 epoll 中删除
    epoll.del(client_fd);
    
    // 3. 关闭文件描述符
    close(client_fd);
    
    // 4. 清理代写数据缓冲区
    auto it = write_buffers.find(client_fd);
    if (it != write_buffers.end()) {
        write_buffers.erase(it);
    }
}

//正常的ET回显读取数据
bool handle_read(Epoll& epoll, int client_fd) {
    //使用内存池分配缓冲区
    char* buffer = static_cast<char*>(g_mempool.alloc(4096));
    if (!buffer) {
        cerr << "[ERROR] Memory pool exhausted for fd=" << client_fd << endl;
        return false;
    }

    bool success = true;

    //更新定时器
    g_timer.update(client_fd);

    // 边缘触发：必须循环读取直到 EAGAIN
    while(true) {
        ssize_t n = read(client_fd, buffer, 4096);

        if(n > 0) {
            //循环读，指导EAGAIN
            continue;
        } 
        else if(n == 0) {
            // 客户端关闭连接
            //cout << "[DEBUG] client " << client_fd << " closed connection" << endl;
            success = false;
            break;
        } 
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;
    }

    {
        //lock_guard<mutex> lock(g_write_buffers_mutex);

        //write_buffers[client_fd].append(buffer, n);  //将buffer里的数据存到缓冲区
                
        // 直接设置HTTP响应，不再存储原始数据(用于wrk测压)
        write_buffers[client_fd] = HTTP_RESPONSE;
    }

    //更新定时器，连接有活动
    g_timer.update(client_fd);

    //监听可写事件
    epoll.mod(client_fd, EPOLLOUT | EPOLLET);

    g_mempool.dealloc(buffer);
    if(!success) g_timer.del(client_fd);

    // 理论上不会到达这里，但为了编译器警告
    return true;
}

// void handle_write(Epoll& epoll, int client_fd) {
//     string& buf = write_buffers[client_fd];

//     {
//         lock_guard<mutex> lock(g_write_buffers_mutex);
//         auto it = write_buffers.find(client_fd);
//         if (it == write_buffers.end()) {
//             return;
//         }
//         buf = move(it->second);  // 拿到数据后立刻解锁
//         write_buffers.erase(it);
//     }

//     while(!buf.empty()) {
//         ssize_t w = write(client_fd, buf.data(), buf.size());

//         if(w > 0) {
//             buf.erase(0, w);  //删除已写数据
//             g_timer.update(client_fd);
//         }
//         else if(w < 0) {
//             if(errno == EAGAIN || errno == EWOULDBLOCK) {
//                 // 等下次EPOLLOUT
//                 lock_guard<mutex> lock(g_write_buffers_mutex);
//                 write_buffers[client_fd] = move(buf);
//                 epoll.mod(client_fd, EPOLLOUT | EPOLLET);
//                 return;
//             } else {
//                 // 写错误，关闭连接
//                 close_client(epoll, client_fd);
//                 return;
//             }
//         }
//         else {
//             return;
//         }
//     }

//     // 写完了，恢复监听读
//     epoll.mod(client_fd, EPOLLIN | EPOLLET);
// }

void handle_write(Epoll& epoll, int client_fd) {
    string buf;
    {
        //lock_guard<std::mutex> lock(g_write_buffers_mutex);
        auto it = write_buffers.find(client_fd);
        if (it == write_buffers.end()) return;
        buf = move(it->second);  // 拿到数据就解锁
        write_buffers.erase(it);
    }

    // 无锁区：专心写
    ssize_t w = write(client_fd, buf.data(), buf.size());
    if (w > 0) {
        if (w < (ssize_t)buf.size()) {
            // 没写完，重新加锁扔回 map
            //lock_guard<mutex> lock(g_write_buffers_mutex);
            write_buffers[client_fd] = buf.substr(w);
            epoll.mod(client_fd, EPOLLOUT | EPOLLET);
        } else {
            // 写完了，恢复监听读
            epoll.mod(client_fd, EPOLLIN | EPOLLET);
        }
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

    //设置定时器回调函数
    g_timer.set_close_callback([&epoll](int fd) {
        //cout << "[Timer] Closing fd=" << fd << " due to timeout" << endl;
        close_client(epoll, fd);
    });

    cout << "Server with Timer(30s) and MemoryPool listening on :8080" << endl;

    //添加循环计数器
    int loop_counter = 0;

    //主循环，接受客户端连接
    while(true) {
        //cout << "[DEBUG] === Waiting for epoll_wait...===" << endl << flush;
        try {
            //使用定时器计算的超时时间
            int timeout_ms = g_timer.get_next_timeout();

            int n = epoll.wait(timeout_ms); 
            
            // if(++loop_counter % 10 == 0) {
            //     g_timer.tick();
            //     loop_counter = 0;
            // }
            g_timer.tick();
            
            for(int i = 0; i < n; ++i) {
                struct epoll_event ev = epoll.get_event(i);
                int fd = ev.data.fd;
                uint32_t events = ev.events;
                
                if(fd == listen_fd) {
                    // 边缘触发：必须循环 accept 直到 EAGAIN
                    while(true) {
                        sockaddr_in client_addr;
                        socklen_t addr_len = sizeof(client_addr);

                        // 使用accept4，直接设置非阻塞和CLOEXEC
                        int client_fd = accept4(listen_fd, (sockaddr*)&client_addr, &addr_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
                        
                        if(client_fd < 0) {
                            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            perror("accept");
                            break;
                        }

                        //客户端client使用 ET 模式
                        epoll.add(client_fd, EPOLLIN | EPOLLET);
                        
                        // 新连接，添加到定时器
                        g_timer.add(client_fd);

                        cout << "[INFO] New connection: fd=" << client_fd << endl;
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
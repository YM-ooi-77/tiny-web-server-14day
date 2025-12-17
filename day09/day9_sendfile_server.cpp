#include "mempool.h"
#include "timer.h"
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/stat.h>
#include "auto_buffer.h"
#include <fcntl.h>
#include <sys/sendfile.h>

#define BUF_SIZE 1024
#define MAX_EVENTS 65536
#define WORKER_THREADS 4  //工作线程数

struct FileTransferState {
    int file_fd;           // 文件描述符
    off_t offset;          // 当前发送偏移
    off_t file_size;       // 文件总大小
    bool in_progress;      // 是否正在传输
    string file_path;      // 文件路径（用于错误处理）
};

//每个工作线程的数据结构(新加的)
struct WorkerThread {
    thread th;
    int epoll_fd;
    Timer timer;
    unordered_map<int, string> write_buffers;
    // MemoryPool mempool;  //每个线程独立的内存池
    atomic<int> process_requests{0};  //不知道是啥东西
    atomic<bool> running{true};
    //添加连接状态管理
    mutex conn_mutex;
    unordered_map<int, atomic<bool>> conn_closed;  // fd -> 是否已关闭

    unordered_map<int, FileTransferState> file_transfers;  // fd -> 传输状态

    WorkerThread() : timer(30){}

    //添加安全关闭连接的方法
    void safe_close(int fd) {
        lock_guard<mutex> lock(conn_mutex);
        
        // 标记为已关闭
        conn_closed[fd] = true;
        
        // 清理文件传输
        auto file_it = file_transfers.find(fd);
        if(file_it != file_transfers.end()) {
            close(file_it->second.file_fd);
            file_transfers.erase(fd);
        }

        // 从epoll中删除
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        
        // 关闭文件描述符
        close(fd);
        
        // 清理数据
        write_buffers.erase(fd);
        timer.del(fd);
        conn_closed.erase(fd);
    }
    
    //检查连接是否已关闭
    bool is_closed(int fd) {
        lock_guard<mutex> lock(conn_mutex);
        auto it = conn_closed.find(fd);
        return it != conn_closed.end() && it->second;
    }
};

//static Timer g_timer(30);  // 30秒超时的全局定时器
static atomic<int> g_next_worker{0};
static WorkerThread g_worker[WORKER_THREADS];
static int g_listen_fd = -1;
static atomic<bool> g_server_running{true};
static thread_local MemoryPool g_mempool(4096, 1000);  // 全局内存池

// mutex g_write_buffers_mutex;
//static thread_local unordered_map<int, string> write_buffers;

const char HTTP_RESPONSE[] = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 132\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "<html><body>"
    "<h1>Sendfile Test Server</h1>"
    "<a href='/file'>Download 100MB File (sendfile)</a><br>"
    "<a href='/'>Hello World (traditional)</a>"
    "</body></html>";
const size_t HTTP_RESPONSE_LEN = sizeof(HTTP_RESPONSE) - 1;

//设置非阻塞模式函数
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//设置TCP选项
void set_tcp_options(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &yes, sizeof(yes));

    //增大缓冲区
    int rcvbuf = 1024 * 1024;
    int sndbuf = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
}

//发送文件的辅助函数
bool start_file_transfer(WorkerThread& worker, int client_fd, const string& file_path) {
    //打开文件
    int file_fd = open(file_path.c_str(), O_RDONLY);
    if(file_fd < 0) {
        perror("open file");
        return false;
    }

    //获取文件大小
    struct stat file_stat;
    if(fstat(file_fd, &file_stat) < 0) {
        perror("fstat");
        close(file_fd);
        return false;
    }

    // 设置文件传输状态
    FileTransferState state;
    state.file_fd = file_fd;
    state.offset = 0;
    state.file_size = file_stat.st_size;
    state.in_progress = true;
    state.file_path = file_path;
    
    worker.file_transfers[client_fd] = state;

    //发送HTTP响应头
    string header = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/octet-stream\r\n"
                    "Content-Length: " + to_string(file_stat.st_size) + "\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
    
    //先发送响应头
    ssize_t header_sent = write(client_fd, header.c_str(), header.size());
    if(header_sent < (ssize_t)header.size()) {
        close(file_fd);
        worker.file_transfers.erase(client_fd);
        return false;
    }

    //修改epoll监听事件为可写事件
    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLET;
    ev.data.fd = client_fd;
    epoll_ctl(worker.epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);

    return true;
}

//继续发送文件内容
bool continue_file_transfer(WorkerThread& worker, int client_fd) {
    auto it = worker.file_transfers.find(client_fd);
    if(it == worker.file_transfers.end()) {
        return false;
    }

    FileTransferState& state = it->second;

    //使用sendfile发送文件内容
    ssize_t sent = sendfile(client_fd, state.file_fd, &state.offset, state.file_size - state.offset);
    if(sent < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            //内核缓冲区满，等待下次EPOLLOUT
            return true;
        }
        perror("sendfile");
        close(state.file_fd);
        worker.file_transfers.erase(client_fd);
        return false;
    }

    //更新定时器
    worker.timer.update(client_fd);

    //检查是否发送完成
    if(state.offset >= state.file_size) {
        //文件发送完成
        close(state.file_fd);
        worker.file_transfers.erase(client_fd);

        //恢复监听读事件
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        epoll_ctl(worker.epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);

        return false;  //传输结束
    }

    //文件还没发送完，继续等待EPOLLOUT
    return true;
}

//清理文件传输状态
void cleanup_file_transfer(WorkerThread& worker, int client_fd) {
    auto it = worker.file_transfers.find(client_fd);
    if(it != worker.file_transfers.end()) {
        close(it->second.file_fd);
        worker.file_transfers.erase(it);
    }
}

//工作线程的handle_client
void worker_handle_client(WorkerThread& worker, int client_fd, uint32_t events) {
    if(worker.is_closed(client_fd)) {
        return;
    }

    //检查是否在进行文件传输
    auto file_it = worker.file_transfers.find(client_fd);
    if(file_it != worker.file_transfers.end()) {
        //在传输文件，处理发送
        continue_file_transfer(worker, client_fd);
        return;
    }

    if(events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        //错误事件，关闭连接
        epoll_ctl(worker.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
        worker.safe_close(client_fd);
        worker.timer.del(client_fd);
        worker.write_buffers.erase(client_fd);
        return;
    }

    if(events & EPOLLIN) {
        //读取数据
        //从内存池分配缓冲区
        AutoBuffer buffer(g_mempool, 4096);
        if(!buffer) {
            // 内存池耗尽，关闭连接
            cleanup_file_transfer(worker, client_fd);
            // epoll_ctl(worker.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
            worker.safe_close(client_fd);
            // worker.timer.del(client_fd);
            return;
        }

        //更新定时器
        worker.timer.update(client_fd);

        //ET模式循环读取
        while(true) {
            ssize_t n = read(client_fd, buffer.get(), 4096);

            if(n > 0) {
                worker.process_requests++;
                
                //解析HTTP请求，决定发送什么
                string requests(buffer.get(), n);

                //简单解析请求行
                size_t space1 = requests.find(' ');
                size_t space2 = requests.find(' ', space1 + 1);

                if(space1 != string::npos && space2 != string::npos) {
                    string method = requests.substr(0, space1);
                    string path = requests.substr(space1 + 1, space2 - space1 - 1);

                    if(path == "/file") {
                        if(start_file_transfer(worker, client_fd, "test_file.bin")) {
                            //文件传输已经开始，直接返回
                            return;
                        }
                        else {
                            //文件传输失败，发送错误响应
                            string error_response = 
                                "HTTP/1.1 404 Not Found\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 13\r\n"
                                "\r\n";
                                "File not found";

                            worker.write_buffers[client_fd] = error_response;
                        }
                    }
                    else if(path.find("/download/") == 0 && path.length() > 10) {
                        //安全检查：确保路径长度大于10（"/download/".length() = 9）
                        // 解析文件名 /download/filename
                        string file_path = "downloads/" + path.substr(10);
                        
                        if(start_file_transfer(worker, client_fd, file_path)) {
                            // 文件传输已开始，直接返回
                            return;
                        } else {
                            // 文件传输失败，发送错误响应
                            string error_response = 
                                "HTTP/1.1 404 Not Found\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 13\r\n"
                                "\r\n"
                                "File not found";
                            worker.write_buffers[client_fd] = error_response;
                        }
                    }
                    else {
                        // 普通HTTP响应
                        worker.write_buffers[client_fd] = HTTP_RESPONSE;
                    }
                }
                else {
                    //无效请求
                    worker.write_buffers[client_fd] = HTTP_RESPONSE;
                }

                //修改为监听写事件
                struct epoll_event ev;
                ev.events = EPOLLOUT | EPOLLET;
                ev.data.fd = client_fd;
                epoll_ctl(worker.epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);

                //更新计时器
                worker.timer.update(client_fd);
                continue;
            }
            else if(n == 0) {
                //客户端关闭连接
                cleanup_file_transfer(worker, client_fd);
                //epoll_ctl(worker.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                worker.safe_close(client_fd);
                //worker.timer.del(client_fd);
                //worker.write_buffers.erase(client_fd);
                return;
            }
            else {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                else {
                    //读取错误
                    cleanup_file_transfer(worker, client_fd);
                    //epoll_ctl(worker.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    worker.safe_close(client_fd);
                    //worker.timer.del(client_fd);
                    //worker.write_buffers.erase(client_fd);
                    return;
                }
            }
        }
    }

    if(events & EPOLLOUT) {
        //写入数据
        auto it = worker.write_buffers.find(client_fd);  //查看是否有待写的数据
        if(it == worker.write_buffers.end()) {
            //没有数据可写
            //恢复到监听读状态
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = client_fd;
            epoll_ctl(worker.epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
            return;
        }
        string& buf = it->second;  //这个it表示什么

        while(!buf.empty()) {
            ssize_t w = write(client_fd, buf.data(), buf.size());

            if(w > 0) {
                buf.erase(0, w);
                worker.timer.update(client_fd);
            }
            else if(w < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    //等下次EPOLLOUT
                    struct epoll_event ev;
                    ev.events = EPOLLOUT | EPOLLET;
                    ev.data.fd = client_fd;
                    epoll_ctl(worker.epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
                    return;
                }
                else {
                    //写错误
                    cleanup_file_transfer(worker, client_fd);
                    //epoll_ctl(worker.epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    worker.safe_close(client_fd);
                    //worker.timer.del(client_fd);
                    //worker.write_buffers.erase(client_fd);
                    return;
                }
            }
        }

        //写完了，恢复监听读事件
        if(buf.empty()) {
            worker.write_buffers.erase(client_fd);
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.fd = client_fd;
            epoll_ctl(worker.epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
        }
    }
} 

//工作线程主函数
void worker_thread_func(int worker_id) {
    WorkerThread& worker = g_worker[worker_id];  //找到当前线程

    //创建epoll实例
    worker.epoll_fd = epoll_create1(0);
    if(worker.epoll_fd < 0) {
        perror("epoll_create1");
        return;
    }

    //设置定时器回调
    worker.timer.set_close_callback([&worker](int fd) {
        if(!worker.is_closed(fd)) {
            worker.safe_close(fd);
        }
    });

    //使用SO_REUSEPORT,每个线程都监听同一个端口
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd < 0) {
        perror("socket");
        close(worker.epoll_fd);
        return;
    }

    //设置socket选项
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    //绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;

    set_nonblocking(listen_fd);

    if((bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)) {
        perror("bind");
        close(listen_fd);
        close(worker.epoll_fd);
        return;
    }

    if(listen(listen_fd, 65535) < 0) {
        //65535是什么
        perror("listen");
        close(listen_fd);
        close(worker.epoll_fd);
        return;
    }

    //将监听socket添加到epoll
    struct epoll_event ev;
    ev.data.fd = listen_fd;
    ev.events = EPOLLIN | EPOLLET;
    epoll_ctl(worker.epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    cout << "Worker " << worker_id << " started, listening on port 8080" << endl;

    struct epoll_event events[MAX_EVENTS];

    //统计变量
    long long last_stats_time = time(nullptr);  //什么含义
    long long local_requests = 0;

    //工作循环
    while(worker.running) {
        //计算超出时间
        int timeout_ms = worker.timer.get_next_timeout();
        if(timeout_ms < 0) timeout_ms = 100;  //最小100ms

        //等待事件(事件的个数)
        int n = epoll_wait(worker.epoll_fd, events, MAX_EVENTS, timeout_ms);

        //处理定时器
        worker.timer.tick();

        //统计
        time_t now = time(nullptr);
        if(now != last_stats_time) {
            worker.process_requests += local_requests;
            local_requests = 0;
            last_stats_time = now;
        }

        for(int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;

            if(fd == listen_fd) {
                //接受新连接(批量处理)
                for(int j = 0; j < 100; ++j) {
                    //同时处理100个
                    int accepted = 0;
                    while(accepted < 64 && worker.running) {  //每次最多接受64个
                        int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if(client_fd < 0) {
                            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            else if(errno == EINTR) {
                                continue;
                            }
                            perror("accept4");
                            usleep(1000);
                            continue;
                        }

                        //设置TCP选项
                        set_tcp_options(client_fd);

                        //添加到epoll
                        struct epoll_event client_ev;
                        client_ev.events = EPOLLIN | EPOLLET;
                        client_ev.data.fd = client_fd;
                        epoll_ctl(worker.epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);

                        //添加到定时器
                        worker.timer.add(client_fd);

                        accepted++;
                    }
                }
            }
            else {
                //处理客户端事件
                worker_handle_client(worker, fd, revents);
                local_requests++;
            }
        }
    }
    //清理
    close(listen_fd);
    close(worker.epoll_fd);

    cout << "Worker:" << worker_id << "stopped!" << endl;
}

//信号处理函数
void signal_handler(int sig) {
    if(sig == SIGINT || sig == SIGTERM) {
        cout << "\nShutting down server..." << endl;
        g_server_running = false;

        //停止所有工作进程
        for(int i = 0; i < WORKER_THREADS; ++i) {
            g_worker[i].running = false;
        }
    }
}

//统计线程函数
void stats_thread_func() {
    while(g_server_running) {
        sleep(1);  //为什么这里要睡一秒

        long long total_requests = 0;
        for(int i = 0; i < WORKER_THREADS; ++i) {
            total_requests += g_worker[i].process_requests;
            g_worker[i].process_requests = 0;
        }

        cout << "QPS:" << total_requests << endl;
    }
}

int main(int argc, char const *argv[])
{
    //设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  //忽略SIGPIPE信号

    cout << "Starting multi-threaded server with " << WORKER_THREADS << " workers" << endl;

    for(int i = 0; i < WORKER_THREADS; ++i) {
        g_worker[i].th = thread(worker_thread_func, i);
    }

    //启动统计线程
    thread stats_thread(stats_thread_func);

    //等待所有工作线程结束
    for(int i = 0; i < WORKER_THREADS; ++i) {
        g_worker[i].th.join();
    }

    //停止统计线程
    g_server_running = false;
    stats_thread.join();

    cout << "Server stopped!" << endl;
    
    return 0;
}
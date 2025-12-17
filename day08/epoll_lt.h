#ifndef EPOLL_LT_H
#define EPOLL_LT_H

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <string.h>
#include <sys/wait.h>
#include <vector>
#include <queue>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <sys/epoll.h>
#include <stdexcept>
#include <fcntl.h>
#include <map>
#include <stack>
#include <unordered_map>

using namespace std;

class Epoll {
public:
    Epoll(int max_events = 1024);
    ~Epoll();
    void add(int fd, uint32_t events);  //将fd添加到epoll监听中，监听的事件由参数events指定
    void mod(int fd, uint32_t events);  //修改fd上监听的事件
    void del(int fd);                   //从epoll监听中删除fd
    int wait(int timeout_ms = -1);      //等待事件发生，返回就绪的事件数量，并将就绪的事件存储在ev_中
    //返回ev_的数据指针，用于获取就绪的事件
    struct epoll_event get_event(int idx) { 
        if (idx >= ev_.size()) {
            throw runtime_error("Event index out of range");
        }
        return ev_[idx]; 
    }
private:
    int epfd_;  //epoll 实例的文件描述符
    /*
    用于：
    1.添加/修改/删除监控的文件描述符
    2.等待事件发生
    3.在内核和用户空间之间传递事件信息*/
    vector<struct epoll_event> ev_;  //用于存储从 epoll_wait 返回的事件
    /*
    每个 epoll_event 结构体包含：
    events：事件类型（如可读、可写、错误等）
    data：用户数据（通常是文件描述符或其他标识）*/
};

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
    //cout << "[DEBUG] epoll_ctl ADD ret=" << ret << " errno=" << errno << endl;

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

void set_nonblocking(int fd);
bool handle_read(Epoll& epoll, int client_fd);
void handle_write(Epoll& epoll, int client_fd);
void close_client(Epoll& epoll, int client_fd);
void handle_client(Epoll& epoll, int client_fd, uint32_t events);

#endif
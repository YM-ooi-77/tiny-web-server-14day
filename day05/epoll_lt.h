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
    struct epoll_event* events() { return ev_.data(); }
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


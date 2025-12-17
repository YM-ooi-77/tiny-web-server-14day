#ifndef TIMER_H
#define TIMER_H 

#include <time.h>
#include <vector>
#include <list>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <chrono>  //高精度时间戳

using namespace std;

class Timer {
public:
    Timer(int timeout_sec = 30);  // 初始化超时时间
    ~Timer();
    
    // 保持原有接口不变
    void add(int fd);           // 添加连接的定时器(病人拿号)
    void update(int fd);        // 更新超时时间(病人动了，重新拿号)
    void del(int fd);           // 删除连接的定时器(病人走了，删号)
    void tick();                // 每 1s 调用，踢掉超时fd(秒针走一格，叫号)
    int get_next_timeout();     // 获取下一个超时时间，用于精确控制epoll_wait的超时(看下一个病人还有多久到)
    
    void set_close_callback(function<void(int)> callback) {
        close_callback_ = callback;  //设置踢人时的回调函数
    } // 设置关闭连接的回调函数

private:
    // 时间轮参数
    static const int SLOT_COUNT = 60;  // 60个槽，每槽1秒  就像钟表60秒一圈
    static const int TICK_INTERVAL_MS = 1000;  // 每tick 1000ms  时间间隔
    
    struct TimerNode {  //该进程的信息卡
        int fd;
        int rotation;  // 还需要多少轮才轮到该进程
        int slot;  //在第几个槽里（0~59）
        list<int>::iterator it;  // 在槽队伍里的位置（迭代器）
        // 为啥存iterator？因为list删除要快，O(1)时间找到人
    };
    
    int timeout_sec_;                 // 超时秒数
    int current_slot_;                // 当前槽指针，秒针现在指向第几格
    long long last_tick_time_;        // 上次tick时间
    
    // 时间轮数据结构
    vector<list<int>> slots_;         // 每个槽存储fd链表，每个槽是个进程队伍
    unordered_map<int, TimerNode> fd_to_node_;  // fd -> TimerNode
    
    function<void(int)> close_callback_;  // 关闭连接的回调函数
    
    // 计算超时时间对应的槽位
    int calculate_slot(int timeout_sec);
    
    // 获取当前毫秒时间戳
    long long get_current_ms();
};

Timer::Timer(int timeout_sec) 
    : timeout_sec_(timeout_sec)
    , current_slot_(0)
    , last_tick_time_(get_current_ms()) {
    
    // 初始化时间轮槽
    slots_.resize(SLOT_COUNT);
}

Timer::~Timer() {
    // 清理所有定时器
    for(auto& slot : slots_) {
        slot.clear();
    }
    fd_to_node_.clear();
}

void Timer::add(int fd) {
    // 先删除可能存在的旧定时器
    del(fd);
    
    // 计算槽位和轮数
    int ticks = timeout_sec_;  // 超时秒数就是ticks数  30秒超时 = 30个tick
    int rotation = ticks / SLOT_COUNT;  //30 / 60 = 0圈（半圈以内）
    int slot = (current_slot_ + ticks) % SLOT_COUNT;  //秒针+30格，对60取模
    
    // 添加到对应槽的链表头部(新病人站前面）
    slots_[slot].push_front(fd);
    
    // 保存节点信息
    TimerNode node;
    node.fd = fd;
    node.rotation = rotation;
    node.it = slots_[slot].begin();
    
    fd_to_node_[fd] = node;
}

void Timer::update(int fd) {
    // 更新就是重新添加
    add(fd);
}

void Timer::del(int fd) {
    auto it = fd_to_node_.find(fd);
    if(it != fd_to_node_.end()) {
        TimerNode& node = it->second;
        // 从槽链表中删除
        slots_[node.slot].erase(node.it);
        // 从map中删除
        fd_to_node_.erase(it);
    }
}

void Timer::tick() {
    long long now = get_current_ms();   //看现在时间点
    
    // 检查是否到达tick间隔（没到1秒就返回，不干活）
    if(now - last_tick_time_ < TICK_INTERVAL_MS) {
        return;
    }
    
    // 更新上次干活的时间
    last_tick_time_ = now;
    
    // 处理当前槽的所有定时器
    auto& current_slot = slots_[current_slot_];
    
    //遍历current_slot链表上的队伍
    for(auto it = current_slot.begin(); it != current_slot.end(); ) {
        int fd = *it;  //取出是哪个进程
        auto node_it = fd_to_node_.find(fd);
        
        if(node_it == fd_to_node_.end()) {
            // 节点已被删除，跳过
            ++it;
            continue;
        }
        
        TimerNode& node = node_it->second;  //拿到该进程信息
        
        if(node.rotation > 0) {
            // 还有好几圈才到他，轮数减1
            node.rotation--;
            ++it;  //下一个进程
        } else {
            // 超时触发
            if(close_callback_) {
                close_callback_(fd);
            }
            
            // 删除定时器
            it = current_slot.erase(it);
            fd_to_node_.erase(fd);
        }
    }
    
    // 移动到下一个槽
    current_slot_ = (current_slot_ + 1) % SLOT_COUNT;
}

int Timer::get_next_timeout() {
    long long now = get_current_ms();
    long long next_tick_time = last_tick_time_ + TICK_INTERVAL_MS;
    
    if(next_tick_time <= now) {
        return 0;  // 立即处理
    }
    
    int timeout_ms = static_cast<int>(next_tick_time - now);
    
    // 限制最小超时时间，避免忙等待
    if(timeout_ms < 1) {
        timeout_ms = 1;
    }
    
    return timeout_ms;
}

int Timer::calculate_slot(int timeout_sec) {
    int ticks = timeout_sec;  // 超时秒数转换为ticks
    int slot = (current_slot_ + ticks) % SLOT_COUNT;
    return slot;
}

long long Timer::get_current_ms() {
    using namespace std::chrono;
    auto now = system_clock::now();  //计算当前时间
    auto duration = now.time_since_epoch();
    return duration_cast<milliseconds>(duration).count();
}

#endif
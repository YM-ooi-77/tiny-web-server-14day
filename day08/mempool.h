#ifndef MEMPOOL_H
#define MEMPOOL_H

#include "epoll_lt.h"

#include <atomic>  //原子操作，保证无锁
#include <cstddef>

using  namespace std;

// 链表节点，像抽屉里的一沓钱
struct alignas(16) BlockNode {  //16字节对齐，避免ABA问题
    void* block;   //指向内存块
    BlockNode* next;  //下一个节点
};

class LockFreeStack {   //无锁栈  抽屉，伸手就能拿
public:
    LockFreeStack() : head_(nullptr) {}
    ~LockFreeStack() {
        // 析构时释放所有节点
        // 抽屉销毁，钱全扔掉
        BlockNode* cur = head_.load();  //.load() -> 获取栈顶
        while(cur) {
            BlockNode* next = cur->next;
            //free(cur->block);
            delete cur;
            cur = next;
        }
    }

    // 放钱进抽屉（无锁）
    void push(void* block) {
        auto* node = new BlockNode{block, nullptr};  //造个新钱袋

        // 看抽屉最上面那沓钱是谁（原子读）
        BlockNode* expected = head_.load(memory_order_relaxed);  //load是什么？  memory_order_relaxed是什么？
        do {
            node->next =  expected;  // 新节点指向旧栈顶  新钱袋指向旧栈顶
        }while(!head_.compare_exchange_weak(expected, node, memory_order_release, memory_order_relaxed));  //CAS：如果栈顶还是expected，换成node
        // 如果CAS失败（别人抢先放了钱），expected会被更新为新栈顶，继续循环
    }

    // 从抽屉拿钱（无锁）
    void* pop() {
        BlockNode* expected = head_.load(memory_order_relaxed);
        while(expected) {
            BlockNode* next = expected->next;
            if(head_.compare_exchange_weak(expected, next, memory_order_release, memory_order_relaxed)) {  //CAS：如果栈顶是expected，换成next
                void* block = expected->block;  //拿到钱
                delete expected;  //扔掉装钱的袋子
                return block;
            }
        }
        return nullptr;  //栈空
    }

private:
    atomic<BlockNode*> head_;  //栈顶，原子变量保证线程安全
};


class MemoryPool {
public:
    //预分配pool_size沓钱
    MemoryPool(size_t block_size, size_t pool_size) : block_size_(block_size), used_blocks_(0) {
        for(size_t i = 0; i < pool_size; ++i) {
            void* block = aligned_alloc(64, block_size);  //64字节对齐
            free_stack_.push(block);
            all_blocks_.push_back(block);
        }
    }

    ~MemoryPool() {
        for(void* block : all_blocks_) {
            free(block);
        }
    }

    //取钱
    void* alloc(size_t size) {  //分配内存
        if(size > block_size_) return nullptr;

        void* block =free_stack_.pop();
        if(block) {
            used_blocks_.fetch_add(1, memory_order_relaxed); //计数+1
        }
        return block;
    }  

    //存钱
    void dealloc(void* p) {  //释放内存
        if(!p) return;

        free_stack_.push(p);
        used_blocks_.fetch_sub(1, memory_order_relaxed);  //计数-1
    }  

    void stats() const  //获取统计信息
    {
        cout << "Used blocks: " << used_blocks_ << "/" << all_blocks_.size() << "\n";
    }

private:
    size_t block_size_;  //每个块的大小
    vector<void*> all_blocks_;  // 所有的块
    LockFreeStack free_stack_;  //无锁栈(空闲的块)
    atomic<size_t> used_blocks_;  // 原子计数，stats 用 花了多少块
};

#endif
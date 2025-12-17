#ifndef AUTO_BUFFER_H
#define AUTO_BUFFER_H

#include "mempool.h"

class AutoBuffer {
private:
    char* ptr_;
    MemoryPool& pool_;
    bool allocated_;
    
public:
    // 禁止拷贝
    AutoBuffer(const AutoBuffer&) = delete;
    AutoBuffer& operator=(const AutoBuffer&) = delete;
    
    // 允许移动
    AutoBuffer(AutoBuffer&& other) noexcept 
        : ptr_(other.ptr_), pool_(other.pool_), allocated_(other.allocated_) {
        other.ptr_ = nullptr;
        other.allocated_ = false;
    }
    
    AutoBuffer(MemoryPool& pool, size_t size) 
        : pool_(pool), ptr_(nullptr), allocated_(false) {
        ptr_ = static_cast<char*>(pool_.alloc(size));
        allocated_ = (ptr_ != nullptr);
    }
    
    ~AutoBuffer() {
        if(allocated_ && ptr_) {
            pool_.dealloc(ptr_);
        }
    }
    
    explicit operator bool() const { return allocated_; }
    char* get() { return ptr_; }
    const char* get() const { return ptr_; }
    char& operator[](size_t idx) { return ptr_[idx]; }
    const char& operator[](size_t idx) const { return ptr_[idx]; }
};

#endif
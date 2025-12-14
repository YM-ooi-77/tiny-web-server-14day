===========================================================================

xu@xu-VMware-Virtual-Platform:~$ wrk -t4 -c200 -d30s http://127.0.0.1:8080
Running 30s test @ http://127.0.0.1:8080
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.07ms    0.87ms  20.02ms   81.55%
    Req/Sec    23.95k     3.89k   41.75k    77.08%
  2863030 requests in 30.05s, 319.46MB read
Requests/sec:  95281.08
Transfer/sec:     10.63MB

============================================================================

xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day07$ valgrind --leak-check=full ./server_timewheel 
==11627== Memcheck, a memory error detector
==11627== Copyright (C) 2002-2024, and GNU GPL'd, by Julian Seward et al.
==11627== Using Valgrind-3.25.1 and LibVEX; rerun with -h for copyright info
==11627== Command: ./server_timewheel
==11627== 
bind: Address already in use
==11627== 
==11627== HEAP SUMMARY:
==11627==     in use at exit: 0 bytes in 0 blocks
==11627==   total heap usage: 2,017 allocs, 2,017 frees, 4,205,062 bytes allocated
==11627== 
==11627== All heap blocks were freed -- no leaks are possible
==11627== 
==11627== For lists of detected and suppressed errors, rerun with: -s
==11627== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)

=============================================================================

# Day7 timer + mempool 基线

- QPS：95281

## 关键优化
1. Timer: 60槽时间轮, O(1) tick, 30秒踢非活跃连接
2. MemoryPool: 无锁栈 + CAS原子操作, 0.1μs alloc/dealloc
3. 锁优化: write_buffers加mutex, 竞争激烈但可接受

## 代码亮点
- LockFreeStack::push/pop 使用 compare_exchange_weak
- Timer::tick 每格1秒, 轮数rotation控制多圈超时
- handle_read/write 使用RAII自动释放buffer
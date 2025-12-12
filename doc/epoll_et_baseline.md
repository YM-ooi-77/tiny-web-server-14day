xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day06$ wrk -t4 -c100 -d30s http://127.0.0.1:8080 
Running 30s test @ http://127.0.0.1:8080 
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.19ms    1.38ms  43.59ms   91.52%
    Req/Sec    24.37k     5.74k   54.38k    75.67%
  2917059 requests in 30.10s, 144.66MB read
Requests/sec:  96917.84
Transfer/sec:      4.81MB
====================================================================================
xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day06$ valgrind --leak-check=full ./day6_server
==19034== Memcheck, a memory error detector
==19034== Copyright (C) 2002-2024, and GNU GPL'd, by Julian Seward et al.
==19034== Using Valgrind-3.25.1 and LibVEX; rerun with -h for copyright info
==19034== Command: ./day6_server
==19034== 
bind: Address already in use
==19034== 
==19034== HEAP SUMMARY:
==19034==     in use at exit: 0 bytes in 0 blocks
==19034==   total heap usage: 5 allocs, 5 frees, 75,246 bytes allocated
==19034== 
==19034== All heap blocks were freed -- no leaks are possible
==19034== 
==19034== For lists of detected and suppressed errors, rerun with: -s
==19034== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
=====================================================================================

# Day6 epoll-ET 基线

- QPS：96917（比 Day5 提升 100%）
- 核心：ET 模式 + 循环读写到 EAGAIN
- 结论：边缘触发减少 epoll_wait 次数，性能翻倍
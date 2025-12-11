xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day05$ wrk -t4 -c100 -d30s http://127.0.0.1:8080
Running 30s test @ http://127.0.0.1:8080
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.69ms  807.86us  17.59ms   79.44%
    Req/Sec    12.15k     1.72k   18.79k    77.25%
  1453414 requests in 30.06s, 108.11MB read
  Socket errors: connect 0, read 1453381, write 0, timeout 0
Requests/sec:  48342.99
Transfer/sec:      3.60MB


xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day05$ valgrind --leak-check=full ./day5_server
==6114== Memcheck, a memory error detector
==6114== Copyright (C) 2002-2024, and GNU GPL'd, by Julian Seward et al.
==6114== Using Valgrind-3.25.1 and LibVEX; rerun with -h for copyright info
==6114== Command: ./day5_server
==6114== 
bind: Address already in use
==6114== 
==6114== HEAP SUMMARY:
==6114==     in use at exit: 0 bytes in 0 blocks
==6114==   total heap usage: 5 allocs, 5 frees, 75,246 bytes allocated
==6114== 
==6114== All heap blocks were freed -- no leaks are possible
==6114== 
==6114== For lists of detected and suppressed errors, rerun with: -s
==6114== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)


## 结果
- QPS：48342  
- P99 延迟：1.69 ms  
- 内存：valgrind 0 leak
- 结论：单线程 epoll-LT 比 Day4 线程池再提升 **4×**
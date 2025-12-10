xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day04$ wrk -t4 -c100 -d30s http://127.0.0.1:8080
Running 30s test @ http://127.0.0.1:8080
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    47.11ms   85.78ms 583.63ms   87.39%
    Req/Sec     2.93k     1.81k    8.86k    59.91%
  297166 requests in 30.04s, 19.55MB read
  Socket errors: connect 0, read 297166, write 0, timeout 0
Requests/sec:   9892.69
Transfer/sec:    666.60KB


xu@xu-VMware-Virtual-Platform:~/tiny-web-server/day04$ valgrind --leak-check=full ./day4_server &
sleep 3
[2] 658490
==658490== Memcheck, a memory error detector
==658490== Copyright (C) 2002-2024, and GNU GPL'd, by Julian Seward et al.
==658490== Using Valgrind-3.25.1 and LibVEX; rerun with -h for copyright info
==658490== Command: ./day4_server
==658490== 
bind: Address already in use
==658490== 
==658490== HEAP SUMMARY:
==658490==     in use at exit: 0 bytes in 0 blocks
==658490==   total heap usage: 18 allocs, 18 frees, 77,158 bytes allocated
==658490== 
==658490== All heap blocks were freed -- no leaks are possible
==658490== 
==658490== For lists of detected and suppressed errors, rerun with: -s
==658490== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
[2]+  退出 1                valgrind --leak-check=full ./day4_server

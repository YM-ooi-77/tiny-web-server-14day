//手写ThreadPool
#include "epoll_lt.h"

using  namespace std;

class ThreadPool{
public:
    ThreadPool(size_t thread_num);
    ~ThreadPool();

    //提交任务到线程池
    //简单说：这个函数能接受任何函数和它的参数，返回一个可以获取结果的future
    template<class F, class... Args> 
    auto enqueue(F&& f, Args&&... args) -> future<typename result_of<F(Args...)>::type> {
        using return_type = typename result_of<F(Args...)>::type;

        //创建packaged_task来包装函数
        auto task = make_shared<packaged_task<return_type()>>(
            bind(forward<F>(f), forward<Args>(args)...)  //创建一个无参数的函数对象（因为所有参数都已经绑定了）
        );

        future<return_type> res = task->get_future();

        {
            lock_guard<mutex> lock(queue_mutex);

            //不允许在停止后添加新任务
            if(stop) {
                throw runtime_error("enqueue on stopped ThreadPool");
            }

            //将新任务添加到队列
            tasks.emplace([task]() {(*task)();});  //当线程执行时，调用(*task)()来执行实际的函数
        }

        //通知一个等待的线程
        cv.notify_one();
        return res;
    }

private:
    vector<thread> workers;             //工作线程
    queue<function<void()>> tasks;      //任务队列
    mutex queue_mutex;                  //队列互斥锁
    condition_variable cv;              //条件变量
    bool stop;                          //线程池结束标志
};

ThreadPool::ThreadPool(size_t thread_num) : stop(false) {
    //创建工作线程
    for(size_t i = 0; i < thread_num; ++i) {
        workers.emplace_back([this]{
            while(true) {
                function<void()> task;  //创建任务变量，用来执行
                {
                    unique_lock<mutex> lock(this->queue_mutex);  //自动加锁，保护队列

                    //等待任务 或者 停止信号
                    this->cv.wait(lock, [this]{
                        return !this->tasks.empty() || this->stop;
                    });

                    //如果线程池已停止 或者 任务队列为空，则结束进程
                    if(this->stop || this->tasks.empty()) {
                        return;
                    }

                    //从任务列表里取出任务
                    task = move(this->tasks.front());
                    this->tasks.pop();
                }

                //执行任务
                task();
            }
        });
    }
}


ThreadPool::~ThreadPool() {
    {
        lock_guard<mutex> lock(queue_mutex);
        stop = true;
    }

    //通知所有线程
    cv.notify_all();

    //等待所有线程结束
    for(thread& worker : workers) {
        worker.join();
    }
}
#ifndef _THREADPOOL_H_
#define _THREADPOOL_H_

#include <iostream>
#include <list>
#include <cstdio>
#include <exception>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include"../log/log.h"
template <class T>
class threadpool
{
public:
    //threadpool();
    // 模型切换   ...   线程数量  请求队列最大允许的等待请求的数量
    // 数据库连接池还没写
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_requests = 1000); // 有参构造
    ~threadpool();

    bool append(T *requests, int state);

    bool append_p(T *requests);

private:
    // 工作线程的工作函数  不断从请求队列取出任务并执行
    static void *worker(void *args);
    void run();

private:
    int m_thread_number;  // 线程池中线程的数量
    pthread_t *m_threads; // 线程池中线程的数组  大小为m_thread_number

    int m_max_requests;          // 请求队列最大请求数量
    std::list<T *> m_workqueue;  // 请求队列
    connection_pool *m_connPool; // 数据库连接池
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestate;            // 是否有新任务需要处理
    // 数据库

    int m_actor_model; // 模型切换
};

// template <class T>
// threadpool<T>::threadpool() {}

// 有参构造
template <class T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests)
    : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)

{
    if (thread_number <= 0 || max_requests <= 0)
    {
// #ifdef _COUT_
//         std::cout << "thread_number <= 0 or max_requests <= 0" << std::endl;
// #endif
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
// #ifdef _COUT_
//         std::cout << "m_threads failed" << std::endl;
// #endif
        throw std::exception();
    }
    // 创建工作线程
    // 属性在请求队列里面
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(&m_threads[i], NULL, worker, this) != 0)
        {
            // 创建失败
            delete[] m_threads;
// #ifdef _COUT_
//             std::cout << "pthread_create failed" << std::endl;
// #endif
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]) != 0)
        {
            delete[] m_threads;
// #ifdef _COUT_
//             std::cout << "pthread_detach failed" << std::endl;
// #endif
            throw std::exception();
        }
    }
}

// 析构
template <class T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

// 往任务队列中添加任务
template <class T>
bool threadpool<T>::append(T *requests, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    requests->m_state = state;
    m_workqueue.push_back(requests);
    m_queuelocker.unlock();
    m_queuestate.post(); // 添加任务后，V信号量+ 1
    return true;
}

template <class T>
bool threadpool<T>::append_p(T *requests)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(requests);
    m_queuelocker.unlock();
    m_queuestate.post(); // 添加任务后，V信号量+ 1
    return true;
}

template <class T>
void *threadpool<T>::worker(void *args)
{
    threadpool *pool = (threadpool *)args;
    pool->run();
    return pool;
}

template <class T>
void threadpool<T>::run()
{
    // 工作线程通常是 长期运行 的，
    // 直到某个条件满足时才退出。在一个线程池的实现中，
    // 工作线程需要反复执行任务，
    // 因此需要一个循环来持续地处理任务。
    while (true)
    {
        // 先 P 操作，再上锁
        m_queuestate.wait();  // P 信号量
        m_queuelocker.lock(); // 上锁
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        // 取出一个任务
        T *requests = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        // 下面就是执行任务队列的任务
        //........
        if (!requests)
        {
            continue;
        }
        // 如果是reactor模式
        if (1 == m_actor_model)
        {
            //LOG_INFO("reactor");
            // 读
            if (0 == requests->m_state)
            {
                // 读成功
                if (requests->read_once())
                {
                    requests->improv = 1;
                    connectionRAII mysqlconn(&requests->mysql, m_connPool);
                    requests->process();
                }
                else
                {
                    requests->improv = 1;
                    requests->timer_flag = 1;
                }
            }
            // 写
            else
            {
                if (requests->write())
                {
                    // 写成功
                    requests->improv = 1;
                }
                else
                {
                    // 写失败
                    requests->improv = 1;
                    requests->timer_flag = 1;
                }
            }
        }
        //proactor
        else{
            //LOG_INFO("proactor");
            connectionRAII mysqlconn(&requests->mysql, m_connPool);
            requests->process();
        }
    }
}

#endif
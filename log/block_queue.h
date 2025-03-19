/*************************************************************
 *循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;
 *线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
 **************************************************************/
#ifndef _BLOCK_QUEUE_H_
#define _BLOCK_QUEUE_H_
#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
#include"log.h"
template <class T>
class block_queue
{
public:
    //主线程调用
    block_queue(int max_size = 1000)
    {
        if(max_size <= 0){
            //LOG_ERROR("%s","block_queue args error");
            exit(-1);
        }
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }
    ~block_queue()
    {
        m_mutex.lock();
        if(m_array != NULL){
            delete[] m_array;
        }
        m_mutex.unlock();
    }

    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    //pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item)
    {
        m_mutex.lock();
        while(m_size <= 0){

            if(!m_cond.wait(m_mutex.get())){
                m_mutex.unlock();
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    //增加了超时处理的pop
    bool pop(T &item , int m_timeout)
    {
        //使用 gettimeofday 获取当前时间，
        //并保存在 now 结构体中。
        //后续会根据这个时间加上超时时间来计算绝对等待截止时间。
        struct timespec t = {0,0};
        struct timeval now = {0,0};
        gettimeofday(&now,NULL);

        m_mutex.lock();

        if(m_size <= 0){
            // 判断队列是否为空：如果 m_size <= 0，说明队列中没有可取出的数据。
            // 计算超时截止时间：
            // 将当前秒数 now.tv_sec 加上 ms_timeout 换算成的秒数，存入 t.tv_sec；
            // 将剩余的毫秒转换为微秒（或纳秒，这里乘以 1000，假设单位为纳秒，但通常纳秒需要乘以 1,000,000；这部分可能与具体平台实现有关），存入 t.tv_nsec。
            // 条件变量等待：
            // 调用 m_cond.timewait(m_mutex.get(), t) 使当前线程等待，直到队列中有新数据（或超时）。
            // 如果等待超时或等待返回失败（timewait 返回 false），则释放互斥锁并返回 false。
            t.tv_sec = now.tv_sec + m_timeout / 1000;
            t.tv_nsec = (m_timeout % 1000) * 1000;
            if(!m_cond.timewait(m_mutex.get(),t)){
                m_mutex.unlock();
                return false;
            }

            //等待结束后，再次检查队列是否依然为空。如果是，说明没有新数据入队，释放锁并返回 false。
            if(m_size <= 0){
                m_mutex.unlock();
                return false;
            }

            m_front = (m_front + 1) % m_max_size;
            item = m_array[m_front];
            m_size--;
            m_mutex.unlock();
            return true;
        }
    
    }

    bool full()
    {
        m_mutex.lock();
        if(m_size >= m_max_size){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool empty()
    {
        m_mutex.lock();
        if(m_size == 0){
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    bool front(T & value)
    {
        m_mutex.lock();
        if(m_size == 0){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    bool back(T & value){
        m_mutex.lock();
        if(m_size == 0){
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;

    }

    bool push(const T &item)
    {
        m_mutex.lock();
        if(m_size >= m_max_size){
            //如果队列满了，就唤醒写日志进程，然后返回false改为同步写入
            //LOG_WARN("%s","asynchronous queue full, synchronized instead");
            m_cond.broadcast();//唤醒写日志进程
            m_mutex.unlock();
            return false;
        }
        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    int size(){
        int temp = 0;
        m_mutex.lock();
        temp = m_size;
        m_mutex.unlock();
        return temp;
    }

    int max_size(){
        int temp = 0;
        m_mutex.lock();
        temp = m_max_size;
        m_mutex.unlock();
        return temp;
    }

private:
    locker m_mutex;
    cond m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
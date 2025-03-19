#ifndef _LOCKER_H_
#define _LOCKER_H_

#include <pthread.h>
#include <semaphore.h>
#include <exception>
class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception(); // 如果 sem_init 初始化信号量 m_sem 时失败（返回值不为 0），
            // 就抛出一个空的 std::exception 异常。调用这个构造函数后，异常会被抛出，
            // 可以在后续的 try-catch 块中捕获并处理。如果你不捕获它，程序会终止。
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0; // 返回是否wait成功
    }
    bool post()
    {
        return sem_post(&m_sem) == 0; // 返回是否post成功
    }

private:
    sem_t m_sem;
};

class locker
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// pthread_cond_signal 需要与互斥锁配合使用。
// 一般情况下，线程在进入等待状态前会先获取互斥锁，
// 然后调用 pthread_cond_wait 等待条件变量。当某个条件被满足时，
// 另一个线程通过 pthread_cond_signal 通知等待的线程继续执行。
// 被通知的线程会重新获取互斥锁，并继续执行。
class cond
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    // 成功返回0 失败返回-1
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        // 当这个函数调用阻塞的时候，会进行解锁，当不阻塞的时候，继续向下执行，会重新加锁
        //  没有数据就会阻塞在这  当生产者执行pthread_cond_signal(&condition);  就会被唤醒，然后往下执行
        return ret == 0;
    }


    //等待多长时间，调用了这个函数，线程会阻塞，直到指定的时间结束
    //成功返回1 失败返回错误号
    bool timewait(pthread_mutex_t *m_mutex,const struct timespec abstime){
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond,m_mutex,&abstime);
        return ret == 0;
    }
    // pthread_cond_signal 通知（唤醒）一个正在等待条件变量的线程。具体来说：
    // 如果有线程在该条件变量上等待，pthread_cond_signal 会唤醒其中的一个线程。
    // 如果没有线程在该条件变量上等待，调用 pthread_cond_signal 时不会发生任何效果。
    // 成功返回0 失败返回错误号
    bool signal()
    {
        // int ret = 0;
        // pthread_cond_signal(&m_cond);
        // return ret == 0;

        return pthread_cond_signal(&m_cond) == 0;
    }
    //唤醒所有在这个条件变量上等待的线程
    bool broadcast()
    {
        // int ret = 0;
        // pthread_cond_broadcast(&m_cond);
        // return ret == 0;

        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};
#endif
#ifndef _LOG_H_
#define _LOG_H_

#include "../lock/locker.h"

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
using namespace std;

class Log
{
public:
    // C++11以后,使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    };
    // 异步模式写日志线程的工作函数
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    Log();
    virtual ~Log();
    // Log(const Log * log) = delete;

private:
    // 异步写日志
    // 从阻塞队列里面取出一个日志  写入文件
    void *async_write_log()
    {
        string single_log;
        // 循环工作
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128]; // 路径名
    char log_name[128]; // 日志文件名
    long long m_count;
    int m_log_buf_size; // 日志缓冲区大小
    int m_split_lines;  // 日志最大行数

    int m_today; // 因为按天分类，记录当前时间是哪一天
    FILE *m_fp;  // 打开的log文件指针
    char *m_buf;
    locker m_mutex;
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                  // 是否同步
    int m_close_log;
};
#define LOG_DEBUG(format, ...)                                    \
    if (m_close_log == 0)                                         \
    {                                                             \
        Log::get_instance()->write_log(0, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
#define LOG_INFO(format, ...)                                     \
    if (m_close_log == 0)                                         \
    {                                                             \
        Log::get_instance()->write_log(1, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
#define LOG_WARN(format, ...)                                     \
    if (m_close_log == 0)                                         \
    {                                                             \
        Log::get_instance()->write_log(2, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
#define LOG_ERROR(format, ...)                                    \
    if (m_close_log == 0)                                         \
    {                                                             \
        Log::get_instance()->write_log(3, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
// #define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#endif
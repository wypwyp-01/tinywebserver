#include "log.h"
#include <string>
#include <string.h>
#include<time.h>
#include<sys/time.h>
#include<pthread.h>
#include <stdarg.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{

    // 如果设置了max_queue_size，则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    // 最后一个/出现的位置
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }
    return true;
}


//等级 格式 内容
//同步  调用这个函数直接写入文件
//异步  调用这个函数写入阻塞队列，由线程写入文件
void Log::write_log(int level,const char * format,...){
    //定义并初始化一个 struct timeval 结构体变量 now，它包含两个成员：
    //tv_sec: 表示秒数，自1970年1月1日以来的秒数（UNIX时间戳）。
    //tv_usec: 表示微秒数（百万分之一秒）。
    struct timeval now = {0, 0};
    //获取当前时间
    gettimeofday(&now, NULL);
    //存储时间戳
    time_t t = now.tv_sec;
    //时间戳转换为tm类型
    struct tm *sys_tm = localtime(&t);
    //保存副本
    struct tm my_tm = *sys_tm;

    char s[16] = {0};

    switch(level){
        case 0:
            strcpy(s,"[debug]:");break;
        case 1:
            strcpy(s,"[info]:");break;
        case 2:
            strcpy(s,"[warn]:");break;
        case 3:
            strcpy(s,"[error]:");break;
        default:
            strcpy(s,"[info]:");break;
    }

    //
    m_mutex.lock();
    m_count++;
    //如果要新建文件
    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0){
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);

        char tail[16] = {0};
        snprintf(tail,16,"%d_%02d_%02d_",my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if(m_today != my_tm.tm_mday){
            snprintf(new_log,255,"%s%s%s",dir_name,tail,log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else{
            snprintf(new_log,255,"%s%s%s.%lld",dir_name,tail,log_name,m_count / m_split_lines);
        }

        m_fp = fopen(new_log,"a");
    }

    m_mutex.unlock();
    //处理变长参数
    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    //拼接要写入的字符串
    //写入具体的时间内容格式
    //返回值表示写入的字符数
    //2025-01-06 12:17:37.932948 [info]
    int n = snprintf(m_buf,48,"%d-%02d-%02d %02d:%02d:%02d.%06ld %s",
                    my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                    my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    int m = vsnprintf(m_buf + n,m_log_buf_size - 1,format,valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;
    m_mutex.unlock();

    //同步还是异步？
    if(m_is_async && !m_log_queue->full()){
        m_log_queue->push(log_str);
    }
    else{
        //同步写入
        m_mutex.lock();
        fputs(log_str.c_str(),m_fp);
        m_mutex.unlock();
    }
    //结束对可变参数列表的访问
    va_end(valst);
}



void Log::flush(){
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
#ifndef _WEBSERVER_H_
#define _WEBSERVER_H_
// #include"./http/http.h"
// #include"./threadpool/threadpool.h"
// #include<string>
// #include"./timer/lst_timer.h"
// #include"./log/log.h"
// #include <sys/epoll.h>
// #include <sys/types.h> /* See NOTES */
// #include <sys/socket.h>
// #include <assert.h>
// #include <netinet/in.h> // 提供 sockaddr_in 和相关的常量
// #include <arpa/inet.h>  // 提供 inet_pton 和 inet_ntoa 等函数
// #include <strings.h>
// #include"./CGImysql/sql_connection_pool.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http.h"

using namespace std;

const int MAX_FD = 65535;           // 最大文件描述符数
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位

class Webserver
{
public:
    Webserver();
    ~Webserver();
    void init(int port, string user, string passWord,
              string databasename, int log_write, int opt_linger,
              int trigmode, int sql_num, int thread_num, int close_log,
              int actor_model);

    // 线程池 数据库连接池
    void thread_pool();
    void sql_pool();

    // 日志 触发模式
    void trig_mode(); // LT + ET模式
    void log_write(); // 初始化日志

    // epoll通信流程
    void eventListen();
    void eventLoop();

    // 定时器类
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);

    // 任务处理类
    bool dealclientdata();
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);
    bool dealwithsignal(bool &timeout, bool &stop_server);

public:
    // 基础
    char *m_root;
    int m_port;       // 端口号
    int m_log_write;  // 日志写入方式 默认同步
    int m_close_log;  // 是否关闭日志 默认不关闭
    int m_actormodel; // 并发模型选择 默认是proactor

    int m_epollfd;    // epoll的描述符
    int m_pipefd[2];  // 管道消息
    http_conn *users; // 每个连接的用户对应一个http_conn类的对象

    // 数据库相关
    connection_pool *m_connpool;
    string m_user;         // 登录数据库用户名
    string m_password;     // 密码
    string m_databasename; // 使用数据库名
    int m_sql_number;      // 数据库连接池数量

    // 线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    // epollevent相关
    epoll_event events[MAX_EVENT_NUMBER];
    int m_listened_fd;    // 监听文件描述符
    int m_OPT_LINGER;     // 优雅断开连接
    int m_TRIGMode;       // 触发组合模式
    int m_LISTENTrigmode; // listen触发模式
    int m_CONNTrigmode;   // connfd触发模式

    // 定时器相关
    client_data *users_timer;
    Utils utils;
};
#endif
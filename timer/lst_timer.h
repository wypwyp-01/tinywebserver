#ifndef _LST_TIMER_H_
#define _LST_TIMER_H_

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

#include"../http/http.h"
class util_timer;

struct client_data{
    sockaddr_in address;
    int sockfd;
    util_timer * timer;
};

class util_timer{
public:
    util_timer():prev(NULL),next(NULL){}

    void (* cb_func) (client_data *);

    time_t expire;
    client_data * user_data;
    util_timer * prev;
    util_timer * next;
};

//定时器的队列
class sort_timer_lst{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer * timer);
    void adjust_timer(util_timer * timer);
    void del_timer(util_timer * timer);
    void tick();


private:
    void add_timer(util_timer * timer,util_timer * lst_head);

    util_timer * head;
    util_timer * tail;
};



class Utils{
public:
    Utils(){}
    ~Utils(){}

    //内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd,int fd,bool one_shot,int TRIGMode);
    
    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setunblocking(int fd);

    //连接的客户端达到了最大连接数 关闭客户端的文件描述符并发送错误
    void show_error(int connfd,const char * info);


    //信号处理函数
    static void sig_handler(int sig); 

    //设置信号函数
    void addsig(int sig,void(handler)(int) ,bool restart = true);


    //定时处理任务  重新定时以不断触发SIGALRM信号
    void timer_handler();
public:
    sort_timer_lst m_timer_lst;
    int m_TIMESLOT;
    static int *u_pipefd;
    static int u_epollfd;
};

void cb_func(client_data * user_data);

#endif
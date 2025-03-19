#include "lst_timer.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    // 如果 timer->expire 小于 tmp->expire，说明 timer 的位置正确，无需调整，直接返回。
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}


void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    if (!head)
    {
        head = tail = timer;
    }
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}


void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // tmp为空 插入到最后
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}



void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }

    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }

    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }

    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// tick() 函数的目的是检查定时器链表中的定时器是否到期，并执行到期定时器的回调函数。
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    time_t cur = time(NULL);

    util_timer *tmp = head;
    while (tmp)
    {
        // 如果定时器已经过期（即 cur >= tmp->expire），则继续处理
        if (cur < tmp->expire)
            break;
        // 如果定时器到期，调用定时器的回调函数 cb_func，
        // 并传递与定时器关联的用户数据 user_data。
        tmp->cb_func(tmp->user_data);
        // 定时器执行完回调函数后，删除当前定时器节点（即 tmp）。
        //  将 head 更新为下一个定时器节点 tmp->next。
        //  如果新的 head 存在，将 head->prev 设置为 NULL，以确保链表的双向链接关系没有被破坏。
        //  使用 delete tmp 删除当前定时器节点，释放内存。
        //  更新 tmp 为新的 head，继续检查下一个定时器。
        head = head->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

// 将内核事件表(epoll的红黑树)注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd; //
    // 在 TCP 连接中，对端关闭连接时，操作系统会向本地发出
    // EPOLLRDHUP 事件。对于 TCP 协议，
    // 这意味着对端已经发送了一个 "FIN"（结束）包，表示不再发送数据。
    if (TRIGMode == 1)
    { // ET
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else
    { // LT
        event.events = EPOLLIN | EPOLLRDHUP;
    }
    // EPOLLONESHOT 是 epoll 中的一种事件标志，
    //  用来设置一个 "一次性" 事件。它的作用是：当一个事件被触发后，
    //  只会被通知一次，即使文件描述符再次变为就绪状态，epoll
    //  也不会再次通知应用程序，除非你显式地重新注册该文件描述符。
    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setunblocking(fd); // 设置非阻塞
}

int Utils::setunblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

// 信号处理函数
void Utils::sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    // 使用 send 函数将信号编号 msg 发送到管道中。u_pipefd[1] 是管道的写端（发送端），msg 是发送的数据，长度为 1 字节。
    send(u_pipefd[1], (char *)&msg, 1, 0);
    // 在函数执行完后，恢复原先的 errno 值，确保信号处理函数对 errno 的修改不会影响到程序的其他部分。这样可以避免信号处理期间对 errno 的修改影响后续的错误处理。
    errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    // sigaction 结构体用于描述信号的处理方式。
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    // sa.sa_handler 用来指定当接收到特定信号时要执行的处理函数。
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    // sa.sa_mask 是一个信号集，它指定在信号处理程序执行期间要屏蔽的信号。sigfillset 会将所有信号（即 32 个信号）添加到信号集 sa.sa_mask 中，意味着在处理当前信号时，所有其他信号会被屏蔽，直到信号处理程序执行完毕。
    sigfillset(&sa.sa_mask);
    // sigaction 函数用于设置特定信号（sig）的处理方式，将 sa 结构体中的配置应用于信号 sig。
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务  重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{

    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data); // 如果为空，程序会在此处终止
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
#include "webserver.h"


void Webserver::thread_pool()
{
    m_pool = new threadpool<http_conn>(m_actormodel, m_connpool, m_thread_num);
}
void Webserver::sql_pool()
{
    // 初始化数据库连接池
    m_connpool = connection_pool::GetInstance();
    m_connpool->init("localhost", m_user, m_password, m_databasename, 3306, m_sql_number, m_close_log);
    // 初始化数据库读取表
    users->initmysql_result(m_connpool);//初始化http_conn中全局变量users，所以只需调用一次
}

Webserver::Webserver()
{
    // http_conn类对象
    // 每个连接的用户对应一个http_conn类的对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path,200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root,server_path);
    strcat(m_root,root);
    // 定时器
    users_timer = new client_data[MAX_FD];
}
Webserver::~Webserver()
{
    close(m_epollfd);
    close(m_listened_fd);
    close(m_pipefd[0]);
    close(m_pipefd[1]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}
void Webserver::init(int port, string user, string passWord,
                     string databasename, int log_write, int opt_linger,
                     int trigmode, int sql_num, int thread_num, int close_log,
                     int actor_model)
{
    m_port = port;
    m_user = user;
    m_password = passWord;
    m_databasename = databasename;
    m_sql_number = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_actormodel = actor_model;
    m_close_log = close_log;
}

void Webserver::trig_mode()
{
    /*
    m_LISTENTrigmode = m_TRIGMode / 2;
    m_CONNTrigmode = m_TRIGMode % 2;
    */
    // LT+LT
    if (m_TRIGMode == 0)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    else if (m_TRIGMode == 1)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    else if (m_TRIGMode == 2)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    else if (m_TRIGMode == 3)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void Webserver::eventListen()
{
    m_listened_fd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listened_fd >= 0); // 确保 m_listenfd 套接字描述符有效，若无效（即套接字创建失败），程序将会在此断言失败处终止

    // 优雅关闭连接
    if (m_OPT_LINGER == 0)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listened_fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (m_OPT_LINGER == 1)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listened_fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    // 启用套接字的 SO_REUSEADDR 选项，使得在套接字关闭后能够快速重用地址和端口，避免端口被占用的问题
    int flag = 1;
    setsockopt(m_listened_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    int ret = 0;
    ret = bind(m_listened_fd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listened_fd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);
    // 创建epoll
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    // 添加文件描述符到epoll
    utils.addfd(m_epollfd, m_listened_fd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    //使用 socketpair 创建一对 UNIX 域套接字，这通常用于在内核和用户空间之间传递信号或事件。
    //将 m_pipefd[1] 设置为非阻塞模式，并将 m_pipefd[0] 添加到 epoll 事件表中，这样可以通过 epoll 监听信号事件。
    ret = socketpair(PF_UNIX,SOCK_STREAM,0,m_pipefd);
    assert(ret != -1);
    utils.setunblocking(m_pipefd[1]);
    utils.addfd(m_epollfd,m_pipefd[0],false,0);

    //忽略 SIGPIPE 信号，防止因向已关闭 socket 写入数据而导致程序终止。
    //为 SIGALRM（定时器信号）和 SIGTERM（终止信号）设置自定义的信号处理函数 utils.sig_handler，第二个参数 false 表示是否重启被信号中断的系统调用（通常不重启）。
    utils.addsig(SIGPIPE,SIG_IGN);
    utils.addsig(SIGALRM,utils.sig_handler,false);
    utils.addsig(SIGTERM,utils.sig_handler,false);

    alarm(TIMESLOT);

    //工具类，信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void Webserver::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        // 等待事件发生
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // EINTR 表示 系统调用被信号中断，
        // 这时可以忽略错误并重新调用 epoll_wait
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure"); //
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理监听文件描述符（新到的客户连接）
            if (sockfd == m_listened_fd)
            {
                bool flag = dealclientdata();
                if (flag == false)
                {
                    continue;
                }
            }

            // 服务器端关闭连接  移除对应的定时器
            // 远程对端关闭了连接 文件描述符被挂起 发生错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (flag == false)
                {
                    LOG_ERROR("%s", "dealwithsignal failure");
                }
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }

        // 定时器相关？
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s","timer tick");

            timeout = false;
        }
    }
}

void Webserver::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        // 如果检测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);
        // 循环等待事件处理完成?
        while (true)
        {
            // 事件有没有处理完成的标志？
            if (1 == users[sockfd].improv)
            {
                if (users[sockfd].timer_flag == 1)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                // 重新设置为未完成?
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    // proactor
    else
    {
        if (users[sockfd].read_once())
        {
            // 读取成功
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若检测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            // 把定时器对象从链表中移除
            deal_timer(timer, sockfd);
        }
    }
}

void Webserver::dealwithwrite(int sockfd)
{
    LOG_INFO("dealwithwrite");
    // 用户的定时器
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel)
    {
        LOG_INFO("dealwithwrite reactor");
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }

    else
    {
        LOG_INFO("dealwithwrite proactor");
        // proactor
        if (users[sockfd].write())
        {

            LOG_INFO("send data to the client(%s))", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            LOG_ERROR("dealwithwrite write failure");
            deal_timer(timer, sockfd);
        }
    }
}

bool Webserver::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);

    if (ret == -1)
    {
        return false;
    }
    else if(ret == 0){
        return false;
    }
    else
    {
        for (int i = 0; i < ret; i++)
        {
            switch (signals[i])
            {
            //定时器信号 (SIGALRM): 通常用于定期执行某些操作，如检查超时的连接或触发周期性任务。
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            //终止信号 (SIGTERM): 通常用于优雅关闭服务器时使用，服务器会在接收到该信号后进行资源清理并安全退出。
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

bool Webserver::dealclientdata()
{
    // 建立通信文件描述符
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);

    if (m_LISTENTrigmode == 0)
    {
        // 水平触发
        int connfd = accept(m_listened_fd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is :%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            // 达到了最大连接数
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address); // 初始化这个用户的定时器
    }
    else
    {
        // 边沿触发
        while (1)
        {
            int connfd = accept(m_listened_fd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

void Webserver::timer(int connfd, struct sockaddr_in client_address)
{
    // 初始化这个用户的http_conn类对象
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_password, m_databasename);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    // 每个用户对应一个定时器对象
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;

    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;

    // 把定时器对象加入到链表中
    utils.m_timer_lst.add_timer(timer);
}

void Webserver::log_write()
{
    if (m_close_log == 0)
    {
        // 初始化日志
        if (m_log_write == 1)
        {
            // 日志写入方式  异步
            // 日志缓冲区大小、最大行数以及最长日志条队列
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        }
        else
        {
            // 同步
            // 日志缓冲区大小、最大行数以及最长日志条队列
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
        }
    }
}


void Webserver::deal_timer(util_timer * timer,int sockfd){
    timer->cb_func(&users_timer[sockfd]);
    //如果 timer 非空，则调用 utils.m_timer_lst.del_timer(timer) 删除该定时器，
    //避免它继续存储在定时器链表中。
    if(timer){
        utils.m_timer_lst.del_timer(timer);
    }
    LOG_INFO("close fd %d",users_timer[sockfd].sockfd);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void Webserver::adjust_timer(util_timer * timer){
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
    LOG_INFO("%s","adjust timer once");
}
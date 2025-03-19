#include "http.h"
#include <mysql/mysql.h>
#include <fstream>
// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;
// EPOLLRDHUP 是一个 epoll 事件标志，
// 用于监测对端套接字的半关闭状态。也就是说，
// 当对方关闭连接的一端但仍保持另一端打开时，
// 会触发 EPOLLRDHUP 事件。
// 将文件描述符重新设置为指定的状态
void modfd(int epollfd, int fd, int ev, int TRIGmode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGmode)
    {
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    }
    else
    {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 对文件描述符设置非阻塞
int setnoblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// close_conn调用
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 这个函数的作用是将文件描述符 fd 注册到 epoll
//  实例 epollfd 中，并配置相关的事件和模式。
//  epoll 是一种高效的 I/O 事件通知机制，
//  用于处理大量的并发 I/O 操作。
//  该函数为 fd 文件描述符设置了一些参数，
//  包括事件类型、触发模式、以及是否启用 EPOLLONESHOT。
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    // EPOLLIN：表示文件描述符准备好读取数据。
    //  EPOLLRDHUP：表示远程对端关闭了连接，适用于 TCP 连接。

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    // 事件触发后会自动移除该事件，直到文件描述符重新注册。
    if (one_shot)
        event.events |= EPOLLONESHOT;

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnoblocking(fd);
}

int http_conn::m_user_count = 0; //
int http_conn::m_epollfd = -1;   //

// 关闭连接 客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("closed %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGmode);
    m_user_count++;

    // 当浏览器出现重置时，可能是网站根目录出错或者是http响应格式出错或访问的文件中的内容完全为空
    doc_root = root;
    m_TRIGmode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接收的连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中获取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果中的列数
    int num_fileds = mysql_num_fields(result);

    // 返回左右字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行,将对应的用户名和密码存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 循环读取客户数据，直到无数据或对方关闭连接
// 非阻塞ET模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    // 如果 m_read_idx 超过或等于缓冲区大小，说明缓冲区已满
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT
    if (m_TRIGmode == 0)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }
        return true;
    }
    // ET
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    break;
                }
                return false;
            }
            else if (bytes_read == 0)
            {
                // 远程连接已经关闭
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

bool http_conn::write()
{
    LOG_INFO("write() bytes to send = %d",bytes_to_send);
    int temp = 0;

    // 没有数据需要发送了
    if (bytes_to_send == 0)
    {
        // epoll 事件重新设置为 EPOLLIN，准备接收新的数据
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGmode);
        init(); // 初始化连接状态
        return true;
    }

    while (1)
    {
        // 参数一：文件描述符
        // 参数二
        // 指向 iovec 结构体数组的指针。
        // 每个 iovec 结构体描述了一个数据缓冲区，包括起始地址和长度。
        // 参数三：iovec 结构体数组的个数，表示有多少个数据缓冲区需要写入
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGmode);
                return true;
            }
            LOG_ERROR("temp < 0");
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        // 根据已发送的字节数调整 iovec 结构中的缓冲区起始地址和长度。
        // 检查已发送的字节数是否大于或等于 m_iv[0].iov_len。这意味着第一个缓冲区的数据已经完全发送完了。
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            // 处理第一个缓冲区的数据已发送完的情况:
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 处理第一个缓冲区的数据未完全发送的情况:
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGmode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                LOG_ERROR("!m_linger");
                return false;
            }
        }
    }
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGmode);
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGmode);
}

http_conn::HTTP_CODE http_conn::process_read()
{

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    ////CHECK_STATE_CONTENT 请求体
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line(); // 请求行的开始位置 并且请求行结束位置已经被设置为\0
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text); // 输出请求行到日志

        // m_check_state初始化是请求行    （请求头  请求体）
        switch (m_check_state)
        {
            // 请求行
        case CHECK_STATE_REQUESTLINE:
        {
            // 解析请求行
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        // 请求头
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        // 请求体
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR; // 服务器内部发生错误，无法处理请求。通常对应于 500 Internal Server Error，可能是服务器逻辑问题或其他非预期错误。
        }
    }
    return NO_REQUEST; // 没有接收到完整的 HTTP 请求
}

// 从状态机 用于分析出一行请求
// 返回值为行的读取状态
http_conn::LINE_STATUS http_conn::parse_line()
{

    char temp;

    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            // 读取到回车
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            // 如果 \r 后紧跟 \n (换行)，将这两个字符替换为 \0，标识行结束，并返回 LINE_OK，表示行解析成功。
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 直接遇到\n
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析请求行 获得请求方法 目标URL 以及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 在 text 中查找第一个空格或制表符的位置，标志 URL 的开始位置。
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    // 将找到的空格替换为 \0，将方法字符串与 URL 分隔。
    *m_url++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    // 如果不是支持的请求方法，返回 BAD_REQUEST。
    else
        return BAD_REQUEST;

    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    // 跳过字符串 m_url 开头的所有空格和制表符，使 m_version 指向第一个非空白字符的位置。
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }
    // 检查 m_url 是否以 http:// 开头
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    // 检查 m_url 是否以 https:// 开头
    if (strncasecmp(m_url, "http://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    // 当URL显示为/时，显示判断界面
    if (strlen(m_url) == 1)
    {
        strcat(m_url, "judge.html");
    }
    m_check_state = CHECK_STATE_HEADER; // 当前正在解析 请求头部。
    return NO_REQUEST;                  // 没有接收到完整的 HTTP 请求
}

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 检查空行
    // 请求头部结束了
    if (text[0] == '\0')
    {
        // 如果 m_content_length 不为零，则表明请求有主体内容，
        // 需要进入内容解析状态 CHECK_STATE_CONTENT
        // 并返回 NO_REQUEST 继续处理请求主体。
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 如果 m_content_length 为零，
        // 则表示这是一个完整的 GET 请求，
        // 返回 GET_REQUEST，准备处理请求。
        return GET_REQUEST;
    }

    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        // 如果值为 "keep-alive"，设置 m_linger 为 true，表示客户端希望保持长连接。
        text += 11;
        text += strspn(text, " \t");

        LOG_INFO("text = %s", text);
        if (strcasecmp(text, "keep-alive") == 0)
        {
            // printf("m_linger = true;");
            LOG_INFO("%s", "m_linger ==== true;");
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknown headers:%s", text);
    }
    return NO_REQUEST; // 没有接收到完整的 HTTP 请求
}

// 解析请求体
// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 处理请求
http_conn::HTTP_CODE http_conn::do_request()
{
    // 将初始化的m_real_file初始化为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    // 找到m_url中/的位置
    const char *p = strrchr(m_url, '/');

    // 实现登录和注册校验
    // 2 登录校验 3 注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        // POST /2CGISQL.cgi HTTP/1.1
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // usr = wyp&password = 123456
        char name[100], passwd[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
        {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
        {
            passwd[j] = m_string[i];
        }
        passwd[j] = '\0';

        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 如果没有重名的，增加数据

            // 存储SQL插入语句
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username,passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "','");
            strcat(sql_insert, passwd);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                // 没找到
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, passwd));
                m_lock.unlock();
                // 返回0表示插入成功
                if (!res)
                {
                    strcpy(m_url, "/log.html");
                }
                else
                {
                    strcpy(m_url, "/registerError.html");
                }
            }
            else
            {
                strcpy(m_url, "/registerError.html");
            }
        }
        // 如果是登录 直接判断
        // 如果浏览器端输入的用户名和密码在表中可以找到 返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == passwd)
            {
                strcpy(m_url, "/welcome.html");
            }
            else
            {
                strcpy(m_url, "/logError.html");
            }
        }
    }
    // 请求资源/0 表示跳转注册页面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 请求资源/1 表示跳转登录页面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 图片请求页面？
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 视频请求页面?
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 关注请求页面
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 以上都不是
    // 直接使用 m_url 拼接到 m_real_file 中
    else
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    // 失败返回-1
    // 检查文件状态
    if (stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_REQUEST;
    }
    // 其他用户（other）”对文件的读取权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // 这段代码检查文件是否是一个目录。
    // 如果 m_file_stat.st_mode 表示一个目录 (S_ISDIR 为真)，
    // 则返回BAD_REQUEST，因为期望访问的是文件，而不是目录。
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    // 0: 映射的起始地址（NULL 表示由系统决定）。
    // m_file_stat.st_size: 映射的字节大小，即文件的大小。
    // PROT_READ: 表示映射内存的保护权限为只读。
    // MAP_PRIVATE: 创建一个私有的映射，写入不会影响文件本身。
    // fd: 文件描述符，用于指定要映射的文件。
    // 0: 文件映射的偏移量，从文件的开头开始映射。
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        // 设置状态行
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            LOG_INFO("bytes_to_send = %d",bytes_to_send);
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    LOG_INFO("bytes_to_send1 = %d",bytes_to_send);
    return true;
}

// 添加响应行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加响应头
bool http_conn::add_headers(int content_length)
{
    return add_content_length(content_length) && add_linger() && add_blank_line();
}

// 添加响应体
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 添加一行响应
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    // 声明一个 va_list 类型的变量 arg_list，用于存储变长参数。
    va_list arg_list;
    // 初始化 arg_list，使其指向变长参数的起始位置。
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len > WRITE_BUFFER_SIZE - 1 - m_write_idx)
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("requests:%s", m_write_buf);
    return true;
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_content_length(int content_length)
{
    return add_response("Content-length:%d\r\n", content_length);
}

// 添加要不要保持连接
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

#ifndef _HTTP_H_
#define _HTTP_H_

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#include <fcntl.h>    // 用于文件控制操作，包括 open 函数
#include <unistd.h>   // 提供 close、read、write 等系统调用
#include <sys/mman.h> // 提供 mmap 函数的定义
using namespace std;
class http_conn
{
public:
    enum METHOD
    {
        GET = 0, // 请求服务器返回指定资源
        POST,    // 向服务器发送数据
        HEAD,    // 类似于 GET 请求，但只请求资源的元信息（头部），不返回资源主体。常用于测试资源是否存在。
        PUT,     // 上传指定资源到服务器，通常用于更新资源或上传文件。
        DELETE,  // 请求服务器删除指定资源。
        TRACE,   // 回显服务器收到的请求，主要用于诊断。它的安全风险较高，通常不启用。
        OPTIONS, // 请求服务器返回支持的 HTTP 方法，通常用于跨域请求或探测服务器功能。
        CONNECT, // 用于建立隧道连接，通常用于代理服务器将 SSL 隧道连接到服务器。
        PATH     // 这是一个错误的拼写，应为 PATCH，用于部分更新资源。
    };
    // HTTP 请求解析过程中检查状态的枚举类型，每个状态表示解析的不同阶段。以下是每个状态的解释：
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, // 当前正在解析 请求行。
        CHECK_STATE_HEADER,          // 当前正在解析 请求头部。
        CHECK_STATE_CONTENT          // 当前正在解析请求体。
    };

    enum HTTP_CODE
    {
        NO_REQUEST,        // 没有接收到完整的 HTTP 请求
        GET_REQUEST,       // 成功接收到并解析了一个完整的 GET 请求
        BAD_REQUEST,       // 请求不符合 HTTP 协议标准，解析失败
        NO_RESOURCE,       // 表示请求的资源不存在。通常对应于 404 Not Found 错误，客户端请求的文件或路径在服务器上找不到。
        FORBIDDEN_REQUEST, // 表示请求被禁止访问。通常对应于 403 Forbidden 错误，客户端尝试访问受限制的资源或权限不足。
        FILE_REQUEST,      // 请求的资源是一个文件
        INTERNAL_ERROR,    // 服务器内部发生错误，无法处理请求。通常对应于 500 Internal Server Error，可能是服务器逻辑问题或其他非预期错误。
        CLOSED_CONNECTION  // 客户端关闭了连接，服务器无需继续处理请求
    };
    // 用于表示 HTTP 请求行或请求头行在解析过程中的状态。
    enum LINE_STATUS
    {
        LINE_OK = 0, // 表示成功读取并解析了一行数据，该行的格式是正确的。
        LINE_BAD,    // 表示读取到的行存在格式错误，解析失败
        LINE_OPEN    // 表示当前行数据不完整，可能需要继续读取数据以完成解析
    };

public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;

    http_conn() {}
    ~http_conn() {}

public:
    //WebServer::timer调用
    void init(int sockfd, const sockaddr_in &addr, char *root, int conntri_mode, int close_log, string user, string passwd, string fatabasename);
    void initmysql_result(connection_pool *connPool);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();//dealwithwrite调用
    int improv;
    int timer_flag;
    // 返回这个用户的sockaddr_in对象
    sockaddr_in *get_address()
    {
        return &m_address;
    }

public:
    static int m_user_count; // 连接的用户数量
    static int m_epollfd;
    MYSQL *mysql;
    int m_state; // 读为0 写为1

private:
    void init();
    HTTP_CODE process_read();          // 处理请求
    bool process_write(HTTP_CODE ret); //
    char *get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();                 // 处理请求行
    HTTP_CODE parse_request_line(char *text); // 解析请求行
    HTTP_CODE parse_headers(char *text);      // 解析请求头
    HTTP_CODE parse_content(char *text);      // 解析请求体
    HTTP_CODE do_request();                   // 处理请求
    
    void unmap();//write调用
    bool add_status_line(int status,const char * title);
    bool add_headers(int content_length);
    bool add_content(const char * content);
    bool add_response(const char * format,...);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();


private:
    int m_sockfd;
    sockaddr_in m_address;

    int m_read_idx;                    // 读数据
    char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
    int m_write_idx;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_checked_idx;
    int m_start_line;
    METHOD m_method; // 请求的方法GET OR POST
    int cgi;         // 是否启用的POST   POST报文 cgi设置为1
    char *m_string;  // 存储请求体数据
    int m_content_length;
    struct stat m_file_stat; // 指向 struct stat 结构的指针，用于存储获取到的文件信息。

    CHECK_STATE m_check_state;

    char *m_url; // 目标URL
    char *m_version;
    char *m_host;

    int bytes_to_send;   // 要发送多少数据
    int bytes_have_send; // 已经发送了多少数据
    char *doc_root;
    char m_real_file[FILENAME_LEN];
    int m_linger;         // 如果http请求是keep-alive设置为true
    char *m_file_address; // 文件的数据映射到内存中的地址

    //char *m_file_address;
    // writev函数的第2 3个参数
    struct iovec m_iv[2];
    int m_iv_count;

    map<string, string> m_users;
    int m_TRIGmode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
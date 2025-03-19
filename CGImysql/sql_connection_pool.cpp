#include "sql_connection_pool.h"

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

// 构造初始化
void connection_pool::init(string url, string user, string passwd, string databasename, int port, int max_conn, int close_log)
{
    m_url = url;//"localhost"
    m_Port = port;//3306
    m_User = user;
    m_PassWord = passwd;
    m_DatabaseName = databasename;
    m_close_log = close_log;

    for (int i = 0; i < max_conn; i++)
    {
        MYSQL *con = NULL;
        con = mysql_init(con); // 使用 mysql_init 函数初始化 MySQL 连接。

        if (con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }

        // 建立与数据库的实际连接
        con = mysql_real_connect(con, url.c_str(), user.c_str(), passwd.c_str(), databasename.c_str(), port, NULL, 0);

        if (con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }

        connList.push_back(con);
        ++m_FreeConn;
    }

    reserve = sem(m_FreeConn);
    m_MaxConn = m_FreeConn;
}
// 当有请求 从数据库连接池中返回一个可用连接 更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con = NULL;

    if (0 == connList.size())
    {
        return NULL;
    }

    //reserve 是一个信号量，表示空闲连接的数量。
    //调用 reserve.wait() 会阻塞当前线程，直到有空闲连接可用（信号量大于 0）。
    reserve.wait();
    lock.lock();

    con = connList.front();

    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;
    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (NULL == con)
        return NULL;

    lock.lock();

    connList.push_back(con);

    ++m_CurConn;
    --m_FreeConn;
    lock.unlock();

    reserve.post();

    return true;
}

connection_pool::~connection_pool()
{
    DestroyPool();
}

// 销毁数据库连接池
void connection_pool::DestroyPool()
{
    lock.lock();

    if(connList.size() > 0){
        list<MYSQL *>::iterator it;
        //遍历并关闭每个 MySQL 连接：
        for(it = connList.begin();it != connList.end();it++){
            MYSQL * con = *it;
            mysql_close(con);
        }

        m_CurConn = 0;
        m_FreeConn = 0;

        connList.clear();
    }
    lock.unlock();
}

// 当前空闲连接数
int connection_pool::GetfreeConn()
{
    return this->m_FreeConn;
}

connectionRAII::connectionRAII(MYSQL **con, connection_pool *connPool)
{
    *con = connPool->GetConnection();

    conRAII = *con;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}

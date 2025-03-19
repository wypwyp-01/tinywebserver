#ifndef _SQL_CONNECTION_POOL_H_
#define _SQL_CONNECTION_POOL_H_

#include<list>
#include<stdio.h>
#include<error.h>
#include<string.h>
#include <mysql/mysql.h>
#include<string>
#include<iostream>
#include"../lock/locker.h"
#include"../log/log.h"
using namespace std;

class connection_pool{
public:
    void DestroyPool();
    int GetfreeConn();

    MYSQL * GetConnection();
    bool ReleaseConnection(MYSQL * conn);
    //单例模式
    static connection_pool * GetInstance();

    void init(string url,string user,string passwd,string databasename,int port,int max_conn,int close_log);




private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;//最大连接数
    int m_CurConn;//当前已使用的连接数
    int m_FreeConn;//当前空闲连接数
    locker lock;
    list<MYSQL *> connList;//连接池
    sem reserve;

public:
    string m_url;//主机地址
    string m_Port;//数据库端口号
    string m_User;//登录数据库用户名
    string m_PassWord;//登录数据库密码
    string m_DatabaseName;//使用数据库名
    int m_close_log;//日志开关

};

class connectionRAII{
public:
    connectionRAII(MYSQL ** sql,connection_pool * connPool);
    ~connectionRAII();
private:
    MYSQL *conRAII;
    connection_pool * poolRAII;
};

#endif
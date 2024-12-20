#include "http_conn.h"

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

void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname, Session *session)
{
    m_sockfd = sockfd;
    m_address = addr;
    m_session_manager = session;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接受的连接
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
    m_session_id = "";

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
    memset(file_list_html, '\0', 8192);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的一个头部信息
/*
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
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
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}
*/
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
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
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");

        // 如果Content-Type是multipart/form-data
        if (strncasecmp(text, "multipart/form-data", 19) == 0)
        {
            text += 19;
            text += strspn(text, " \t");

            // 跳过分号和空格
            text += strspn(text, "; \t");

            if (strncasecmp(text, "boundary=", 9) == 0)
            {
                text += 9;
                text += strspn(text, " \t");

                // 复制boundary到m_boundary
                strcpy(m_boundary, text);
                std::cout << std::endl;
                std::cout << 3 << std::endl;
                std::cout << m_boundary << std::endl;
            }
        }
    }
    // 处理 Cookie 字段，提取 session_id
    else if (strncasecmp(text, "Cookie:", 7) == 0)
    {
        text += 7;
        text += strspn(text, " \t");

        // 查找 session_id
        const char *session_id_start = strstr(text, "session_id=");
        if (session_id_start)
        {
            session_id_start += 11; // 跳过 "session_id=" 字段

            const char *session_id_end = strchr(session_id_start, ';');
            if (session_id_end == nullptr)
            {
                session_id_end = text + strlen(text); // 如果没有找到分号，直到字符串结束
            }

            size_t session_id_length = session_id_end - session_id_start;
            m_session_id.assign(session_id_start, session_id_length); // 保存 session_id
            m_username = m_session_manager->get_username(m_session_id);
            std::cout << m_username << std::endl;
            std::cout << "Session ID: hhh " << m_session_id << std::endl;
        }
    }
    else
    {
        LOG_INFO("oop!unknown header: %s", text);
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{

    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        std::cout << text << std::endl;
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
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
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    // 判断是否是登录
    char ppp;
    // std::cout  << "**********1*********" << std::endl;
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    // 处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来s
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
            {
                ppp = *(p + 1);
                strcpy(m_url, "/welcome.html");
                string session_id = m_session_manager->generate_session_id();
                m_session_manager->create_session(session_id, name);
                m_session_id = session_id;
                std::cout << "session_id::::::::" << session_id << std::endl;
            }
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        std::cout << m_real_file << std::endl;

        free(m_url_real);
    }
    // 处理文件上传
    else if (*(p + 1) == '8')
    {
        std::cout << "*******************" << std::endl;

        // 获取boundary的长度
        size_t boundary_len = strlen(m_boundary);

        // 获取请求体的长度
        size_t body_len = strlen(m_string);

        // 指向请求体开始的位置
        const char *body = m_string;

        // 文件存储路径
        const char *upload_dir = "./root/file_list/";

        // 文件解析的起始位置
        const char *file_start = body;
        const char *file_end = nullptr;

        time_t current_time = time(nullptr);
        struct tm *local_time = localtime(&current_time);
        char upload_time[20];
        strftime(upload_time, sizeof(upload_time), "%Y-%m-%d %H:%M:%S", local_time);
        std::cout << upload_time << std::endl;

        // 获取当前用户名
        std::string temp_username = m_session_manager->get_username(m_session_id);
        SnowflakeIDGenerator snowtemp(1);
        std::string snow_num = snowtemp.generateID();
        std::cout << snow_num << std::endl;

        std::string log_num = snowtemp.generateID();

        // 循环查找文件数据块
        while ((file_start = strstr(file_start, m_boundary)) != nullptr)
        {
            // 跳过当前的boundary
            file_start += boundary_len;

            // 查找 Content-Disposition
            const char *content_disposition = strstr(file_start, "Content-Disposition:");
            if (content_disposition)
            {
                content_disposition += 19; // 跳过 "Content-Disposition: "

                // 查找文件名
                const char *filename_start = strstr(content_disposition, "filename=\"");
                if (filename_start)
                {
                    filename_start += 10; // 跳过 "filename=\""
                    const char *filename_end = strstr(filename_start, "\"");
                    if (filename_end)
                    {
                        // 提取文件名
                        size_t filename_len = filename_end - filename_start;
                        char filename[256] = {0};
                        strncpy(filename, filename_start, filename_len);

                        std::string query = "SELECT EXISTS(SELECT 1 FROM file WHERE file_name = '"+std::string(filename)+"');";
                        m_lock.lock();
                        if(mysql_query(mysql,query.c_str())){
                            std::cerr<<"mysql query error"<<mysql_errno(mysql)<<std::endl;
                        }
                        m_lock.unlock();
                        MYSQL_RES *res = mysql_store_result(mysql);
                        if(res == NULL){
                            std::cerr<<"store result error"<<mysql_error(mysql)<<std::endl;
                        }
                        MYSQL_ROW row = mysql_fetch_row(res);
                        if(row&&row[0]){
                            if(atoi(row[0]) == 1){
                                return FILE_EXIST;
                            }
                        }

                        // 生成文件保存路径
                        char file_path[512] = {0};
                        snprintf(file_path, sizeof(file_path), "%s%s", upload_dir, filename);

                        // 查找文件内容的起始位置（在\r\n\r\n之后）
                        const char *file_data_start = strstr(filename_end, "\r\n\r\n") + 4;
                        const char *file_data_end = strstr(file_data_start, m_boundary);

                        // 如果找到了文件数据结束的boundary
                        if (file_data_end)
                        {
                            size_t file_data_size = file_data_end - file_data_start;

                            // 打开文件并写入数据
                            FILE *file = fopen(file_path, "wb");
                            if (file)
                            {
                                fwrite(file_data_start, 1, file_data_size, file);
                                fclose(file);
                                std::cout << "文件上传成功，保存为：" << file_path << std::endl;

                                // 生成插入数据库的 SQL 语句
                                char *sql_insert = (char *)malloc(sizeof(char) * 512);
                                snprintf(sql_insert, 512,
                                         "INSERT INTO file (id, user, uploadtime, file_path,file_name, download) "
                                         "VALUES ('%s', '%s', '%s','%s','%s', 0)",
                                         snow_num.c_str(), temp_username.c_str(), upload_time, file_path,filename);

                                char *sql_insert_log = (char*)malloc(sizeof(char)*512);
                                snprintf(sql_insert_log,512,"INSERT INTO log (id,operator,file_id,operate_time,operate_type)"
                                        "VALUES ('%s','%s','%s','%s','upload')",
                                        log_num.c_str(),temp_username.c_str(),snow_num.c_str(),upload_time);

                                // 将文件信息插入到数据库
                                std::cout << snow_num << std::endl;
                                m_lock.lock(); // 锁定数据库操作
                                // int res = mysql_query(mysql, sql_insert);
                                mysql_query(mysql,"START TRANSACTION");
                                if (mysql_query(mysql, sql_insert))
                                {
                                    std::cerr << "Error: " << mysql_errno(mysql) << " - " << mysql_error(mysql) << std::endl;
                                    mysql_query(mysql,"ROLLBACK");
                                    exit(1);
                                }
                                else if (mysql_query(mysql, sql_insert_log))
                                {
                                    std::cerr << "Error: " << mysql_errno(mysql) << " - " << mysql_error(mysql) << std::endl;
                                    mysql_query(mysql,"ROLLBACK");
                                    exit(1);
                                }
                                else {
                                    mysql_query(mysql,"COMMIT");
                                }
                                m_lock.unlock(); // 解锁数据库操作
                                free(sql_insert_log);
                                free(sql_insert);
                                /*
                                if (res)
                                {
                                    std::cerr << "数据库插入失败！" << std::endl;
                                }
                                else
                                {
                                    std::cout << "文件信息已成功插入数据库！" << std::endl;
                                }
                                */

                                break;
                            }
                            else
                            {
                                std::cerr << "文件保存失败！" << std::endl;
                            }
                        }
                    }
                }
            }

            // 继续查找下一个boundary
            file_start = file_end;
        }
        std::cout << "文件上传成功，保存为：" << "*******************" << std::endl;
        // 发送HTTP响应给浏览器
        return FILE_UPLOAD;
    }

    // 返回给前端文件列表
    else if (*(p + 1) == '9')
    {
        // 设定目录路径
        const char *directory_path = "./root/file_list";
        char file_list_htmll[8192]; // 用于存放生成的 HTML 文件列表

        // 打开指定目录
        DIR *dir = opendir(directory_path);
        if (dir == nullptr)
        {
            std::cerr << "Failed to open directory: " << directory_path << std::endl;
            return NO_RESOURCE;
        }

        // HTML 页面头部
        const char *header = "<!DOCTYPE html><html lang=\"zh-CN\">"
                             "<head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
                             "<title>文件列表 - 文件管理系统</title>"
                             "<style>"
                             "body { font-family: 'Arial', sans-serif; background-color: #f4f7fc; margin: 0; padding: 0; }"
                             ".container { width: 100%; height: 100vh; display: flex; flex-direction: column; justify-content: center; align-items: center; text-align: center; }"
                             "h1 { font-size: 36px; margin-bottom: 30px; color: #333; }"
                             ".file-list { display: flex; flex-direction: column; gap: 20px; width: 300px; }"
                             ".file-list button { padding: 10px 20px; font-size: 18px; color: #fff; background-color: #007bff; border: none; border-radius: 5px; cursor: pointer; transition: background-color 0.3s; }"
                             ".file-list button:hover { background-color: #0056b3; }"
                             ".footer { margin-top: 30px; color: #999; font-size: 14px; }"
                             "</style>"
                             "</head><body><div class=\"container\"><h1>文件列表</h1>";

        // 初始化缓冲区
        snprintf(file_list_htmll, sizeof(file_list_htmll), "%s", header);

        // 计算剩余空间
        size_t remaining_space = sizeof(file_list_htmll) - strlen(file_list_htmll) - 1;

        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            // 排除特殊目录 "." 和 ".."
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            // 如果剩余空间不足，停止添加文件
            if (remaining_space <= 0)
            {
                std::cerr << "Buffer overflow detected!" << std::endl;
                break;
            }

            // 生成每个文件的按钮，action 修改为 'a'
            char file_entry[512];
            snprintf(file_entry, sizeof(file_entry),
                     "<form method=\"post\" action=\"a\">"
                     "<input type=\"hidden\" name=\"filename\" value=\"%s\">"
                     "<button type=\"submit\">%s</button>"
                     "</form>",
                     entry->d_name, entry->d_name);

            // 更新剩余空间
            size_t file_entry_len = strlen(file_entry);
            remaining_space -= file_entry_len;

            // 拼接文件项到缓冲区
            strncat(file_list_htmll, file_entry, remaining_space);
        }

        // HTML 页面尾部
        const char *footer = "<div class=\"footer\"><p>© 2024 文件管理系统</p></div></body></html>";
        remaining_space -= strlen(footer);

        if (remaining_space > 0)
        {
            strncat(file_list_htmll, footer, remaining_space);
        }

        closedir(dir); // 关闭目录

        strcpy(file_list_html, file_list_htmll);
        return FILE_DIR_REQUEST;
    }

    else if (*(p + 1) == 'a')
    {
        char filepath[100];
        int j = 0;
        for (int i = 9; m_string[i] != '\0'; j++, i++)
        {
            filepath[j] = m_string[i];
        }
        filepath[j] = '\0';
        strcpy(m_file_name, filepath);
        string query = "SELECT id FROM file WHERE file_name ='"+string(m_file_name)+"';";
        m_lock.lock();
        if(mysql_query(mysql,query.c_str())){
            std::cerr<<"mysql query file_id error"<<mysql_error(mysql)<<std::endl;
        }
        m_lock.unlock();
        MYSQL_RES* res = mysql_store_result(mysql);
        if(res == NULL){
            std::cerr<<"mysql res error"<<mysql_error(mysql)<<std::endl;
        }
        MYSQL_ROW row = mysql_fetch_row(res);
        string file_id;
        if(row&&row[0]){
            file_id = row[0];
        }
        SnowflakeIDGenerator snowtemp(1);
        std::string snow_num = snowtemp.generateID();

        time_t current_time = time(nullptr);
        struct tm *local_time = localtime(&current_time);
        char download_time[20];
        strftime(download_time,sizeof(download_time),"%Y-%m-%d %H:%M:%S",local_time);

        string username = m_session_manager->get_username(m_session_id);

        char* sql_insert_log = (char*)malloc(sizeof(char)*512);
        snprintf(sql_insert_log,512,"INSERT INTO log (id,file_id,operator,operate_time,operate_type)"
            "VALUES('%s','%s','%s','%s','download')",
            snow_num.c_str(),file_id.c_str(),username.c_str(),download_time);

        string sql_update = "UPDATE file SET download=download + 1 WHERE id = '"+file_id+"';";

        m_lock.lock();
        mysql_query(mysql,"START TRANSACTION");

        if(mysql_query(mysql,sql_insert_log)){
            std::cerr<<"mysql insert log error"<<mysql_error(mysql)<<std::endl;
            mysql_query(mysql,"ROLLBACK");
        }
        else if(mysql_query(mysql,sql_update.c_str())){
            std::cerr<<"mysql update error"<<mysql_error(mysql)<<std::endl;
            mysql_query(mysql,"ROLLBACK");
        }
        else {
            mysql_query(mysql,"COMMIT");
        }
        m_lock.unlock();
        free(sql_insert_log);
        char *m_real_url = (char *)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/file_list/");
        strcat(m_real_url, filepath);
        strncpy(m_real_file + len, m_real_url, strlen(m_real_url));
        std::cout << "download file: " << m_real_file << std::endl;
        free(m_real_url);
        if (stat(m_real_file, &m_file_stat) < 0)
            return NO_RESOURCE;

        if (!(m_file_stat.st_mode & S_IROTH))
            return FORBIDDEN_REQUEST;

        if (S_ISDIR(m_file_stat.st_mode))
            return BAD_REQUEST;

        int fd = open(m_real_file, O_RDONLY);
        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        return FILE_DOWNLOAD;
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ppp == '2')
        return LOG_SESSION;
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_session_header(const std::string &session_id)
{
    // 如果 session_id 为空，返回 false
    if (session_id.empty())
    {
        std::cerr << "Session ID is empty!" << std::endl;
        return false;
    }
    return add_response("Set-Cookie: session_id=%s; path=/; HttpOnly; Secure\r\n", session_id.c_str());
}

bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_headers(int content_len, const std::string &session_id)
{
    return add_content_length(content_len) && add_session_header(session_id) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_headers_download(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_content_disposition(m_file_name) && add_content_type_file() && add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_content_type(const char *type)
{
    return add_response("Content-Type:%s\r\n", type);
}
bool http_conn::add_content_type_file()
{
    return add_response("Content-Type:%s\r\n", "application/octet-stream");
}
bool http_conn::add_content_disposition(const char *filename)
{
    return add_response("Content-Disposition: attachment; filename=\"%s\"\r\n", filename);
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
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
    case FILE_DOWNLOAD:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers_download(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
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
    case LOG_SESSION:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size, m_session_id);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            std::cout << "LOG_SESSION" << m_write_buf << std::endl;
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
    case FILE_UPLOAD:
    {
        add_status_line(200, ok_200_title);
        char content[256] = "文件上传成功!";
        bool result = add_content_length(strlen(content)) && add_linger() && add_content_type("text/plain; charset=utf-8") && add_blank_line() && add_content(content);
        if (!result)
            return false;

        break;
        // bytes_to_send = m_write_idx;
    }
    case FILE_EXIST:
    {
        add_status_line(200, ok_200_title);
        char content[256] = "文件已存在服务器，请勿重复上传!";
        bool result = add_content_length(strlen(content)) && add_linger() && add_content_type("text/plain; charset=utf-8") && add_blank_line() && add_content(content);
        if (!result)
            return false;

        break;
        // bytes_to_send = m_write_idx;
    }
    case FILE_DIR_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (strlen(file_list_html) != 0)
        {
            add_headers(strlen(file_list_html));
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = file_list_html;
            m_iv[1].iov_len = strlen(file_list_html);
            m_iv_count = 2;
            bytes_to_send = m_write_idx + strlen(file_list_html);
            return true;
        }
        else
        {
            const char *ok_string = "<html><body><h1>No files available</h1></body></html>";
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
    std::cout << m_write_buf << std::endl
              << 111111111 << std::endl;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    std::cout << 1111111111111111111111 << std::endl;
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

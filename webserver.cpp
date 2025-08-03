#include "webserver.h"

#include <sys/sendfile.h>

WebServer::WebServer()
{
    //http_conn类对象
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::reactor_init() {
    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //epoll添加listenfd
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    //epoll添加统一事件源的pipefd
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    //信号处理
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    //超时机制
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::proactor_init() {
    // Initialize io_uring
    memset(&params, 0, sizeof(params));
    io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params);
    
    // io_uring 添加 accept 事件
    socklen_t len = sizeof(clientaddr);
    Utils::set_event_accept(&ring, m_listenfd, (struct sockaddr*)&clientaddr, &len, 0);

    // io_uring 处理 signal 事件: SIGPIPE，SIGALRM，SIGTERM
    signal(SIGPIPE, SIG_IGN);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    LOG_INFO("SIGALRM blocked? %d\n", sigismember(&mask, SIGALRM));

    sfd = signalfd(-1, &mask, 0);

    // 添加监听 signfd 的 poll 事件
    Utils::set_event_poll(&ring, sfd);

    // 超时机制
    alarm(TIMESLOT);
}

void WebServer::eventListen()
{
    http_conn::m_actormodel = m_actormodel;
    http_conn::ring = &ring;

    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // 创建 socket，绑定地址并且进行 listen
    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    if (m_actormodel == 1) {         // Reactor 使用 epoll 
        reactor_init();
    } else {                    // Proactor 使用 io_uring
        proactor_init();
    }
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    if (!timer) return;  // 防御性代码：只 delete 一次
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
    users_timer[sockfd].timer = nullptr;
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
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

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
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

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

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
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
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
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::reactor_epoll(bool &timeout, bool &stop_server) {

    int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
    if (number < 0 && errno != EINTR)
    {
        LOG_ERROR("%s", "epoll failure");
        stop_server = true;
        return;
    }

    for (int i = 0; i < number; i++)
    {
        int sockfd = events[i].data.fd;

        //处理新到的客户连接
        if (sockfd == m_listenfd)
        {
            bool flag = dealclientdata();
            if (false == flag)
                continue;
        }
        else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
        {
            //服务器端关闭连接，移除对应的定时器
            util_timer *timer = users_timer[sockfd].timer;
            deal_timer(timer, sockfd);
        }
        //处理信号
        else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
        {
            bool flag = dealwithsignal(timeout, stop_server);
            if (false == flag)
                LOG_ERROR("%s", "dealclientdata failure");
        }
        //处理客户连接上接收到的数据
        else if (events[i].events & EPOLLIN)
        {
            dealwithread(sockfd);
        }
        else if (events[i].events & EPOLLOUT)
        {
            dealwithwrite(sockfd);
        }
    }
    if (timeout)
    {
        utils.timer_handler();

        LOG_INFO("%s", "timer tick");

        timeout = false;
    }
}

void WebServer::proactor_uring(bool &timeout, bool &stop_server) {
    
    io_uring_submit(&ring);

    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(&ring, &cqe);

    struct io_uring_cqe *cqes[URING_EVENTS];
    int nready = io_uring_peek_batch_cqe(&ring, cqes, URING_EVENTS);

    for (int i = 0; i < nready; i ++) {
        struct io_uring_cqe *entries = cqes[i];
        struct conn_info *result = reinterpret_cast<conn_info*>(entries->user_data);
        // memcpy(&result, &entries->user_data, sizeof(struct conn_info));

        if (result->event == EVENT_TYPE::ACCEPT) {

            socklen_t len = sizeof(clientaddr);

            Utils::set_event_accept(&ring, m_listenfd, (struct sockaddr*)&clientaddr, &len, 0);
            
            int connfd = entries->res;
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                continue;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                continue;
            }
            LOG_INFO("Accept: %s, fd: %d", inet_ntoa(clientaddr.sin_addr), entries->res);
            timer(connfd, clientaddr);

            Utils::set_event_recv(&ring, connfd, users[connfd].m_read_buf, http_conn::READ_BUFFER_SIZE, 0);
            
        } else if (result->event == EVENT_TYPE::READ) {

            int ret = entries->res;
            users[result->fd].m_read_idx += ret;
            util_timer *timer = users_timer[result->fd].timer;

            if (ret > 0) {
                m_pool->append_p(users + result->fd);

                if (timer)
                {
                    adjust_timer(timer);
                }
            } else {
                deal_timer(timer, result->fd);
            }

        }  else if (result->event == EVENT_TYPE::WRITE) {

            int ret = entries->res;
            util_timer *timer = users_timer[result->fd].timer;

            if (ret > 0) {
            
                http_conn *cur_user = &users[result->fd];
                cur_user->bytes_have_send += ret;
                size_t bytes_remain = cur_user->m_write_idx - cur_user->bytes_have_send;

                // header 未发送完就继续发
                if (cur_user->bytes_have_send < cur_user->m_write_idx) {
                    
                    Utils::set_event_send(&ring, result->fd, result->file_fd, 
                        result->file_size, cur_user->m_write_buf + cur_user->bytes_have_send, bytes_remain, 0);

                    if (timer) 
                    {
                        adjust_timer(timer);
                    }
                    
                    continue;
                }

                LOG_INFO("WRITE to %d with header \n%s", result->fd, users[result->fd].m_write_buf);

                // header 发送完根据状态发送文件或者结束
                if (result->file_fd == -1) {

                    // Utils::set_event_recv(&ring, result->fd, users[result->fd].m_read_buf, http_conn::READ_BUFFER_SIZE, 0);
                    deal_timer(timer, result->fd); 
                    continue;
                
                } else {

                    off_t offset = 0;
                    Utils::sent_event_sendfile(&ring, result->fd, result->file_fd, offset, result->file_size);

                }
                
                if (timer) 
                {
                    adjust_timer(timer);
                }

            } else {
                deal_timer(timer, result->fd);
            }
            
        } else if (result->event == EVENT_TYPE::SPLICE_FILE_TO_PIPE) {

            if (entries->res < 0) {
                int err = -entries->res;
                if (err == EAGAIN) {
                    LOG_INFO("splice file→pipe got EAGAIN, will retry");

                    // 重新注册原来的 splice 请求
                    auto* sqe = io_uring_get_sqe(&ring);
                    size_t bytes_remaining = result->file_size - result->offset;
                    io_uring_prep_splice(sqe, result->file_fd, result->offset, result->pipe_fd[1], -1, bytes_remaining, 0);

                    auto* info = new conn_info {
                        .fd = result->fd,
                        .event = EVENT_TYPE::SPLICE_FILE_TO_PIPE,
                        .pipe_fd = {result->pipe_fd[0], result->pipe_fd[1]},
                        .offset = result->offset,
                        .file_fd = result->file_fd,
                        .file_size = result->file_size,
                        .pipe_buf_size = result->pipe_buf_size,
                    };
                    sqe->user_data = reinterpret_cast<__u64>(info);

                    continue;
                } else {
                    LOG_ERROR("splice file→pipe failed: %s", strerror(err));
                    // close_and_cleanup(result); TODO: 对错误的处理 ？
                    continue;
                }
            }

            LOG_DEBUG("FILE -> PIPE:   offset %d   to    %d", result->pipe_buf_size, result->pipe_buf_size + entries->res);

            // 已经从 file → pipe[1] 复制成功，下一步：pipe[0] → socket
            auto* sqe = io_uring_get_sqe(&ring);

            size_t bytes_remaining = result->file_size - result->offset;

            io_uring_prep_splice(sqe, result->pipe_fd[0], -1, result->fd, -1, bytes_remaining, 0);

            struct conn_info *info = new conn_info {
                .fd = result->fd,
                .event = EVENT_TYPE::SPLICE_PIPE_TO_SOCKET,
                .pipe_fd = {result->pipe_fd[0], result->pipe_fd[1]},
                .offset = result->offset,
                .file_fd = result->file_fd,
                .file_size = result->file_size,
                .pipe_buf_size = result->pipe_buf_size + entries->res,
            };

            sqe->user_data = reinterpret_cast<__u64>(info);
        
        } else if (result->event == EVENT_TYPE::SPLICE_PIPE_TO_SOCKET) {

            if (entries->res < 0) {
                int err = -entries->res;
                if (err == EAGAIN) {
                    LOG_INFO("splice pipe→socket got EAGAIN, will retry");

                    auto* sqe = io_uring_get_sqe(&ring);
                    size_t bytes_remaining = result->file_size - result->offset;
                    io_uring_prep_splice(sqe, result->pipe_fd[0], -1, result->fd, -1, bytes_remaining, 0);
                    auto* info = new conn_info {
                        .fd = result->fd,
                        .event = EVENT_TYPE::SPLICE_PIPE_TO_SOCKET,
                        .pipe_fd = {result->pipe_fd[0], result->pipe_fd[1]},
                        .offset = result->offset,
                        .file_fd = result->file_fd,
                        .file_size = result->file_size,
                        .pipe_buf_size = result->pipe_buf_size,
                    };
                    sqe->user_data = reinterpret_cast<__u64>(info);

                    delete result;
                    continue;
                } else {
                    LOG_ERROR("splice pipe→socket failed: %s", strerror(err));
                    // close_and_cleanup(result);
                    continue;
                }
            }
            
            LOG_DEBUG("PIPE -> SOCKET:   offset %d   to    %d", result->offset, result->offset + entries->res);

            result->offset += entries->res;  // 更新文件偏移

            if (result->offset >= result->file_size) {    // 文件发送完成，清理资源
                
                close(result->file_fd);
                close(result->pipe_fd[0]);
                close(result->pipe_fd[1]);
                if (users[result->fd].m_linger) {
                    users[result->fd].init();
                }

                // 注册下次 read
                Utils::set_event_recv(&ring, result->fd, users[result->fd].m_read_buf, http_conn::READ_BUFFER_SIZE, 0);

            } else if (result->offset < result->pipe_buf_size) {      // pipe 还有数据，继续 pipe → socket

                auto* sqe = io_uring_get_sqe(&ring);
                io_uring_prep_splice(sqe, result->pipe_fd[0], -1, result->fd, -1, result->pipe_buf_size, 0);
                auto* info = new conn_info{
                    .fd = result->fd,
                    .event = EVENT_TYPE::SPLICE_PIPE_TO_SOCKET,
                    .pipe_fd = {result->pipe_fd[0], result->pipe_fd[1]},
                    .offset = result->offset,
                    .file_fd = result->file_fd,
                    .file_size = result->file_size,
                    .pipe_buf_size = result->pipe_buf_size,
                };
                sqe->user_data = reinterpret_cast<__u64>(info);
                
            } else {                                    // pipe 清空了，文件没发完，file → pipe   

                // 继续下一轮 splice（file → pipe）
                size_t bytes_remaining = result->file_size - result->offset;

                auto* sqe = io_uring_get_sqe(&ring);
                io_uring_prep_splice(sqe, result->file_fd, result->offset, result->pipe_fd[1], -1, bytes_remaining, 0);
                struct conn_info *info = new conn_info {
                    .fd = result->fd,
                    .event = EVENT_TYPE::SPLICE_FILE_TO_PIPE,
                    .pipe_fd = {result->pipe_fd[0], result->pipe_fd[1]},
                    .offset = result->offset,
                    .file_fd = result->file_fd,
                    .file_size = result->file_size,
                    .pipe_buf_size = result->pipe_buf_size,
                };
                // io_uring_sqe_set_data(sqe, info);
                sqe->user_data = reinterpret_cast<__u64>(info);

            }
        
        } else if (result->event == EVENT_TYPE::SIGNAL) {

            struct signalfd_siginfo fdsi;
            read(sfd, &fdsi, sizeof(fdsi));

            if (fdsi.ssi_signo == SIGALRM) {
                
                LOG_INFO("SIGNAL\n");
                timeout = true;

            } else if (fdsi.ssi_signo == SIGTERM) {

                stop_server = true;

            }

            Utils::set_event_poll(&ring, sfd);
        }

        // delete result;
	}

	io_uring_cq_advance(&ring, nready);
}


void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        if (m_actormodel == 1) {     
            reactor_epoll(timeout, stop_server);
        } else {                
            proactor_uring(timeout, stop_server);   
        }

        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}

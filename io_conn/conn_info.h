#pragma once

enum class EVENT_TYPE {
    ACCEPT,   	
    READ,
    WRITE,      // write header 和 sendfile 都使用 WRITE 类型，当 file_fd == -1 的时候重新注册读事件
    SIGNAL,
    SPLICE_FILE_TO_PIPE,
    SPLICE_PIPE_TO_SOCKET
};

struct conn_info {
	int fd;
	EVENT_TYPE event;

    int pipe_fd[2];         // pipe[0]=read, pipe[1]=write
    int offset;
    int file_fd = -1;       // 默认是 -1 , 如果不为 -1 表示不用发送文件 
    off_t file_size;
    int pipe_buf_size;
};

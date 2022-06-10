#define _GNU_SOURCE 1
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <poll.h>
#include <sys/epoll.h>


#define EPOLLMAX 1024

#define BUFSIZE 10240

#define MAXLINE 1024


typedef struct
{
    int rio_fd;
    int rio_cnt;            // the length of rio_buf
    char *rio_buf_ptr;      // point unread pos
    char rio_buf[BUFSIZE];
} rio_t;

typedef enum 
{
    GET = 0,
    POST,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATH
} REQUEST_METHOD;

typedef struct
{
    REQUEST_METHOD method;
    
    int keep_alive;
    int content_length;
    char *uri;
    int uri_dir;    // 请求的uri是目录还是文件
    char path[30];
} request_t;

typedef struct
{
    char *content;
    int content_length;
    int status_code;
    char *msg;
} response_t;


typedef enum
{
    PROCESS_READ = 0,
    PROCESS_PARSE_HEADERS,
    PROCESS_PARSE_BODY,
    PROCESS_END,
    PROCESS_ERR
} PROCESS;

void format_size(char *buf, struct stat *stat)
{
    if (S_ISDIR(stat->st_mode))
    {
        sprintf(buf, "%s", "[DIR]");
    }
    else
    {
        off_t size = stat->st_size;
        if (size < 1024)
        {
            sprintf(buf, "%luB", size);
        }
        else if (size < 1024 * 1024)
        {
            sprintf(buf, "%.1fK", (double)size / 1024);
        }
        else if (size < 1024 * 1024 * 1024)
        {
            sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
        }
        else
        {
            sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
        }
    }
}

int check_uri(request_t *request, response_t *response)
{
    
    sprintf(request->path, ".%s", request->uri);

    struct stat filestat;
    if(stat(request->path, &filestat)==-1)
    {
        response->status_code = 404;
        return 1;
    }

    int path_fd = open(request->path, O_RDONLY, 0);

    response->status_code = 200;
    // 文件下载
    if(!S_ISDIR(filestat.st_mode))
    {
        response->content_length = filestat.st_size;
        request->uri_dir = 0;
    }else{
        response->content = (char *)malloc(BUFSIZE);
        memset(response->content, 0, BUFSIZE);

        request->uri_dir = 1;
        strcat(response->content, "<html><head><style>"
                                "body{font-family: monospace; font-size: 13px;}"
                                "td {padding: 1.5px 6px;}"
                                "</style></head><body><table>\n");
        char m_time[32], size[16], content_buf[MAXLINE];

        DIR *d = fdopendir(path_fd);
        struct dirent *dp;
        int ffd;
        while((dp=readdir(d))!=NULL)
        {
            if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) continue;
            if((ffd=openat(path_fd, dp->d_name, O_RDONLY))==-1)
            {
                perror(dp->d_name);
                continue;
            }
            fstat(ffd, &filestat);
            strftime(m_time, sizeof(m_time), "%Y-%m-%d %H:%M", localtime(&filestat.st_mtime));
            format_size(size, &filestat);
            if(S_ISREG(filestat.st_mode) || S_ISDIR(filestat.st_mode))
            {
                const char *d = S_ISDIR(filestat.st_mode) ? "/" : "";
                sprintf(content_buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n", dp->d_name, d, dp->d_name, d, m_time, size);
                strcat(response->content, content_buf);

            }
            close(ffd);
        }
        closedir(d);
        strcat(response->content, "</table></body></html>");
        response->content_length = strlen(response->content);
    }

    close(path_fd);
    return 1;
}

void noblocksleep(int us)
{
    struct timeval timev;
    timev.tv_sec = 0;
    timev.tv_usec = us;
    select(0, NULL, NULL, NULL, &timev);
}

int read_all(rio_t *rio)
{
    int buf_len = sizeof(rio->rio_buf);
    int read_size;
    while(1)
    {
        read_size = read(rio->rio_fd, rio->rio_buf+rio->rio_cnt, buf_len-rio->rio_cnt);
        if(read_size<0)
        {
            if((errno==EAGAIN) || (errno==EWOULDBLOCK))
            {
                break;
            }else{
                continue;
            }
        }else if(read_size==0)
        {
            return -1;
        }
        rio->rio_cnt += read_size;
    }
}

PROCESS rio_read(rio_t *rio)
{
    int read_size;
    char *p = rio->rio_buf_ptr;
    int count = 0;
    while(1)
    {   
        count ++;
        read_size=read(rio->rio_fd, p, sizeof(rio->rio_buf)-(p-rio->rio_buf));
        if(read_size<=0)
        {
            if(read_size==0)
            {
                return PROCESS_ERR;
            }
            else if(errno==EAGAIN)
            {
                
                noblocksleep(count*100);
                if(count==100) {
                    printf("try to read %d times fail!\n", count);
                    return PROCESS_ERR;
                }
                continue;
            }
            return PROCESS_ERR;
        }else{
            while(*p!='\n' && *p!=0) ++p;
            
            if(*p==0) continue;
            else break;
        }
    }
    return PROCESS_PARSE_HEADERS;
}

void rio_init(int fd, rio_t *rio)
{
    rio->rio_buf_ptr = rio->rio_buf;
    rio->rio_cnt = 0;
    rio->rio_fd = fd;
    memset(rio->rio_buf, 0, BUFSIZE);
}

int parse_requestline(rio_t *rio, request_t *request)
{
    
    if(strncasecmp(rio->rio_buf_ptr, "GET", 3)==0)
    {
        request->method = GET;
    }else if(strncasecmp(rio->rio_buf_ptr, "POST", 4)==0)
    {
        request->method = POST;
    }else{
        return 1;
    }

    while(*rio->rio_buf_ptr++!=' ');

    char *p = rio->rio_buf_ptr;
    while(*p++!=' ');
    *(p-1) = 0;
    int sizeofpath = strlen(rio->rio_buf_ptr);
    request->uri = (char *)malloc(sizeofpath+1);

    strcpy(request->uri, rio->rio_buf_ptr);
    rio->rio_buf_ptr = p;
    
    rio->rio_buf_ptr = strpbrk(rio->rio_buf_ptr, "\r\n");
    rio->rio_buf_ptr += 2;
    return 3;
}

PROCESS parse_requestheader(rio_t *rio, request_t *request)
{
    char *p;
    while(1)
    {
        if(*rio->rio_buf_ptr=='\r') return PROCESS_PARSE_BODY;
        else if(strncasecmp(rio->rio_buf_ptr, "GET", 3)==0 || strncasecmp(rio->rio_buf_ptr, "POST", 4)==0)
        {
            if(strncasecmp(rio->rio_buf_ptr, "GET", 3)==0) request->method = GET;
            else if(strncasecmp(rio->rio_buf_ptr, "POST", 4)==0) request->method = POST;
            else return PROCESS_ERR;
        }
        else if(strncasecmp(rio->rio_buf_ptr, "Connection:", 11)==0)
        {
            rio->rio_buf_ptr += 11;
            while(*rio->rio_buf_ptr==' ') rio->rio_buf_ptr++;
            if(strncasecmp(rio->rio_buf_ptr, "keep-alive", 10)==0) request->keep_alive = 1;
            else request->keep_alive = 0;
        }else if(strncasecmp(rio->rio_buf_ptr, "Content-Length", 15)==0)
        {
            rio->rio_buf_ptr += 15;
            while(*rio->rio_buf_ptr==' ') rio->rio_buf_ptr++;
            p = strpbrk(rio->rio_buf_ptr, "\r\n");
            if(p==NULL) return 0;
            *p = 0;
            request->content_length = atoi(rio->rio_buf_ptr);
            *p = '\r';
        }
        p = strpbrk(rio->rio_buf_ptr, "\r\n");
        if(p==NULL) return PROCESS_READ;
        rio->rio_buf_ptr = p + 2;
    }
}

PROCESS parse_requestcontent(rio_t *rio, request_t *request)
{
    if(request->method==GET)
        return PROCESS_END;
}

int parse_request(rio_t *rio, request_t *request)
{
    int ret;
    parse_requestline(rio, request);
    parse_requestheader(rio, request);
    parse_requestcontent(rio, request);
}

int builder(request_t *request, response_t *response, rio_t *rio)
{
    char header_line[MAXLINE];
    memset(header_line, 0, MAXLINE);

    if(response->status_code==200)
    {
        sprintf(header_line, "HTTP/1.1 %d %s\r\n", response->status_code, "OK");
    }else if(response->status_code==404)
    {
        sprintf(header_line, "HTTP/1.1 %d %s\r\n", response->status_code, "FILE NOT FOUND!");
    }else if(response->status_code==500)
    {
        sprintf(header_line, "HTTP/1.1 %d %s\r\n", response->status_code, "bad request");
    }
    strcat(rio->rio_buf, header_line);

    sprintf(header_line, "Content-Length: %d\r\n", response->content_length);
    strcat(rio->rio_buf, header_line);

    if(request->keep_alive)
    {
        sprintf(header_line, "Connection: Keep-Alive\r\n");
    }else{
        sprintf(header_line, "Connection: close\r\n");
    }
    strcat(rio->rio_buf, header_line);

    if(request->uri_dir)
    {
        sprintf(header_line, "Content-Type: text/html\r\n");
    }else{
        sprintf(header_line, "Content-Type: application/octet-stream\r\n");
    }
    strcat(rio->rio_buf, header_line);

    strcat(rio->rio_buf, "\r\n");
}

int write_all(request_t *request, response_t *response, rio_t *rio)
{
    rio->rio_cnt = strlen(rio->rio_buf);
    rio->rio_buf_ptr = rio->rio_buf;
    int writed_size;
    while(rio->rio_cnt>0)
    {
        if((writed_size=write(rio->rio_fd, rio->rio_buf_ptr, rio->rio_cnt))<0)
        {
            if(errno==EAGAIN)
            {
                continue;
            }
            return 0;
        }
        rio->rio_cnt -= writed_size;
        rio->rio_buf_ptr += writed_size;
    }

    int cnt=0;
    if(request->uri_dir)
    {
        while(response->content_length>0)
        {
            if((writed_size=write(rio->rio_fd, response->content+cnt, response->content_length))<0)
            {
                if(errno==EAGAIN)
                {
                    continue;
                }
                return 0;
            }
            cnt += writed_size;

            response->content_length -= writed_size;
        }
    }else{
        int path_fd = open(request->path, O_RDONLY, 0);

        char *src_addr = mmap(NULL, response->content_length, PROT_READ, MAP_PRIVATE, path_fd, 0);

        while(response->content_length>0)
        {
            if((writed_size=write(rio->rio_fd, src_addr+cnt, response->content_length))<0)
            {
                if(errno==EAGAIN)
                {
                    continue;
                }
                break;
            }
            cnt += writed_size;
            response->content_length -= writed_size;
        }

        munmap(src_addr, response->content_length);
        close(path_fd);
    }

    return 1;
}

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return new_option;
}

int resource_free(request_t *request, response_t *response)
{
    if(response->content!=NULL) free(response->content);
    if(request->uri!=NULL) free(request->uri);

}

int process_request(int sockcli)
{
    rio_t rio;

    request_t request;
    request.keep_alive = 0;
    request.uri = NULL;
    response_t response;
    response.content = NULL;

    rio_init(sockcli, &rio);

    PROCESS process_state = PROCESS_READ;

    int requestlineok = 0;
    while(process_state!=PROCESS_END)
    {   
        switch (process_state)
        {
        // 读
        case PROCESS_READ:
            process_state = rio_read(&rio);
            break;
        // 解析
        case PROCESS_PARSE_HEADERS:
        {
            if(!requestlineok) {
                parse_requestline(&rio, &request);
                requestlineok = 1;
            }
            else process_state = parse_requestheader(&rio, &request);
            break;
        }

        // 解析body
        case PROCESS_PARSE_BODY:
            process_state = parse_requestcontent(&rio, &request);
            break;
        case PROCESS_ERR:
        {
            response.status_code = 500;
            response.content_length = 0;
            process_state = PROCESS_END;
            goto res;
        }
            break;
        default:
            break;
        }
    }

    check_uri(&request, &response);

res:
    rio_init(sockcli, &rio);

    builder(&request, &response, &rio);

    write_all(&request, &response, &rio);

    resource_free(&request, &response);

    return request.keep_alive;
}


void init(int port, char *host)
{
    int socksev, sockcli;
    struct sockaddr_in addrcli;
    int addrlen;
    
    socksev = socket_init(port, host);

    // int epollfd = epoll_create(1024);

    struct pollfd pollfds[EPOLLMAX];

    int i;
    for(i=0;i<EPOLLMAX;++i)
        pollfds[i].fd = -1;
    

    pollfds[socksev].fd = socksev;
    pollfds[socksev].events = POLLIN;
    int maxfd = socksev;
    setnonblocking(socksev);
    // struct epoll_event ev;
    // ev.events = EPOLLIN|EPOLLET;
    // ev.data.fd = socksev;
    // epoll_ctl(epollfd, EPOLL_CTL_ADD, socksev, &ev);
    // struct epoll_event events[EPOLLMAX];
    while(1)
    {
        // int num = epoll_wait(epollfd, events, EPOLLMAX, -1);
        int num = poll(pollfds, maxfd+1, -1);

        if(num<0)
        {
            perror("poll");
            break;
        }

        // int i;
        int currentfdsize = maxfd;
        for(i=0;i<=currentfdsize;++i)
        {
            if(pollfds[i].fd<0) continue;

            // if((pollfds[i].revents&POLLIN)==0 && (pollfds[i].revents&POLLRDHUP)==0) continue;
            if(pollfds[i].revents==0) continue;

            if((pollfds[i].revents&POLLIN) && (pollfds[i].fd==socksev))
            {   
                int sockcli;
                while( (sockcli=accept(socksev, (struct sockaddr*)&addrcli, &addrlen))>0)
                {
                    pollfds[sockcli].events = POLLIN | POLLRDHUP;
                    pollfds[sockcli].fd = sockcli;
                    if(sockcli>maxfd) maxfd = sockcli;
                    setnonblocking(sockcli);
                    printf("socket %d connected\n", sockcli);
                }
            }else if(pollfds[i].revents & POLLRDHUP)
            {
                // epoll_ctl(epollfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                printf("socket %d disconnected\n", pollfds[i].fd);
                
                if(maxfd==pollfds[i].fd)
                {
                    int j=maxfd-1;
                    for(;j>=0;j--)
                    {
                        if(pollfds[j].fd>0)
                        {
                            maxfd = j;
                            break;
                        }
                    }
                }
                close(pollfds[i].fd);
                pollfds[i].fd = -1;
            }else if(pollfds[i].revents & POLLIN)
            {
                printf("socket %d event response\n", pollfds[i].fd);
                int keep_alive = process_request(pollfds[i].fd);
                printf("socket %d response OK\n", pollfds[i].fd);

                printf("keep alive: %d\n", keep_alive);
                if(keep_alive==0)
                {
                    printf("socket %d disconnected\n", pollfds[i].fd);
                    
                    if(maxfd==pollfds[i].fd)
                    {
                        int j=maxfd-1;
                        for(;j>=0;j--)
                        {
                            if(pollfds[j].fd>0)
                            {
                                maxfd = j;
                                break;
                            }
                        }
                    }
                    close(pollfds[i].fd);
                    pollfds[i].fd = -1;
                }

            }else {
                printf("unknown event\n");
            }
        }
    }

    close(socksev);
}

int socket_init(int port, char *host)
{
    int socksev;
    struct sockaddr_in addrsev;
    memset(&addrsev, '\0', sizeof(addrsev));

    addrsev.sin_family = AF_INET;
    addrsev.sin_port = htons(port);
    addrsev.sin_addr.s_addr = inet_addr(host);

    socksev = socket(AF_INET, SOCK_STREAM, 0);
    if(socksev==-1){
        printf("create socket fail\n");
        exit(1);
    }
    int reuse = 1;
    if(setsockopt(socksev, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int))<0)
    {
        perror("setsocketopt");
        exit(0);
    }
    if(bind(socksev, (struct sockaddr*)&addrsev, sizeof(addrsev))<0){
        printf("bind error!\n");
        exit(1);
    }

    if(listen(socksev, 128)<0){
        printf("listen fail\n");
        exit(0);
    }

    return socksev;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    init(9999, "127.0.0.1");
    
    return 0;
}

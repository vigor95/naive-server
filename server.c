#include <arpa/inet.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>

#define LISTENQ 1024
#define MAXLINE 1024
#define RIO_BUFSIZE 1024

typedef struct {
    int rio_fd;
    int rio_cnt;
    char *rio_bufptr;
    char rio_buf[RIO_BUFSIZE];
} rio_t;

typedef struct sockaddr SA;

typedef struct {
    char filename[512];
    off_t offset;
    size_t end;
} http_request;

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map;

mime_map mime_types[] = {
    {".css", "text/css"},
    {".html", "text/html"},
    {".js", "application/javascript"},
    {NULL, NULL}
};

const char *default_mime_type = "text/plain";

void rioReadinitb(rio_t *rp, int fd) {
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

ssize_t writen(int fd, void *usrbuf, size_t n) {
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = (char*)usrbuf;

    while (nleft > 0) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR)
                nwritten = 0;
            else
                return -1;
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}

static ssize_t rioRead(rio_t *rp, char *usrbuf, size_t n) {
    int cnt;
    while (rp->rio_cnt <= 0) {
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0) {
            if (errno != EINTR)
                return -1;
        }
        else if (rp->rio_cnt == 0)
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf;
    }

    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

ssize_t rioReadlineb(rio_t *rp, void *usrbuf, size_t maxlen) {
    int n, rc;
    char c;
    char *bufp = (char*)usrbuf;

    for (n = 1; n < maxlen; n++) {
        if ((rc = rioRead(rp, &c, 1)) == 1) {
            *bufp++ = c;
            if (c == '\n') break;
        }
        else if (rc == 0) {
            if (n == 1) return 0;
            break;
        }
        else return -1;
    }
    *bufp = 0;
    return n;
}

void formatSize(char *buf, struct stat *stat) {
    if (S_ISDIR(stat->st_mode))
        sprintf(buf, "%s", "[DIR]");
    else {
        off_t size = stat->st_size;
        if (size < 1024)
            sprintf(buf, "%lu", size);
        else if (size < 1024 * 1024)
            sprintf(buf, "%.1fK", (double)size / 1024);
        else if (size < 1024 * 1024 * 1024)
            sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
        else
            sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
    }
}

void handleDirectoryRequest(int out_fd, int dir_fd, char *filename) {
    char buf[MAXLINE], m_time[32], size[16];
    struct stat statbuf;
    sprintf(buf, "HTTP/1.1 200 OK\r\n%s%s%s%s%s",
            "Content-Type: text/html\r\n\r\n",
            "<html><head><style>",
            "body{font-family: monaco;font-size: 15px;}",
            "td {padding: 1.5px 6px;}",
            "</style></head><body><table>\n");
    writen(out_fd, buf, strlen(buf));
    DIR *d = fdopendir(dir_fd);
    struct dirent *dp;
    int ffd;
    while ((dp = readdir(d)) != NULL) {
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
            continue;
        if ((ffd = openat(dir_fd, dp->d_name, O_RDONLY)) == -1) {
            perror(dp->d_name);
            continue;
        }
        fstat(ffd, &statbuf);
        strftime(m_time, sizeof(m_time), "%Y-%m-%d %H:%M",
                localtime(&statbuf.st_mtime));
        formatSize(size, &statbuf);
        if (S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode)) {
            const char *d = S_ISDIR(statbuf.st_mode) ? "/" : "";
            sprintf(buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n", dp->d_name, d, dp->d_name, d, m_time, size);
            writen(out_fd, buf, strlen(buf));
        }
        close(ffd);
    }
    sprintf(buf, "</table></body></html>");
    writen(out_fd, buf, strlen(buf));
    closedir(d);
}

static const char* getMimeType(char *filename) {
    char *dot = strrchr(filename, '.');
    if (dot) {
        for (mime_map *map = mime_types; map->extension; map++)
            if (!strcmp(map->extension, dot))
                return map->mime_type;
    }
    return default_mime_type;
}

int openListenfd(int port) {
    int listenfd, optval = 1;
    struct sockaddr_in serveraddr;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                (const void*)&optval, sizeof(int)) < 0)
        return -1;

    if (setsockopt(listenfd, 6, TCP_CORK, (const void*)&optval,
                sizeof(int)) < 0)
        return -1;

    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);

    if (bind(listenfd, (SA*)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    
    return listenfd;
}

void urlDecode(char *src, char *dest, int max) {
    char *p = src;
    char code[3] = {0};
    while (*p && --max) {
        if (*p == '%') {
            memcpy(code, ++p, 2);
            *dest++ = (char)strtoul(code, NULL, 16);
            p += 2;
        }
        else
            *dest++ = *p++;
    }
    *dest = '\0';
}

void parseRequest(int fd, http_request *req) {
    rio_t rio;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE];
    req->offset = req->end = 0;

    rioReadinitb(&rio, fd);
    rioReadlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s", method, uri);

    while (buf[0] != '\n' && buf[1] != '\n') {
        rioReadlineb(&rio, buf, MAXLINE);
        if (buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'n') {
            sscanf(buf, "Range: bytes = %lu ~ %lu",
                    &req->offset, &req->end);
            if (req->end) req->end++;
        }
    }
    char *filename = uri;
    if (uri[0] == '/') {
        filename = uri + 1;
        int length = strlen(filename);
        if (!length) filename = ".";
        else for (int i = 0; i < length; i++)
            if (filename[i] == '?') {
                filename[i] = '\0';
                break;
            }
    }
    urlDecode(filename, req->filename, MAXLINE);
    printf("req->filename: %s\n", req->filename);
}

void logAccess(int status, struct sockaddr_in *c_addr,
        http_request *req) {
    printf("%s:%d %d - %s\n", inet_ntoa(c_addr->sin_addr),
            ntohs(c_addr->sin_port), status, req->filename);
}

void clientError(int fd, int status,
        const char *msg, const char *longmsg) {
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, msg);
    sprintf(buf + strlen(buf), "Content-length: %lu\r\n\r\n",
            strlen(longmsg));
    sprintf(buf + strlen(buf), "%s", longmsg);
    writen(fd, buf, strlen(buf));
}

void serveStatic(int out_fd, int in_fd,
        http_request *req, size_t tot_size) {
    char buf[256];
    if (req->offset > 0) {
        sprintf(buf, "HTTP/1.1 206 Partial\r\n");
        sprintf(buf + strlen(buf), "Content-Range: bytes %lu-%lu/%lu\r\n",
                req->offset, req->end, tot_size);
    }
    else
        sprintf(buf, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
    sprintf(buf + strlen(buf), "Cache-Control: no-cache\r\n");
    sprintf(buf + strlen(buf), "Content-length: %lu\r\n",
            req->end - req->offset);
    sprintf(buf + strlen(buf), "Content-type: %s\r\n\r\n",
            getMimeType(req->filename));
    
    writen(out_fd, buf, strlen(buf));
    off_t offset = req->offset;
    while (offset < req->end) {
        if (sendfile(out_fd, in_fd, &offset, req->end - req->offset) <= 0)
            break;
        printf("Offset: %d \n\n", offset);
        close(out_fd);
        break;
    }
}

void process(int fd, struct sockaddr_in *clientaddr) {
    printf("Accept request, fd is %d, pid is %d\n", fd, getpid());
    http_request req;
    parseRequest(fd, &req);

    struct stat sbuf;
    int status = 200, ffd = open(req.filename, O_RDONLY, 0);
    if (ffd <= 0) {
        status = 404;
        const char *msg = "File not found";
        clientError(fd, status, "Not found", msg);
    }
    else {
        fstat(ffd, &sbuf);
        if (S_ISREG(sbuf.st_mode)) {
            if (req.end == 0) req.end = sbuf.st_size;
            if (req.offset > 0) status = 206;
            serveStatic(fd, ffd, &req, sbuf.st_size);
        }
        else if (S_ISDIR(sbuf.st_mode)) {
            status = 200;
            handleDirectoryRequest(fd, ffd, req.filename);
        }
        else {
            status = 400;
            const char *msg = "Unknown Error";
            clientError(fd, status, "Error", msg);
        }
        close(ffd);
    }
    logAccess(status, clientaddr, &req);
}

int main(int argc, char **argv) {
    struct sockaddr_in clientaddr;
    int default_port = 1234, listenfd, connfd;
    char buf[256];
    char *path = getcwd(buf, 256);
    socklen_t clientlen = sizeof(clientaddr);
    
    if (argc == 2) {
        if (isdigit(argv[1][0]))
            default_port = atoi(argv[1]);
        else {
            path = argv[1];
            if (chdir(argv[1])) {
                perror(argv[1]);
                exit(1);
            }
        }
    }
    else if (argc == 3) {
        default_port = atoi(argv[2]);
        path = argv[1];
        if (chdir(path)) {
            perror(path);
            exit(1);
        }
    }

    listenfd = openListenfd(default_port);
    if (listenfd)
        printf("Listen on port %d, fd is %d\n", default_port, listenfd);
    else {
        perror("ERROR");
        exit(listenfd);
    }

    signal(SIGPIPE, SIG_IGN);

    /*for (int i = 0; i < 10; i++) {
        int pid = fork();
        if (!pid) {                 //子进程
            while (1) {
                connfd =
                    accept(listenfd, (SA*)&clientaddr, &clientlen);
                process(connfd, &clientaddr);
                close(connfd);
            }
        }
        else if (pid > 0)
            printf("Child pid is %d\n", pid);
        else
            perror("fork");
    }*/

    while (1) {
        connfd = accept(listenfd, (SA*)&clientaddr, &clientlen);
        process(connfd, &clientaddr);
        close(connfd);
    }

    return 0;
}

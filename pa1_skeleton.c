/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/*
Please specify the group members here
# Student #1:
# Student #2:
# Student #3:
*/

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    int epoll_fd;        /* File descriptor for the epoll instance */
    int socket_fd;       /* File descriptor for the client socket connected to the server */
    long long total_rtt; /* Accumulated RTT for all request/response pairs (microseconds) */
    long total_messages; /* Total number of request/response pairs completed */
    float request_rate;  /* Requests per second for this thread */
} client_thread_data_t;

/* ---------- Helpers: fixed-size send/recv for TCP ---------- */

static long long timeval_diff_us(const struct timeval *start, const struct timeval *end) {
    long long s = (long long)start->tv_sec * 1000000LL + (long long)start->tv_usec;
    long long e = (long long)end->tv_sec * 1000000LL + (long long)end->tv_usec;
    return e - s;
}

static int send_full(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return 0;
}

static int recv_full(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t recvd = 0;

    while (recvd < len) {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n > 0) {
            recvd += (size_t)n;
            continue;
        }
        if (n == 0) return 0; /* peer closed */
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return 1;
}

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];

    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* 16 bytes */
    char recv_buf[MESSAGE_SIZE];

    struct timeval start, end;

    /* Register this thread's connected socket with its epoll instance */
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) < 0) {
        perror("client epoll_ctl ADD");
        return NULL;
    }

    data->total_rtt = 0;
    data->total_messages = 0;
    data->request_rate = 0.0f;

    for (int i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);

        if (send_full(data->socket_fd, send_buf, MESSAGE_SIZE) < 0) {
            perror("client send_full");
            break;
        }

        int nfds;
        while (1) {
            nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
            if (nfds < 0 && errno == EINTR) continue;
            break;
        }
        if (nfds < 0) {
            perror("client epoll_wait");
            break;
        }

        int ok = 0;
        for (int e = 0; e < nfds; e++) {
            if ((events[e].data.fd == data->socket_fd) && (events[e].events & EPOLLIN)) {
                int r = recv_full(data->socket_fd, recv_buf, MESSAGE_SIZE);
                if (r <= 0) {
                    if (r < 0) perror("client recv_full");
                    ok = -1;
                } else {
                    ok = 1;
                }
                break;
            }
        }
        if (ok <= 0) break;

        gettimeofday(&end, NULL);

        data->total_rtt += timeval_diff_us(&start, &end);
        data->total_messages += 1;
    }

    if (data->total_rtt > 0 && data->total_messages > 0) {
        double total_time_sec = (double)data->total_rtt / 1000000.0;
        data->request_rate = (float)((double)data->total_messages / total_time_sec);
    } else {
        data->request_rate = 0.0f;
    }

    return NULL;
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server_ip: %s\n", server_ip);
        exit(1);
    }

    /* Create sockets + epoll fds, connect each socket */
    for (int i = 0; i < num_client_threads; i++) {
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("client socket");
            exit(1);
        }

        if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("client connect");
            close(sockfd);
            exit(1);
        }

        int epfd = epoll_create1(0);
        if (epfd < 0) {
            perror("client epoll_create1");
            close(sockfd);
            exit(1);
        }

        thread_data[i].socket_fd = sockfd;
        thread_data[i].epoll_fd = epfd;
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0.0f;
    }

    for (int i = 0; i < num_client_threads; i++) {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    long long total_rtt = 0;
    long total_messages = 0;
    double total_request_rate = 0.0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);

        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;

        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }

    if (total_messages > 0) {
        printf("Average RTT: %lld us\n", total_rtt / total_messages);
    } else {
        printf("Average RTT: N/A (no messages)\n");
    }
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
}

void run_server() {
    int listen_fd = -1;
    int epoll_fd = -1;
    struct sockaddr_in addr;

    /* Create listening socket */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("server socket");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("server setsockopt SO_REUSEADDR");
        close(listen_fd);
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server_ip: %s\n", server_ip);
        close(listen_fd);
        exit(1);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("server bind");
        close(listen_fd);
        exit(1);
    }

    if (listen(listen_fd, 128) < 0) {
        perror("server listen");
        close(listen_fd);
        exit(1);
    }

    /* Create epoll instance and register listening socket */
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("server epoll_create1");
        close(listen_fd);
        exit(1);
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("server epoll_ctl ADD listen_fd");
        close(epoll_fd);
        close(listen_fd);
        exit(1);
    }

    printf("Server listening on %s:%d\n", server_ip, server_port);

    /* Event loop */
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        int nfds;
        while (1) {
            nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
            if (nfds < 0 && errno == EINTR) continue;
            break;
        }
        if (nfds < 0) {
            perror("server epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /* Accept exactly ONE connection per readiness event (prevents blocking) */
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);

                int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
                if (cfd < 0) {
                    perror("server accept");
                    continue;
                }

                struct epoll_event cev;
                memset(&cev, 0, sizeof(cev));
                cev.events = EPOLLIN;
                cev.data.fd = cfd;

                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                    perror("server epoll_ctl ADD client");
                    close(cfd);
                }
            } else if (events[i].events & EPOLLIN) {
                /* Read MESSAGE_SIZE bytes and echo back */
                char buf[MESSAGE_SIZE];
                int r = recv_full(fd, buf, MESSAGE_SIZE);

                if (r == 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else if (r < 0) {
                    perror("server recv_full");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else {
                    if (send_full(fd, buf, MESSAGE_SIZE) < 0) {
                        perror("server send_full");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                    }
                }
            } else {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
            }
        }
    }

    if (epoll_fd >= 0) close(epoll_fd);
    if (listen_fd >= 0) close(listen_fd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#define PORT 6379
#define MAX_EVENTS 1000
#define REDIS_PONG "+PONG\r\n"

int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
	{
		perror("fcntl(F_GETFL)");
		return -1;
	}

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		perror("fcntl(F_SETFL)");
		return -1;
	}
	return 0;
}

int main()
{
	// Disable output buffering
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");

	int server_fd;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
	{
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
	{
		printf("Setsockopt failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(PORT),
		.sin_addr = {htonl(INADDR_ANY)},
	};

	if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0)
	{
		printf("Bind failed: %s \n", strerror(errno));
		return 1;
	}

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0)
	{
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}
	if (set_nonblocking(server_fd) == -1)
	{
		printf("set_nonblocking failed: %s\n", strerror(errno));
		return 1;
	}
	printf("Waiting for a client to connect...\n");

	int epfd = epoll_create1(0);
	if (epfd < 0)
	{
		perror("epoll_create1");
		return 1;
	}
	struct epoll_event ev, events[MAX_EVENTS];
	ev.events = EPOLLIN;
	ev.data.fd = server_fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0)
	{
		perror("epoll_ctl: listen_fd");
		exit(EXIT_FAILURE);
	}

	printf("Event loop started\n");

	while (1)
	{
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n == -1)
		{
			if (errno == EINTR)
			{
				continue;
			}
			perror("epoll_wait");
			break;
		}

		for (int i = 0; i < n; i++)
		{
			int fd = events[i].data.fd;
			if (fd == server_fd)
			{
				struct sockaddr_in client_addr;
				socklen_t client_len = sizeof(client_addr);
				int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
				if (client_fd < 0)
				{
					perror("accept");
					continue;
				}
				printf("New client connected\n");
				if (set_nonblocking(client_fd) < 0)
				{
					close(client_fd);
					continue;
				}
				ev.events = EPOLLIN;
				ev.data.fd = client_fd;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
				{
					perror("epoll_ctl: client_fd");
					close(client_fd);
					continue;
				}
			}
			else if (events[i].events & EPOLLIN)
			{
				char buf[1024];
				int bytes_read = recv(fd, buf, sizeof(buf) - 1, 0);

				if (bytes_read == 0)
				{
					// Client disconnected
					printf("Client (fd=%d) disconnected.\n", fd);
					close(fd);
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
				}
				else if (bytes_read < 0)
				{
					if (errno != EAGAIN && errno != EWOULDBLOCK)
					{
						perror("recv");
						close(fd);
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					}
				}
				else
				{
					buf[bytes_read] = '\0';
					printf("Received from fd=%d: %s", fd, buf);

					send(fd, REDIS_PONG, strlen(REDIS_PONG), 0);
				}
			}
		}
	}
	close(epfd);
	close(server_fd);

	return 0;
}

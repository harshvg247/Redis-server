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
#include <ctype.h>
#include "uthash.h"
#include "utils.h"

#define PORT 6379			   // Redis default port
#define MAX_EVENTS 1000		   // Maximum simultaneous events epoll can handle
#define REDIS_PONG "+PONG\r\n" // Redis protocol response for "PING"
#define REDIS_OK "+OK\r\n"
#define NULL_BULK_STRING "$-1\r\n"
#define dbg 1

/**
 * Sets a socket file descriptor to non-blocking mode.
 * Non-blocking sockets are essential for epoll to work efficiently,
 * preventing the server from stalling on slow clients.
 */
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

int extract_number(int *ind, const char *str)
{
	int num = 0;
	while (str[*ind] - '0' >= 0 && str[*ind] - '0' <= 9)
	{
		num = num * 10 + (str[(*ind)++] - '0');
	}
	return num;
}
// ind at '$'
char *extract_bulk_string(int *ind, const char *str)
{
	if (dbg)
		printf("Extracting bulk string\n");
	(*ind)++;
	int bulk_str_size = extract_number(ind, str);
	if (dbg)
		printf("bulk_str_size: %d\n", bulk_str_size);
	(*ind) += 2;
	char *bulk_str = (char *)malloc(bulk_str_size + 1);
	for (int i = 0; i < bulk_str_size; i++)
	{
		bulk_str[i] = str[(*ind)++];
	}
	bulk_str[bulk_str_size] = '\0';
	if (dbg)
		printf("Extracted string: %s\n", bulk_str);
	return bulk_str;
}
char *encode_bulk_str(const char *str)
{
	int num_digits_in_size_str = count_digits(strlen(str));
	char *encoded_str = (char *)malloc(num_digits_in_size_str + strlen(str) + 6);
	if (encoded_str == NULL)
	{
		return NULL; // Handle allocation failure
	}
	sprintf(encoded_str, "$%d\r\n%s\r\n", (int)strlen(str), str);
	return encoded_str;
}

int handle_echo(const char *str, int fd)
{
	if (dbg)
		printf("Handling echo command\n");
	char *encoded_str = encode_bulk_str(str);
	send(fd, encoded_str, strlen(encoded_str), 0);
	free(encoded_str);
	return 1;
}

typedef struct db_entry
{
	char *key;
	char *value;
	UT_hash_handle hh;
} db_entry;
db_entry *db = NULL;

void handle_set(char *key, char *value, int fd)
{
	db_entry *e;
	HASH_FIND_STR(db, key, e);
	if (e == NULL)
	{
		e = (db_entry *)malloc(sizeof(db_entry));
		e->key = strdup(key);
	}
	e->value = strdup(value);
	HASH_ADD_STR(db, key, e);
	send(fd, REDIS_OK, strlen(REDIS_OK), 0);
}

void handle_get(char *key, int fd)
{
	db_entry* e;
	HASH_FIND_STR(db, key, e);
	if(e == NULL){
		send(fd, NULL_BULK_STRING, strlen(NULL_BULK_STRING), 0);
	}else{
		char* response = encode_bulk_str(e->value);
		send(fd, response, strlen(response), 0); 
		free(response);
	}
}

int main()
{
	// Disable stdout and stderr buffering for immediate log visibility
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("Logs from your program will appear here!\n");

	int server_fd;

	/** STEP 1: Create a TCP socket **/
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
	{
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}

	// 	Normally, when a TCP socket is closed, the port goes into a TIME_WAIT state for about 1–2 minutes.
	// During this time, the OS keeps the port reserved to ensure:

	// Any delayed packets from the old connection don’t interfere with new ones.

	// Proper TCP shutdown is respected.
	/** STEP 2: Enable address reuse to prevent “Address already in use” errors **/
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
	{
		printf("Setsockopt failed: %s \n", strerror(errno));
		return 1;
	}

	/** STEP 3: Bind socket to a port **/
	struct sockaddr_in serv_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(PORT),
		.sin_addr = {htonl(INADDR_ANY)}, // Bind this socket to all my machine’s IPv4 addresses.
	};

	if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0)
	{
		printf("Bind failed: %s \n", strerror(errno));
		return 1;
	}
	// This specifies how many pending client connections the kernel should queue while your server hasn’t yet accepted them
	/** STEP 4: Start listening for incoming connections **/
	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0)
	{
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}

	/** STEP 5: Make listening socket non-blocking **/
	if (set_nonblocking(server_fd) == -1)
	{
		printf("set_nonblocking failed: %s\n", strerror(errno));
		return 1;
	}

	printf("Waiting for a client to connect...\n");

	/** STEP 6: Create an epoll instance **/
	int epfd = epoll_create1(0);
	if (epfd < 0)
	{
		perror("epoll_create1");
		return 1;
	}

	/** STEP 7: Register the listening socket with epoll **/
	struct epoll_event ev, events[MAX_EVENTS];
	ev.events = EPOLLIN; // Interested in "readable" events
	ev.data.fd = server_fd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0)
	{
		perror("epoll_ctl: listen_fd");
		exit(EXIT_FAILURE);
	}

	// printf("Initialising database\n");

	printf("Event loop started\n");

	/** STEP 8: Main event loop **/
	while (1)
	{
		// Wait indefinitely (-1) for events on registered file descriptors
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n == -1)
		{
			if (errno == EINTR)
				continue; // Interrupted by signal, just continue
			perror("epoll_wait");
			break;
		}

		/** STEP 9: Handle all triggered events **/
		for (int i = 0; i < n; i++)
		{
			int fd = events[i].data.fd;

			// Event on the listening socket — means new client connection
			if (fd == server_fd)
			{
				struct sockaddr_in client_addr;
				socklen_t client_len = sizeof(client_addr);
				// Accept a pending client connection.
				// Creates a new socket (client_fd) dedicated to this client,
				// while server_fd continues listening for others.
				int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
				if (client_fd < 0)
				{
					perror("accept");
					continue;
				}
				printf("New client connected\n");

				// Set new client socket to non-blocking
				if (set_nonblocking(client_fd) < 0)
				{
					close(client_fd);
					continue;
				}

				// Register client_fd to epoll to monitor for incoming data
				ev.events = EPOLLIN;
				ev.data.fd = client_fd;
				if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
				{
					perror("epoll_ctl: client_fd");
					close(client_fd);
					continue;
				}
			}

			// Event from a connected client
			else if (events[i].events & EPOLLIN)
			{
				char buf[1024];
				int bytes_read = recv(fd, buf, sizeof(buf) - 1, 0);
				buf[bytes_read] = '\0';
				if (bytes_read == 0)
				{
					/** Client disconnected gracefully **/
					printf("Client (fd=%d) disconnected.\n", fd);
					close(fd);
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
				}
				else if (bytes_read < 0)
				{
					/** Read error (ignore EAGAIN/EWOULDBLOCK since it’s non-blocking) **/
					if (errno != EAGAIN && errno != EWOULDBLOCK)
					{
						perror("recv");
						close(fd);
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
					}
				}
				else
				{
					/** Data received successfully **/
					int ind = 0;
					if (dbg)
						printf("buf: %s\n", buf);
					if (buf[ind] == '*')
					{
						ind++;
						int cmnd_list_size = extract_number(&ind, buf);
						if (dbg)
							printf("cmnd_list_size: %d\n", cmnd_list_size);
						char *cmnds[cmnd_list_size];
						ind += 2;
						for (int i = 0; i < cmnd_list_size; i++)
						{
							cmnds[i] = extract_bulk_string(&ind, buf);
							ind += 2;
						}
						to_lowercase(cmnds[0]);
						for (int i = 0; i < cmnd_list_size; i++)
						{
							printf("%s ", cmnds[i]);
						}
						printf("\n");
						if (!strcmp(cmnds[0], "echo"))
						{
							handle_echo(cmnds[1], fd);
						}
						else if (!strcmp(cmnds[0], "ping"))
						{
							send(fd, REDIS_PONG, strlen(REDIS_PONG), 0);
						}
						else if (!strcmp(cmnds[0], "set"))
						{
							handle_set(cmnds[1], cmnds[2], fd);
						}
						else if (!strcmp(cmnds[0], "get")){
							handle_get(cmnds[1], fd);
						}
						for (int i = 0; i < cmnd_list_size; i++)
						{
							free(cmnds[i]);
						}
					}
				}
			}
		}
	}

	/** Cleanup **/
	close(epfd);
	close(server_fd);

	return 0;
}

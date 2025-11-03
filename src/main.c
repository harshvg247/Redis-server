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

#include "utils.h"      // Assumed to exist
#include "time_utils.h" // Assumed to exist

// Include all parser and handler logic
#include "parser.h"
#include "handler.h" // This now includes minheap.h and data structs

#define PORT 6379
#define MAX_EVENTS 1000
#define REDIS_PONG "+PONG\r\n"
#define dbg 1

/**
 * Sets a socket file descriptor to non-blocking mode.
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

int main()
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("Logs from your program will appear here!\n");

    // 1. Initialize DB and Heap
    db_entry *db = NULL;
    heap_t *expiry_heap = heap_create(compare_expiry_entry); // Use new func
    if (expiry_heap == NULL)
    {
        printf("Failed to create expiry heap.\n");
        return 1;
    }

    int server_fd;

    /** STEP 1: Create a TCP socket **/
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        printf("Socket creation failed: %s...\n", strerror(errno));
        return 1;
    }

    /** STEP 2: Enable address reuse **/
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
        .sin_addr = {htonl(INADDR_ANY)},
    };
    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0)
    {
        printf("Bind failed: %s \n", strerror(errno));
        return 1;
    }

    /** STEP 4: Start listening **/
    if (listen(server_fd, 5) != 0)
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

    // https://man7.org/linux/man-pages/man7/epoll.7.html
    /** STEP 6: Create an epoll instance **/
    int epfd = epoll_create1(0);
    if (epfd < 0)
    {
        perror("epoll_create1");
        return 1;
    }

    /** STEP 7: Register the listening socket with epoll **/
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0)
    {
        perror("epoll_ctl: listen_fd");
        exit(EXIT_FAILURE);
    }

    printf("Event loop started\n");

    /** STEP 8: Main event loop **/
    while (1)
    {
        // Set epoll_wait timeout to 100ms so the eviction loop runs
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (n == -1)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        /** STEP 9: Handle all triggered events **/
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == server_fd)
            {
                // Accept new connection
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
                // Data from a client
                char buf[1024];
                int bytes_read = recv(fd, buf, sizeof(buf) - 1, 0);
                buf[bytes_read] = '\0';

                if (bytes_read == 0) /***************** */
                {
                    // Client disconnected
                    printf("Client (fd=%d) disconnected.\n", fd);
                    close(fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                }
                else if (bytes_read < 0)
                {
                    // Read error
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        perror("recv");
                        close(fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    }
                }
                else
                {
                    // Data received
                    int ind = 0;
                    if (dbg)
                        printf("buf: %s\n", buf);
                    if (buf[ind] == '*')
                    {
                        //
                        // --- Parsing logic (from parser.h) ---
                        //
                        ind++;
                        int cmnd_list_size = extract_number(&ind, buf);
                        char *cmnds[cmnd_list_size];
                        ind += 2;
                        for (int j = 0; j < cmnd_list_size; j++)
                        {
                            cmnds[j] = extract_bulk_string(&ind, buf);
                            if (cmnds[j] == NULL) { // Malloc failure
                                cmnd_list_size = j; // only free the ones we allocated
                                break;
                            }
                            ind += 2;
                        }
                        to_lowercase(cmnds[0]);
                        if (dbg)
                        {
                            for (int j = 0; j < cmnd_list_size; j++)
                                printf("%s ", cmnds[j]);
                            printf("\n");
                        }

                        //
                        // --- Command dispatch (calls functions from handler.h) ---
                        //
                        if (!strcmp(cmnds[0], "echo"))
                        {
                            if (cmnd_list_size > 1) handle_echo(cmnds[1], fd);
                        }
                        else if (!strcmp(cmnds[0], "ping"))
                        {
                            send(fd, REDIS_PONG, strlen(REDIS_PONG), 0);
                        }
                        else if (!strcmp(cmnds[0], "set"))
                        {
                            if (cmnd_list_size < 3) continue; // Not enough args
                            long long expiry = -1;
                            if (cmnd_list_size > 3)
                            {
                                to_lowercase(cmnds[3]);
                                if (!strcmp(cmnds[3], "px") && cmnd_list_size > 4)
                                {
                                    expiry = current_time_ms() + atoi(cmnds[4]);
                                }
                            }
                            handle_set(&db, expiry_heap, cmnds[1], cmnds[2], expiry, fd);
                        }
                        else if (!strcmp(cmnds[0], "get"))
                        {
                            if (cmnd_list_size > 1) handle_get(&db, cmnds[1], fd);
                        }
                        else if (!strcmp(cmnds[0], "rpush"))
                        {
                            handle_rpush(&db, cmnds, cmnd_list_size, fd);
                        }
                        else if (!strcmp(cmnds[0], "lrange"))
                        {
                            handle_lrange(&db, cmnds, cmnd_list_size, fd);
                        }
                        else if (!strcmp(cmnds[0], "zadd"))
                        {
                            handle_zadd(&db, cmnds, cmnd_list_size, fd);
                        }
                        else if (!strcmp(cmnds[0], "zrange"))
                        {
                            handle_zrange(&db, cmnds, cmnd_list_size, fd);
                        }   
                        

                        // Free parsed commands
                        for (int j = 0; j < cmnd_list_size; j++)
                        {
                            free(cmnds[j]);
                        }
                    }
                }
            }
        } // End of epoll event loop

        // --- 4. Active Eviction Logic (NEW AND CORRECT) ---
        while (1)
        {
            // 1. Peek at the HEAP'S entry (this is now safe)
            expiry_entry_t *e_heap = (expiry_entry_t *)heap_peek(expiry_heap);

            // 2. Stop if heap is empty or top item is not expired
            if (e_heap == NULL || e_heap->expiry_ms > current_time_ms()) {
                break;
            }

            // 3. Item is expired. Pop it.
            e_heap = (expiry_entry_t *)heap_pop(expiry_heap);

            // 4. --- The Stale Pointer Check ---
            //    Find the *real* entry in the hash table
            db_entry *e_hash;
            HASH_FIND_STR(db, e_heap->key, e_hash);

            // 5. Check if it's stale
            //    Case A: Key was deleted (e_hash is NULL)
            //    Case B: Key was overwritten with a *different* expiry
            //            (e_hash->expiry_ms != e_heap->expiry_ms)
            if (e_hash == NULL || e_hash->expiry_ms != e_heap->expiry_ms)
            {
                if (dbg) printf("Stale heap entry found for key: %s\n", e_heap->key);
                // This heap entry is garbage. Free it and check the next one.
                free(e_heap->key);
                free(e_heap);
                continue;
            }

            // 6. If we're here, it's a valid, expired entry.
            //    e_hash points to the real db_entry.
            //    e_heap points to the heap's bookkeeping entry.
            if (dbg) printf("Active evict: %s\n", e_hash->key);
            
            // Delete from hash table
            free_db_value(e_hash);
            HASH_DEL(db, e_hash);
            free(e_hash->key);
            free(e_hash);

            // Delete the heap entry
            free(e_heap->key);
            free(e_heap);
        }

    } // End of main while(1)

    /** Cleanup **/
    // On a real shutdown, you would iterate and free all entries
    // in both 'db' and 'expiry_heap' before destroying them.
    heap_destroy(expiry_heap);
    close(epfd);
    close(server_fd);

    return 0;
}
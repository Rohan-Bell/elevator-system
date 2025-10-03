#ifndef SHARED_H
#define SHARED_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include "shared_mem.h"


#define CONTROLLER_PORT 3000
#define CONTROLLER_IP "127.0.0.1"

#define MAX_FLOOR 999
#define MIN_FLOOR 99 //Keep in mind it is B99 not 99
#define MILLISECOND 1000
#define DELAY 0


// Network utility functions
void recv_looped(int fd, void *buf, size_t sz);
void send_looped(int fd, const void *buf, size_t sz);
char *receive_msg(int fd);
void send_message(int fd, const char *buf);

// Floor utility functions
int validate_floor(const char* floor);
int floor_to_int(const char *floor_str);
void int_to_floor(int floor_int, char *floor_str, size_t size);

void msg(const char *s);




#endif
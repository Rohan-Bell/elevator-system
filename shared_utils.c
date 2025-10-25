#include "shared.h"
#include <stddef.h>

void recv_looped(int fd, void *buf, size_t sz) {
    char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t received = read(fd, ptr, remain);
        //error determined by -1
        if (received == -1) {
            perror("read()");
            exit(1);
        }
        if (received == 0) { //The connection is closed by the other end
            //fprintf(stderr, "Connection closed unexpectedly\n");
            exit(1);
        }
        //We can move the pointer forwad and decrease remainng bytes needed 
        ptr += received; 
        remain -= received;
    }
}

void send_looped(int fd, const void *buf, size_t sz) {
    const char *ptr = buf;
    size_t remain = sz;
    while (remain > 0) {
        ssize_t sent = write(fd, ptr, remain);
        if (sent == -1) {
            //Error when writing 
            perror("write()");
            exit(1);
        }
        ptr += sent;
        remain -= sent;
    }
}

char *receive_msg(int fd) {
    uint16_t nlen;
    recv_looped(fd, &nlen, sizeof(nlen));
    uint16_t len = ntohs(nlen); // To work out how much mem we need
    char *buf = malloc(len + 1); //Allocate the necessary memorey ( 1 is NT)
    if (buf == NULL) {
        perror("malloc()"); //We know it is malloc error because should be at least malloc(1)
        exit(1);
    }
    buf[len] = '\0'; //End the string (null terminate)
    recv_looped(fd, buf,len);
    return buf;
}

void send_message(int fd, const char *buf) {
    //Get length of string and convert big-endian (host to network)
    uint16_t len = htons(strlen(buf));
    //Lets send a 2-byte length prefix
    send_looped(fd, &len, sizeof(len));
    //sending the message
    send_looped(fd, buf, strlen(buf));
    
}


/*
Considerations: 
B1,2,3,4,5 (increase lower)
Floor can go up to 999
but as low as B99
no ground floor ... B2, B1, 1, 2 ...
*/
int validate_floor(const char* floor) {
    if (!floor || strlen(floor) == 0 || strlen(floor) > 3) {
        return 0; 
    }

    //Handle basement floors
    if (floor[0] == 'B') {
        //Check if remaining cahracters are digits
        for (int i = 1; floor[i] != '\0'; i++) {
            if(floor[i] < '0' || floor[i] > '9'){
                return 0; //Maybe we want to return something else
            }
        }

        //Convert to num
        int basement_num = atoi(floor + 1);
        return basement_num >= 1 && basement_num <= MIN_FLOOR; //Basedment is 1 to 99
    } else {
        //regular floor
        for(int i = 0; floor[i] != '\0'; i++) {
            if(floor[i] < '0' || floor[i] > '9'){
                //It is a character
                return 0;
            }
        }
        int floor_num = atoi(floor);
        return floor_num >= 1 && floor_num <= MAX_FLOOR;
    }
}

int floor_to_int(const char *floor_str) {
    if(floor_str == NULL) return 0;
    if (floor_str[0] == 'B') {
        return -atoi(floor_str + 1);
    }
    return atoi(floor_str);
}

void int_to_floor(int floor_int, char *floor_str, size_t size) {
    if (floor_int < 0) {
        snprintf(floor_str, size, "B%d", -floor_int);
    } else {
        snprintf(floor_str, size, "%d", floor_int);
    }
}

// Message and shared mem
void msg(const char *string)
{
  printf("%s\n    ", string);
  fflush(stdout);
}

void reset_shm(car_shared_mem *s)
{
  pthread_mutex_lock(&s->mutex);
  size_t offset = offsetof(car_shared_mem, current_floor);
  memset((char *)s + offset, 0, sizeof(*s) - offset);

  strcpy(s->status, "Closed");
  strcpy(s->current_floor, "1");
  strcpy(s->destination_floor, "1");
  pthread_mutex_unlock(&s->mutex);
}

void init_shm(car_shared_mem *s)
{
  pthread_mutexattr_t mutattr;
  pthread_mutexattr_init(&mutattr);
  pthread_mutexattr_setpshared(&mutattr, PTHREAD_PROCESS_SHARED);
  pthread_mutex_init(&s->mutex, &mutattr);
  pthread_mutexattr_destroy(&mutattr);

  pthread_condattr_t condattr;
  pthread_condattr_init(&condattr);
  pthread_condattr_setpshared(&condattr, PTHREAD_PROCESS_SHARED);
  pthread_cond_init(&s->cond, &condattr);
  pthread_condattr_destroy(&condattr);

  reset_shm(s);
}
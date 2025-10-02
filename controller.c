/**
 * The controller is not the designated safety-critical compoennt but 
 * it is still smart to use the safety critical mindset. 
 * 
 * Throughout the following code the following considerations will be met
 * 
 *  1. No dynamic memory allocation
 *          There will be no use of malloc() and free() and all data structs
 *          should be a fixed size static array ensuring no memory leaks
 *          heap fragmentation or non-deterministic allocation times. 
 *          The max number of cars and queu depth are compile-time constnats
 * 
 *  2. Strict Error checking and resource management 
 *          The return value of every system call that can fail is checked
 *          this includes stuff like socket or accept or bind or even read  /write.
 *          If failed, errors are logged and program goes to a safe state
 *          and ensures that FDs are closed
 * 
 * 3.  Concurrecnt and Race Conditions
 *          All shared data (the cars array) is protected by a signle 
 *          'pthread_mutex_t' where the mutex is locked before any 
 *          read or write acions and then immediately unlocked after
 *          The lock is held to ensure that the car states are prevented from
 *          chaning while decisions are being made
 * 
 * 4. Async
 *      to handle termination via CTRL+C (SIGINT), a safe handle is used.
 *      the handler sets a voltaile 'sig_atomic_t' flag true. The main loop
 *      will check for this loop and have a graceful shutdown by doing the 
 *      following: closing the socket, and allowing to prgoram to terminate
 *      cleanly. SIGPIPE is ignored to prevent the program crashing
 * 
 * 5. Justified Decision of using snprintf 
 *      While the MISRA C standards discourage the <stdio.h> the functions are 
 *      being used to create log messages. These should be replaced with a 
 *      write function in a true safety criitcal system. But for the goal
 *      of the logs here, this would make the code very messy. 
 *      snprintf is a safe choice for composing messages in a bounded buffer
 *      this I deemed as an acceptable change and justification.
 */

#include "shared.h"

#define MAX_CARS 10
#define MAX_CLIENTS (MAX_CARS + 20) // Cars + some call pads
#define MAX_QUEUE_DEPTH 20
#define BUFFER_SIZE 256
#define MAX_FLOOR_STR_LEN 8 // "B99" + null
#define MAX_CAR_NAME_LEN 128 //half of max buffer size to ensure no memory overflow


typedef enum {
    DIR_UP,
    DIR_DOWN,
    DIR_IDLE
} Direction;


//Represent the state of a single elevator car

typedef struct {
    int in_use;
    int socket_fd;
    char car_name[MAX_CAR_NAME_LEN];
    int floor_min;
    int floor_max;

    //A real-time status
    int current_floor;
    char status[BUFFER_SIZE];

    //scheduling queue
    int queue[MAX_QUEUE_DEPTH];
    int queue_size;
} Car;

//Global status for all cars
static Car cars[MAX_CARS];
static pthread_mutex_t cars_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int in_use;
    int client_fd;
} thread_arg_t;
static thread_arg_t thread_args[MAX_CLIENTS];
static pthread_mutex_t thread_args_mutex = PTHREAD_MUTEX_INITIALIZER;

//status flag for graceful shutdown
static volatile sig_atomic_t shutdown_requested = 0;

//Funciton prototypes
void *client_handler_thread(void *arg);
void handle_car_connection(int client_fd, const char* initial_message);
void handle_call_connection(int client_fd, const char* initial_message);
void sigint_handler(int signum);
void setup_signal_handlers(void);

//Scheduling Algorithm
void schedule_request(int source_floor, int dest_floor, int client_fd);
int calculate_insertion_cost(const Car *car, int source, int dest, int *pickup_idx, int *final_len);

//Queue Management
void insert_into_queue(int *queue, int *size, int index, int value);
void remove_from_queue(int *queue, int *size, int index);
void send_next_destination(Car *car);

//Utility
int parse_car_info(const char *buffer, char *name, int *min_floor, int *max_floor);
int parse_call_info(const char *buffer, int *source, int *dest);
int parse_status_info(const char *buffer, int *floor, char *status_buf);
void safe_write(int fd, const char *message);

//The main function 
int main() {
    int listen_fd = -1;
    int client_fd = -1;
    struct sockaddr_in serv_addr;
    pthread_t threads[MAX_CLIENTS];
    int opt_enable = 1;

    setup_signal_handlers();

    //Create a listening socket 
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("Socket() failed!");
        return EXIT_FAILURE;
    }

    //Set address reuse option
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt_enable, sizeof(opt_enable)) < 0) {
        perror("setsocketopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    //Bind a sockeet to a port 
    memset(&serv_addr, 0 , sizeof(serv_addr));
    serv_addr.sin_family = AF_INET; // Keep in mind ipv4 not ipv6
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(CONTROLLER_PORT);

    //Bind the socket now
    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0 ) { //would return -1 if bad
        perror("bind() failed");
        close(listen_fd); //close the file descriptor
        return EXIT_FAILURE;
    }

    //Listen for the connections
    if (listen(listen_fd, 10) < 0) { // 10 requests
        perror("listen() failed.");
        return EXIT_FAILURE;
    }

    printf("Controller listening on port %d\n", CONTROLLER_PORT);

    //the actual main accept loop, where we check if CTRL+C
    while (!shutdown_requested){
        client_fd = accept(listen_fd, NULL, NULL);
        if(client_fd < 0) {
            if(errno == EINTR) continue; //Was interruped by signal handler
            perror("accept() failed");
            break; //Exit the loop on other errors
        }
        int arg_idx = -1;
        pthread_mutex_lock(&thread_args_mutex);
        for(int i = 0; i < MAX_CLIENTS; i++) {
            if(!thread_args[i].in_use) {
                thread_args[i].in_use = 1;
                thread_args[i].client_fd = client_fd;
                arg_idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&thread_args_mutex);

        if (arg_idx!= -1) {
            //Pass thje indexinto static pool as arg
            if (pthread_create(&threads[arg_idx], NULL, client_handler_thread, 
            (void *)(intptr_t)arg_idx) != 0) {
                perror("pthread_create() failed");
                //If thread creation fails mark the arg struct as free
                pthread_mutex_lock(&thread_args_mutex);
                thread_args[arg_idx].in_use = 0;
                pthread_mutex_unlock(&thread_args_mutex);
                close(client_fd);
            } else {
                pthread_detach(threads[arg_idx]);
            }
        } else {
            printf("Max clients readchyed. rejecting new connection.\n");
            close(client_fd);
        }
    }
    //Requested to be shutdown from terminal being CTRL+C
    printf("\nShutdown signal received. Closing the listening socket.\n");
    if(listen_fd >= 0) {
        close(listen_fd);
    }    
    return EXIT_SUCCESS;
}

/**
 * @brief handles a signle client connection it it's own thread
 */
void *client_handler_thread(void *arg) {
    int arg_idx = (intptr_t)arg;
    //get the client file descriptor from the static pool
    int client_fd = thread_args[arg_idx].client_fd;
    char *buffer = receive_message(client_fd);

    if (buffer == NULL) {
        //Client has disconnected befdore sending anything
        close(client_fd);
    } else if (strncmp(buffer, "CAR", 3) == 0) {
        handle_car_connection(client_fd, buffer);
    } else if (strncmp(buffer, "CALL", 4) == 0) {
        handle_call_connection(client_fd, buffer);
        close(client_fd);
    }
    //Free the initial buffer once handler done
    if(buffer != NULL) {
        free(buffer);
    }
    pthread_mutex_lock(&thread_args_mutex);
    thread_args[arg_idx].in_use = 0;
    pthread_mutex_unlock(&thread_args_mutex);
    return NULL;
}

void handle_car_connection(int client_fd, const char* initial_message) {
    char car_name[BUFFER_SIZE];
    int min_floor, max_floor;

    //The initial "CAR.." ;line is consumed, get the next line

    if(parse_car_info(initial_message, car_name, &min_floor, &max_floor) != 0) {
        printf("Failed to parse car info.\n");
        close(client_fd);
        return;
    }

    pthread_mutex_lock(&cars_mutex);
    int car_idx = -1;
    for (int i = 0; i < MAX_CARS; i++) {
        if(!cars[i].in_use) {
            car_idx = i;
            break;
        }
    }
    if (car_idx == -1){
        pthread_mutex_unlock(&cars_mutex);
        printf("Max cars reached. Rejecting car %s.\n", car_name);
        close(client_fd);
        return;
    }
    //car is good to go. Lets register the new car
    Car *car  = &cars[car_idx];
    car->in_use = 1;
    car->socket_fd = client_fd;
    strncpy(car->car_name, car_name, sizeof(car->car_name) -1);
    car->floor_min = min_floor;
    car->floor_max = max_floor;
    car-> queue_size = 0;
    //Initial status is unkwon until the first update
    strcpy(car->status, "Unknown");
    car->current_floor = min_floor;

    //Finished handlign the data lets unloick the mutex
    pthread_mutex_unlock(&cars_mutex);
    printf("Car %s regisered (Floors %d to %d).\n", car_name, min_floor, max_floor);

    //Loop for a status update
    while(1) {
        char* msg_buffer = receive_message(client_fd);
        if (msg_buffer == NULL) break;
        
        int floor;
        char status_buf[BUFFER_SIZE];
        if(parse_status_info(msg_buffer, &floor, status_buf) == 0) {
            //altering the car so lock the mutex
            pthread_mutex_lock(&cars_mutex);
            car->current_floor = floor;
            strncpy(car->status, status_buf, sizeof(car->status) -1);
            car->status[sizeof(car->status) - 1] = '\0';

            //If the car has arrived open the doors and service the queue
            if(car->queue_size > 0 && car->current_floor == car->queue[0] &&
                (strcmp(car->status, "Open") == 0 || strcmp(car->status, "Opening") == 0)) {
                remove_from_queue(car->queue, &car->queue_size, 0);
                send_next_destination(car);
            }
            pthread_mutex_unlock(&cars_mutex);
        }
        free(msg_buffer);
    }
    
    //The car has disconnected 
    printf("Car %s disconnected.\n", car_name);
    pthread_mutex_lock(&cars_mutex);
    car->in_use = 0;
    pthread_mutex_unlock(&cars_mutex);
    close(client_fd);
}

/**
 * @brief Handles a transient connection froma a call pad
 */
void handle_call_connection(int client_fd, const char* call_message){
    int source_floor, dest_floor;

    if(call_message == NULL || parse_call_info(call_message, &source_floor, &dest_floor) != 0) {
        printf("Faield to parse call info.\n");
        return;
    }

    printf("Received call from floor %d to %d.\n", source_floor, dest_floor);
    schedule_request(source_floor, dest_floor, client_fd);
}

/**
 * @brief sets up signal handler for graceful shutdwon as outlined by the task
 * (SIGINT), also for pipe errors (SIGPIPE)
 */

 void setup_signal_handlers(void) {
    //Ignore teh SIGPIPE to rpevent crashing on write to closed socket
    signal(SIGPIPE, SIG_IGN);

    //Setup SIGINT (CTRL+C) handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);

 }


 /**
  * @brief Async-signal safge handle for SIGINT
  */
void sigint_handler(int signum) {
    (void)signum;
    shutdown_requested = 1;
}


/**
 * SCHEDULING LOGIC 
 */


/**
 * @brief Finds the best car for a request and updates queue
 */

 void schedule_request(int source_floor, int dest_floor, int client_fd) {
    int best_car_idx = -1;
    int min_cost = 1000;
    int best_final_len = 1000;
    //Lock the mutex as we found the best, so no one can cahnge it 
    pthread_mutex_lock(&cars_mutex);
    for (int i = 0; i < MAX_CARS; i++) {
        if (!cars[i].in_use) continue; 
        //Elevator car must eb able to service both the floor as a rule
        if (source_floor < cars[i].floor_min || source_floor > cars[i].floor_max
            || dest_floor < cars[i].floor_min || dest_floor > cars[i].floor_max) {
                continue;
            }
        int pickup_idx, final_len;
        int cost = calculate_insertion_cost(&cars[i], source_floor, dest_floor,
        &pickup_idx, &final_len);

        if (cost < 0) continue; //An invalid insertion do not consider

        /*
        Using the lowest cost by finding the earliest pickup index. If two
        have the same it is the shorter final queue length as a tiebreaker.
        costr is defined as the idnex of the pickup floor a lower index means the passanger
        will be picked up sooner realtive to the car's current plan. This heautristic 
        directly prioritizes passenger wait time for the new call. It is also a balanced apporach
        taht isn't computationally inexpensive 
        */

        if (cost < min_cost || (cost == min_cost && final_len < best_final_len)) {
            min_cost = cost;
            best_final_len = final_len;
            best_car_idx = i;
        }
    }
    if (best_car_idx != -1) {
        //No error 
        Car *chosen_car = &cars[best_car_idx];
        int old_head = (chosen_car->queue_size > 0) ? chosen_car->queue[0] : -1000;

        //recalc the best insertion to get final queue state
        int pickup_idx, final_len;
        calculate_insertion_cost(chosen_car, source_floor, dest_floor, &pickup_idx, &final_len);
        int temp_queue[MAX_QUEUE_DEPTH];
        int temp_size = chosen_car->queue_size;
        memcpy(temp_queue, chosen_car->queue, sizeof(int) *temp_size);

        insert_into_queue(temp_queue, &temp_size, pickup_idx, source_floor);

        //Find where to insret dest
        int dest_idx = -1;
        Direction travel_dir = (dest_floor > source_floor) ? DIR_UP : DIR_DOWN;

        for (int i = pickup_idx + 1; i < temp_size; i++) {
            if (travel_dir == DIR_UP){
                if(dest_floor < temp_queue[i]) {
                    dest_idx = i;
                    break;
                }
            } else { // directioon down
                if (dest_floor > temp_queue[i]) {
                    dest_idx = i;
                    break;
                }

            }
        }
        if (dest_idx == -1) dest_idx = temp_size;

        insert_into_queue(temp_queue, &temp_size, dest_idx, dest_floor);

        //Commit the change by memcpy
        memcpy(chosen_car->queue, temp_queue, sizeof(int) *temp_size);
        chosen_car->queue_size = temp_size;
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), "CAR %s", chosen_car->car_name);
        send_message(client_fd, response);

        printf("Assigned call (%d->%d) to Car %s. New queue size: %d\n",
        source_floor, dest_floor, chosen_car->car_name, chosen_car->queue_size);

        //If the ehad of the queue has changed send a new destination
        if (chosen_car->queue[0] != old_head) {
            send_next_destination(chosen_car);
        }
    } else {
        send_message(client_fd, "UNAVAILABLE");
        printf("Call (%d->%d) is unavailable.\n", source_floor, dest_floor);
    }
    //We are done so unlock the mutex
    pthread_mutex_unlock(&cars_mutex);
 }



 /**
  * @brief Calculates the cost of inserting a new request into a cars's queue.
  * @return the index of the pickup floor (cost) or -1 if impossible
  */
 int calculate_insertion_cost(const Car *car, int source, int dest, int *pickup_idx, int *final_len) {
    int effective_floor = car->current_floor;
    if (car-> queue_size > 0) {
        //If closing / between we are effectively at the next floor
        if (strcmp(car->status, "Closing") == 0 || strcmp(car->status, "Between") == 0) {
            effective_floor = car->queue[0];
        }
    }
    Direction request_dir = (dest > source) ? DIR_UP : DIR_DOWN; // is it up or down
    //Insert as early as possiblew
    int current = effective_floor;
    for(int i = 0; i <= car->queue_size; i++) {
        int next = (i < car->queue_size) ? car->queue[i] : current;// if at end stay

        Direction segment_dir = (next > current) ? DIR_UP : DIR_DOWN;
        //Condition can we pick up on thsi segment. We are moving in same direction as request
        // the source floor is between opur current and next stop

        if (segment_dir == request_dir) {
            if((request_dir == DIR_UP && source >= current && source < next) ||
            (request_dir == DIR_DOWN && source <= current && source > next)) {
                //found a valid ickup point. now can we drop off without reversing
                for(int j = i; j <= car->queue_size; j++) {
                    int check_next = (j < car->queue_size) ? car->queue[j] : dest;
                    // Check for direction reversal before drop-off
                    if ((request_dir == DIR_UP && check_next < source) ||
                        (request_dir == DIR_DOWN && check_next > source)) {
                        goto next_segment; // Fails direction rule, break inner loop
                    }

                    // Check if we can drop off at or before the next stop
                    if (j == car->queue_size ||
                       (request_dir == DIR_UP && dest <= check_next) ||
                       (request_dir == DIR_DOWN && dest >= check_next)) {
                        
                        // Valid insertion found
                        *pickup_idx = i;
                        *final_len = car->queue_size + 2;
                        return *pickup_idx;
                    }
                }
            }
        }
        next_segment:
            current = next;
    }
    //If we couldn't insert it on the way, add it to the end. The csot is higher,
    //which means it is waiting for jobs to finish
    *pickup_idx = car->queue_size;
    *final_len = car->queue_size +2;
    return *pickup_idx;
 }

 /**
  * Queue maangement
  */

  void insert_into_queue(int *queue, int *size, int index, int value) {
    if (*size >= MAX_QUEUE_DEPTH || index > *size) return;
    //Don't add if already the last iutenm or dupliate of the nex prev item
    if (index > 0 && queue[index-1] == value) return;

    memmove(&queue[index + 1 ], & queue[index], (*size - index) * sizeof(int));
    queue[index] = value;
    (*size)++;
  }

  void remove_from_queue(int *queue, int *size, int index) {
    if (*size == 0 || index >= *size) return;
    memmove(&queue[index], &queue[index +1], (*size - 1 - index) *sizeof(int));
    (*size)--;
  }

  void send_next_destination(Car *car) {
    if (car->queue_size > 0) {
        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "FLOOR %d", car->queue[0]);
        send_message(car->socket_fd,msg);
    }
  }


int parse_car_info(const char *buffer, char *name, int *min_floor, int *max_floor) {
    char min_str[MAX_FLOOR_STR_LEN], max_str[MAX_FLOOR_STR_LEN];
    if (sscanf(buffer, "CAR %s %s %s", name, min_str, max_str) != 3) {
        return -1;
    }
    *min_floor = floor_to_int(min_str);
    *max_floor = floor_to_int(max_str);
    return 0;
}
int parse_call_info(const char *buffer, int *source, int *dest){
    char source_str[MAX_FLOOR_STR_LEN], dest_str[MAX_FLOOR_STR_LEN];
    if (sscanf(buffer, "CALL %s %s", source_str, dest_str) != 2) {
        return -1;
    }
    *source = floor_to_int(source_str);
    *dest = floor_to_int(dest_str);
    return 0;
}
int parse_status_info(const char *buffer, int *floor, char *status_buf) {
    char floor_str[MAX_FLOOR_STR_LEN];
    char dest_str [MAX_FLOOR_STR_LEN];
    // The format is: STATUS <status> <current_floor> <dest_floor>
    // But we only care about status and current_floor
    int result = sscanf(buffer, "STATUS %s %s %s", status_buf, floor_str, dest_str);
    if (result < 2) {
        return -1;
    }
    *floor = floor_to_int(floor_str);
    return 0;
}

 void safe_write(int fd, const char *message) {
    if (write(fd, message, strlen(message)) < 0) {
        perror("write failed");
    }
}
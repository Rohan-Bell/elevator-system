#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#include "shared.h"
#include <time.h>
#include <sys/select.h>

static car_shared_mem *shm = NULL;
static int shm_fd = -1;
static char shm_name[256];
static int delay_ms = 0;
static int controller_fd = -1;
static pthread_mutex_t controller_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int should_exit = 0;
static char car_name[64];
static char lowest_floor[8];
static char highest_floor[8];

static volatile sig_atomic_t cleanup_in_progress = 0;
static volatile int destination_changed = 0; //bool to see when dest changed

//Function definitions 
void setup_signal_handler(void);
void signal_handler(int sig);
void init_shared_memory(void);
void *controller_thread(void *arg);
void *main_operation_thread(void *arg);
int connect_to_controller(void);
void disconnect_from_controller(void);
void send_status_update(void);
int floor_compare(const char *f1, const char *f2);
void move_towards_destination(void);
void handle_buttons(void);
int is_in_range(const char *floor);
int my_usleep(__useconds_t usec); //Replacement for usleep as was getting errors with POSIX Source and wanted to use clock time


//Setip the signal handler
void setup_signal_handler(void) {
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

void signal_handler(int sig){
    if (sig == SIGINT) {
        should_exit = 1;
        cleanup_in_progress = 1;
        if (shm) {
            pthread_mutex_lock(&shm->mutex);
            pthread_cond_broadcast(&shm -> cond);
            pthread_mutex_unlock(&shm->mutex);
        }
    }
}

//Initialize the shared memory 

void init_shared_memory(void) {
    shm_fd = shm_open(shm_name, O_CREAT | O_EXCL |O_RDWR, 0666);
    int created = (shm_fd != -1);
    if (!created) {
        //already exists
        shm_fd = shm_open(shm_name, O_RDWR, 0666);
        if (shm_fd == -1) {
            perror("shm_open");
            exit(1);
        }
    } else {
        //Memory exists lets set it's size
        if(ftruncate(shm_fd, sizeof(car_shared_mem)) ==-1) {
            perror("ftruncate");
            exit(1);
        }
    }

    shm = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if(shm == MAP_FAILED){
        perror("mmap");
        exit(1);
    }  
    if (created) {
        init_shm(shm);
        //set starting floor
        strncpy(shm->current_floor, lowest_floor, sizeof(shm->current_floor) -1);
        shm->current_floor[sizeof(shm->current_floor) -1] = '\0';
        strncpy(shm->destination_floor, lowest_floor, sizeof(shm->destination_floor) -1);
        shm->destination_floor[sizeof(shm->destination_floor) -1] = '\0';
    }
}


/// @brief Connect to the controller using the socket address. Uses IPV4 not IPV6
/// @param  void (Everythign is predfined)
/// @return int return (-1 if failed, sockfd if success)
int connect_to_controller(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROLLER_PORT);
    inet_pton(AF_INET, CONTROLLER_IP, &addr.sin_addr);

    if(connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(sockfd);
        return -1;
    }
    //Send a CAR rego message
    char msg[256];
    snprintf(msg, sizeof(msg), "CAR %s %s %s", car_name, lowest_floor, highest_floor);
    send_message(sockfd, msg);

    return sockfd;
}

void disconnect_from_controller(void) {
    //Lock the mutex while we close the controller file descriptor unlock after
    pthread_mutex_lock(&controller_mutex);
    if(controller_fd != -1) {
        close(controller_fd);
        controller_fd = -1;
    }
    pthread_mutex_unlock(&controller_mutex);
}

void send_status_update(void) {
    pthread_mutex_lock(&controller_mutex); //Sending something realted to controller mutex lock it up
    if(controller_fd != -1){
        char msg[256];
        pthread_mutex_lock(&shm->mutex);
        snprintf(msg, sizeof(msg), "STATUS %s %s %s", shm->status, shm->current_floor, shm->destination_floor);
        pthread_mutex_unlock(&shm->mutex);
        send_message(controller_fd, msg);
    }
    //sent our message lets unlock it for other tasks
    pthread_mutex_unlock(&controller_mutex);
}

/// @brief Performs a comaprsion between two floors 
/// @param f1 First floor that is desired to be compared
/// @param f2  Second floor that is desired to be compared
/// @return Returns <0 if f1 < f2m 0 if equal and >0 if f1>f2
int floor_compare(const char *f1, const char *f2) {
    int i1 = floor_to_int(f1);
    int i2 = floor_to_int(f2);
    return i1 - i2;
}


int is_in_range(const char *floor) {
    return floor_compare(floor, lowest_floor) >= 0 && floor_compare(floor, highest_floor) <= 0; //Esnure floor is between range
}

void move_one_floor_towards(char *current, const char *dest, size_t buffer_size) {
    int current_int = floor_to_int(current);
    int dest_int = floor_to_int(dest);
    if (current_int < dest_int) {
        //move up floor
        current_int++;
    } else if (current_int > dest_int) {
        current_int--;
    }

    //Convert aback to string
    int_to_floor(current_int, current, buffer_size);
}




void *controller_thread(void *arg) {
    (void)arg;
    while(!should_exit) {
        pthread_mutex_lock(&shm->mutex);
        //Wait for the safety system
        while(shm->safety_system != 1 && !should_exit && shm->individual_service_mode == 0 && shm->emergency_mode == 0) {
            pthread_cond_wait(&shm->cond, &shm->mutex);
        }
        pthread_mutex_unlock(&shm->mutex);

        if(should_exit) break; // ctrl + c pressed
        //Check to see if we should be connected
        pthread_mutex_lock(&shm->mutex);
        int should_connect = (shm->individual_service_mode == 0 && shm->emergency_mode == 0 && shm->safety_system == 1);
        pthread_mutex_unlock(&shm->mutex);

        if(should_connect && controller_fd == -1) {
            controller_fd = connect_to_controller();
            if(controller_fd != -1) {
                //Controller connected send update
                send_status_update();
            } else {
                my_usleep(delay_ms  * MILLISECOND);
                continue;
            }
        }

        //Handle messages the controller
        if(controller_fd != -1){
                // Use a timeout-based receive or non-blocking read
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(controller_fd, &read_fds);
            struct timeval timeout = {0, delay_ms * MILLISECOND};  // timeout = delay_ms
            int ready = select(controller_fd + 1, &read_fds, NULL, NULL, &timeout);
            if (ready > 0) {
                // printf("[DEBUG] controller_thread: About to call receive_msg, fd=%d\n", controller_fd);
                char *recv_msg = receive_msg(controller_fd);
                // printf("[DEBUG] controller_thread: Received message: '%s' (or NULL)\n", 
                //     recv_msg ? recv_msg : "NULL");
                if(recv_msg == NULL) {
                    disconnect_from_controller();
                    break;
                }
                if(strncmp(recv_msg, "FLOOR", 5) == 0) {
                    char floor[8];
                    sscanf(recv_msg + 6, "%s", floor);
                    pthread_mutex_lock(&shm->mutex);
                    //Allow setting destiantion event between
                    if(is_in_range(floor)) {
                        strncpy(shm->destination_floor, floor, sizeof(shm->destination_floor) -1);
                        destination_changed =1;
                        pthread_cond_broadcast(&shm->cond);
                    }
                    pthread_mutex_unlock(&shm->mutex); //Unlock the shared memorey as we are done with it
                }
            free(recv_msg); //free up any unnecessary buffer
            }
        } else {
            //only sleep if controller not connected
            my_usleep(delay_ms  * MILLISECOND);
        }
    }
    return NULL;
}

void add_ms(struct timespec *t, long ms) {
    t->tv_sec  += ms / 1000;
    t->tv_nsec += (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}


void open_door_sequence(void) {
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    //printf("[TIMING] open_door_sequence START at %ld.%09ld\n", start_time.tv_sec, start_time.tv_nsec);

    // --- Opening at t=0 ---
    pthread_mutex_lock(&shm->mutex);
    shm->open_button = 0;
    strcpy(shm->status, "Opening");
    pthread_cond_broadcast(&shm->cond);
    pthread_mutex_unlock(&shm->mutex);
    //printf("[TIMING] Status set to Opening at t=0\n");
    send_status_update();

    // --- Open at t=delay_ms ---
    struct timespec open_time = start_time;
    add_ms(&open_time, delay_ms);
    //printf("[TIMING] Will transition to Open at t=%d ms (target: %ld.%09ld)\n", 
        //    delay_ms, open_time.tv_sec, open_time.tv_nsec);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &open_time, NULL);

    struct timespec now1;
    clock_gettime(CLOCK_MONOTONIC, &now1);

    //printf("[TIMING] Woke up after %ld ms, transitioning to Open\n", elapsed1);

    pthread_mutex_lock(&shm->mutex);
    if(strcmp(shm->status, "Opening") == 0) {
        strcpy(shm->status, "Open");
        pthread_cond_broadcast(&shm->cond);
    }
    pthread_mutex_unlock(&shm->mutex);
    send_status_update();

    // --- Wait in Open state until close_button or t=2*delay_ms ---
    struct timespec close_time = start_time;
    add_ms(&close_time, 2 * delay_ms);
    //printf("[TIMING] Will auto-close at t=%d ms (target: %ld.%09ld)\n", 
        //    2 * delay_ms, close_time.tv_sec, close_time.tv_nsec);

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        pthread_mutex_lock(&shm->mutex);
        
        // 1. If user pressed close_button early
        if (shm->close_button == 1 && strcmp(shm->status, "Open") == 0) {
            shm->close_button = 0;
            struct timespec close_button_time;
            clock_gettime(CLOCK_MONOTONIC, &close_button_time);
            //printf("[TIMING] Close button pressed at t=%.3f ms, recalculating closed_time\n", elapsed * 1000);
            
            strcpy(shm->status, "Closing");
            pthread_cond_broadcast(&shm->cond);
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            break;
        }
        
        // if state changed externally
        if (strcmp(shm->status, "Open") != 0) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }
        
        pthread_mutex_unlock(&shm->mutex);

        // 2. If scheduled close time has arrived
        if ((now.tv_sec > close_time.tv_sec) ||
            (now.tv_sec == close_time.tv_sec && now.tv_nsec >= close_time.tv_nsec)) {
            //MING] Auto-close time reached at t=%ld ms, transitioning to Closing\n", elapsed_auto);
            pthread_mutex_lock(&shm->mutex);
            if(strcmp(shm->status, "Open") == 0) {
                strcpy(shm->status, "Closing");
                pthread_cond_broadcast(&shm->cond);
            }
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            break;
        }

        my_usleep(1000);  // 1ms
        
    }

    // --- Closing phase ---struct timespec closing_start;
    struct timespec closing_start;
    clock_gettime(CLOCK_MONOTONIC, &closing_start);
    struct timespec new_closed_time = closing_start;
    add_ms(&new_closed_time, delay_ms);

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &new_closed_time, NULL);

    pthread_mutex_lock(&shm->mutex);
    if (strcmp(shm->status, "Closing") == 0) {
        strcpy(shm->status, "Closed");
        pthread_cond_broadcast(&shm->cond);
    }
    pthread_mutex_unlock(&shm->mutex);
    send_status_update();
}

void handle_buttons(void) {
    pthread_mutex_lock(&shm->mutex);
    
    // In individual service mode, handle buttons immediately
    if (shm->individual_service_mode == 1) {
        if (shm->close_button == 1 && strcmp(shm->status, "Open") == 0) {
            shm->close_button = 0;
            strcpy(shm->status, "Closing");
            pthread_cond_broadcast(&shm->cond);
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            
            my_usleep(delay_ms * MILLISECOND);
            
            pthread_mutex_lock(&shm->mutex);
            if(strcmp(shm->status, "Closing") == 0) {
                strcpy(shm->status, "Closed");
                pthread_cond_broadcast(&shm->cond);
            }
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            return;
        }
        
        if (shm->open_button == 1 && strcmp(shm->status, "Closed") == 0) {
            shm->open_button = 0;
            strcpy(shm->status, "Opening");
            pthread_cond_broadcast(&shm->cond);
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            
            my_usleep(delay_ms * MILLISECOND);
            
            pthread_mutex_lock(&shm->mutex);
            if(strcmp(shm->status, "Opening") == 0) {
                strcpy(shm->status, "Open");
                pthread_cond_broadcast(&shm->cond);
            }
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            return;
        }
        
        pthread_mutex_unlock(&shm->mutex);
        return;
    }
    
    // Normal mode - close button has highest priority
    if (shm->close_button == 1 && strcmp(shm->status, "Open") == 0) {
        shm->close_button = 0;
        strcpy(shm->status, "Closing");
        pthread_cond_broadcast(&shm->cond);
        pthread_mutex_unlock(&shm->mutex);
        send_status_update();
        
        my_usleep(delay_ms * MILLISECOND);
        
        pthread_mutex_lock(&shm->mutex);
        if(strcmp(shm->status, "Closing") == 0) {
            strcpy(shm->status, "Closed");
            pthread_cond_broadcast(&shm->cond);
        }
        pthread_mutex_unlock(&shm->mutex);
        send_status_update();
        return;
    }

    if(shm->open_button == 1 && strcmp(shm->current_floor, shm->destination_floor) == 0 && 
        (strcmp(shm->status, "Closing") == 0 || strcmp(shm->status, "Closed") == 0)) {
        shm->open_button = 0;
        pthread_mutex_unlock(&shm->mutex);
        open_door_sequence();
        return;
    }
    
    pthread_mutex_unlock(&shm->mutex);
}

void *main_operation_thread(void *arg) {
    (void)arg;
    struct timespec last_safety_check;
    clock_gettime(CLOCK_MONOTONIC, &last_safety_check);
    
    while (!should_exit) {
        
        // Safety system heartbeat check based on actual time
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_safety_check.tv_sec) * MILLISECOND + 
                         (now.tv_nsec - last_safety_check.tv_nsec) / 1000000;
        
        if (elapsed_ms >= delay_ms) {
            last_safety_check = now;
            
            pthread_mutex_lock(&shm->mutex);
            //Only check safety system if connected and not in emergency mode or indiviudal service mode
            if (controller_fd != -1 && shm->individual_service_mode == 0 && shm->emergency_mode == 0) {
                if (shm->safety_system == 1) {
                    shm->safety_system = 2;
                    pthread_cond_broadcast(&shm->cond);
                } else if (shm->safety_system >= 3) {
                    printf("Safety system disconnected! Entering emergency mode.\n");
                    shm->emergency_mode = 1;
                    pthread_cond_broadcast(&shm->cond);
                    pthread_mutex_unlock(&shm->mutex);
                    pthread_mutex_lock(&controller_mutex);
                    if (controller_fd != -1) {
                        send_message(controller_fd, "EMERGENCY");
                        close(controller_fd);
                        controller_fd = -1;
                    }
                    pthread_mutex_unlock(&controller_mutex);
                    pthread_mutex_lock(&shm->mutex);
                }
            }
            pthread_mutex_unlock(&shm->mutex);
        }
        
        pthread_mutex_lock(&shm->mutex);
        int is_individual_mode = shm->individual_service_mode;
        int is_emergency = shm->emergency_mode;
        pthread_mutex_unlock(&shm->mutex);
        
        // Handle buttons (handles doors in individual service mode)
        if (is_individual_mode || !is_emergency) {
            handle_buttons();
        }
        
        pthread_mutex_lock(&shm->mutex);
        
        // Handle mode changes
        if (shm->individual_service_mode == 1) {
            if (controller_fd != -1) {
                pthread_mutex_unlock(&shm->mutex);
                pthread_mutex_lock(&controller_mutex);
                send_message(controller_fd, "INDIVIDUAL SERVICE");
                close(controller_fd);
                controller_fd = -1;
                pthread_mutex_unlock(&controller_mutex);
                pthread_mutex_lock(&shm->mutex);
            }
            
            // Handle manual movement in individual service mode - floor by floor
            if (strcmp(shm->status, "Closed") == 0 && strcmp(shm->current_floor, shm->destination_floor) != 0) {
                if (!is_in_range(shm->destination_floor)) {
                    strncpy(shm->destination_floor, shm->current_floor, sizeof(shm->destination_floor) - 1);
                    pthread_mutex_unlock(&shm->mutex);
                } else {
                    strcpy(shm->status, "Between");
                    pthread_cond_broadcast(&shm->cond);
                    pthread_mutex_unlock(&shm->mutex);
                    
                    my_usleep(delay_ms * MILLISECOND);
                    
                    pthread_mutex_lock(&shm->mutex);
                    move_one_floor_towards(shm->current_floor, shm->destination_floor, sizeof(shm->current_floor));
                    
                    // Check if we've arrived at destination
                    if (floor_compare(shm->current_floor, shm->destination_floor) == 0) {
                        strcpy(shm->status, "Closed");
                        pthread_cond_broadcast(&shm->cond);
                        pthread_mutex_unlock(&shm->mutex);
                    } else {
                        // Still moving, keep status as Between
                        pthread_mutex_unlock(&shm->mutex);
                    }
                }
            } else {
                pthread_mutex_unlock(&shm->mutex);
                my_usleep(1 * MILLISECOND);
            }
            continue;
        }
        
        if (shm->emergency_mode == 1) {
            pthread_mutex_unlock(&shm->mutex);
            continue;
        }
        
        // Normal operation
        if (strcmp(shm->status, "Closed") == 0) {
            int cmp = floor_compare(shm->current_floor, shm->destination_floor);
            
            if (cmp == 0 && destination_changed) {
                // Controller sent us to current floor - open doors
                destination_changed = 0;
                pthread_mutex_unlock(&shm->mutex);
                open_door_sequence();
            } else if (cmp != 0) {
                //Change status to between to start the actual journey
                strcpy(shm->status, "Between");
                pthread_cond_broadcast(&shm->cond);
                pthread_mutex_unlock(&shm->mutex);
                send_status_update(); // status between ... message

                //lets loop until we get to our destination
                while(floor_compare(shm->current_floor, shm->destination_floor) != 0 && !should_exit) {
                    if(strcmp(shm->status, "Between") == 0){
                        my_usleep(delay_ms  * MILLISECOND);
                        pthread_mutex_lock(&shm->mutex);
                        //Check fi we should still be moving i.e. not emergency not service
                        if (shm->emergency_mode == 0 && strcmp(shm->status, "Between") == 0){
                            move_one_floor_towards(shm->current_floor, shm->destination_floor, sizeof(shm->current_floor));
                            // printf("[DEBUG] main_op: Moving from '%s' toward '%s', status='%s'\n",
                            //         shm->current_floor, shm->destination_floor, shm->status);
                            //Check if we have arrived 
                            if (floor_compare(shm->current_floor, shm->destination_floor) ==0) {
                                //Destination has been reached and we need to unlock and start the door sequenece
                                destination_changed = 0; // Clear flag
                                pthread_mutex_unlock(&shm->mutex);
                                open_door_sequence();
                                break; // Nog longer in the movement loop leave it
                            } else {
                                //Not at the floor send a status update
                                pthread_mutex_unlock(&shm->mutex);
                                send_status_update();
                            }
                        } else {
                            pthread_mutex_unlock(&shm->mutex);
                            break;
                        }
                    } else {
                        break;
                    }
                    
                } 

            } else {
                //Current floor is the destination floor so we do not need to do anyhthing until given a new dest
                pthread_mutex_unlock(&shm->mutex);
                my_usleep(delay_ms * MILLISECOND);
            }
        } else {
            pthread_mutex_unlock(&shm->mutex);
            my_usleep(1 * MILLISECOND);
        }
    }
    
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <name> <lowest_floor> <highest_floor> <delay>\n", argv[0]);
        return 1;
    }
    
    strncpy(car_name, argv[1], sizeof(car_name) - 1);
    strncpy(lowest_floor, argv[2], sizeof(lowest_floor) - 1);
    strncpy(highest_floor, argv[3], sizeof(highest_floor) - 1);
    delay_ms = atoi(argv[4]);
    
    snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);
    
    setup_signal_handler();
    init_shared_memory();
    
    pthread_t ctrl_thread, main_thread;
    pthread_create(&ctrl_thread, NULL, controller_thread, NULL);
    pthread_create(&main_thread, NULL, main_operation_thread, NULL);
    
    pthread_join(main_thread, NULL);
    pthread_join(ctrl_thread, NULL);
    if (shm && !cleanup_in_progress) {
        pthread_mutex_destroy(&shm->mutex);
        pthread_cond_destroy(&shm->cond);
    }
    
    // Cleanup
    if (shm) {
        if(!cleanup_in_progress) {
            pthread_mutex_destroy(&shm->mutex);
            pthread_cond_destroy(&shm->cond);
        }
        munmap(shm, sizeof(car_shared_mem));
    }
    if (shm_fd != -1) {
        close(shm_fd);
    }
    shm_unlink(shm_name);
    
    return 0;
}


int my_usleep(__useconds_t usec) {
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}
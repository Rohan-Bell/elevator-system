#define _POSIX_C_SOURCE 199309L
#include <unistd.h>
#include "shared.h"
#include <time.h>

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

//Function definitions 
void setup_signal_handler(void);
void signal_handler(int sig);
void init_shared_memory(void);
void *controller_thread(void *arg);
void *main_operation_thread(void *arg);
void send_message(int fd, const char *msg);
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
        if (shm) {
            pthread_cond_broadcast(&shm -> cond);
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
        //Initialise the mutex with a shared attribute
        pthread_mutexattr_t mutex_attr;
        pthread_mutexattr_init(&mutex_attr);
        pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&shm->mutex, &mutex_attr);
        pthread_mutexattr_destroy(&mutex_attr);

        //Init the conditional variable with teh shared attribute
        pthread_condattr_t cond_attr;
        pthread_condattr_init(&cond_attr);
        pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
        pthread_cond_init(&shm->cond, &cond_attr);
        pthread_condattr_destroy(&cond_attr);
    }

    //Initialise the values using strncpy
    strncpy(shm->current_floor, lowest_floor, sizeof(shm->current_floor) -1);
    strncpy(shm->destination_floor, lowest_floor, sizeof(shm->destination_floor) -1);
    strcpy(shm->status, "Closed"); //closed to start
    //Set to default values
    shm->open_button = 0;
    shm->close_button = 0;
    shm->safety_system = 0;
    shm->door_obstruction = 0;
    shm->overload = 0;
    shm->emergency_stop =0;
    shm->emergency_mode = 0;
    shm-> individual_service_mode =0;
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
        snprintf(msg, sizeof(msg), "STATUS %s %s %s", shm->status, shm->current_floor, shm->destination_floor);
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

    int is_b1 = (f1[0] == 'B');
    int is_b2 = (f2[0] == 'B');

    if (is_b1 && is_b2) {
        //Both floors are a basement 
        int n1 = atoi(f1 + 1);
        int n2 = atoi(f2 + 1);
        return n2 - n1; //Reversed for basement because higher is lower
    } else if (is_b1) {
        //basdement < regular floor
        return -1;
    } else if (is_b2) {
        return 1; //floor 1 is larger second floor is basement
    } else {
        return atoi(f1) - atoi(f2); //Normal floor
    }
}


int is_in_range(const char *floor) {
    return floor_compare(floor, lowest_floor) >= 0 && floor_compare(floor, highest_floor) <= 0; //Esnure floor is between range
}

void move_one_floor_towards(char *current, const char *dest, size_t buffer_size) {
    int cmp = floor_compare(current, dest);
    if (cmp < 0) {
        //current < destination therefore go up
        if (current[0] == 'B') {
            //asement floor
            int n = atoi(current + 1);
            if (n == 1) {
                strncpy(current, "1", buffer_size -1 );
                current[buffer_size - 1] = '\0'; //Null terminator protection
            } else {
                snprintf(current, buffer_size, "B%d",n-1);
            }
        } else {
            int n = atoi(current);
            snprintf(current,  buffer_size, "%d", n+1);
        }
    } else {
        //Current > destination therefroe go down
        if(current[0] == 'B') {
            int n = atoi(current + 1);
            snprintf(current, buffer_size, "B%d", n +1);
        } else {
            int n = atoi(current);
            if (n == 1) {
                strncpy(current, "B1", buffer_size - 1); // no floor 0
                current[buffer_size - 1] = '\0'; //Null terminator protection
            } else {
                snprintf(current, buffer_size, "%d", n -1);
            }
        }
    }
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
            char *recv_msg = receive_msg(controller_fd);
            if(recv_msg == NULL) {
                disconnect_from_controller();
                continue;
            }
            if(strncmp(recv_msg, "FLOOR", 6) == 0) {
                char floor[8];
                sscanf(recv_msg + 6, "%s", floor);
                pthread_mutex_lock(&shm->mutex);
                //Allow setting destiantion event between
                if(is_in_range(floor)) {
                    strncpy(shm->destination_floor, floor, sizeof(shm->destination_floor) -1);
                    pthread_cond_broadcast(&shm->cond);
                }
                pthread_mutex_unlock(&shm->mutex); //Unlock the shared memorey as we are done with it
                send_status_update();
            }
            free(recv_msg); //free up any unnecessary buffer
        }
    }
    my_usleep(delay_ms  * MILLISECOND);
    return NULL;
}


void open_door_sequence(void) {
    struct timespec start_time, now;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    //set the opening state
    pthread_mutex_lock(&shm->mutex); //lock set door status to Opening and then unlock mutex
    strcpy(shm->status, "Opening");
    pthread_cond_broadcast(&shm->cond);
    pthread_mutex_unlock(&shm->mutex);
    send_status_update();

    my_usleep(delay_ms  * MILLISECOND);

    //Calcualte how much time has elapsed
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - start_time.tv_sec) * MILLISECOND +
                        (now.tv_nsec - start_time.tv_nsec) / 1000000;
    //Adjust the remaing sleep time to account for overhead of sending status updates
    long remaining_ms = delay_ms - elapsed_ms;
    if (remaining_ms > 0) {
        my_usleep(remaining_ms * MILLISECOND);
    }

    //Now set to the Open State - this shopuld happen at the delay_ms and include the overhead of sending status
    pthread_mutex_lock(&shm->mutex);
    if(strcmp(shm->status, "Opening") == 0) {
        strcpy(shm->status, "Open");
        pthread_cond_broadcast(&shm->cond);
    }
    pthread_mutex_unlock(&shm->mutex);
    send_status_update();

    //Wait while in the open state but we got to wake up periodically to check if the close button ahs been pressedd
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    while (1) {
        pthread_mutex_lock(&shm->mutex);

        //Lets check if the close button was pressed while the door was open otherwsie we will do normal closing sequence
        if (shm->close_button == 1 && strcmp(shm->status, "Open") == 0) {
            shm->close_button = 0;
            strcpy(shm->status, "Closing");
            pthread_cond_broadcast(&shm->cond);
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            break; // exit the waiting loop immediately
        }
        //Check fi status changed for any other reason
        if(strcmp(shm->status, "Open") != 0) {
            pthread_mutex_unlock(&shm->mutex);
            break;
        }
        pthread_mutex_unlock(&shm->mutex);

        //Havce we waited long enough for the door to close?
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start_time.tv_sec) * MILLISECOND +
                        (now.tv_nsec - start_time.tv_nsec) / 1000000;
        if (elapsed_ms >= delay_ms) {
            //Time is up lets close on up
            pthread_mutex_lock(&shm->mutex);
            if(strcmp(shm->status, "Open") == 0) {
                strcpy(shm->status, "Closing");
            }
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            break;
        }

        //Sleep for a very short time before checking again
        my_usleep(1* MILLISECOND);


    }

    //Complete the closing sequence
    my_usleep(delay_ms * MILLISECOND);
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
    //Closed button ahs highest priority
    if (shm->close_button == 1) {
        shm->close_button = 0; // reset button state first 
        if (strcmp(shm->status, "Open") == 0) {
            //Transitioin t closing
            strcpy(shm->status, "Closing");
            pthread_cond_broadcast(&shm->cond);
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            //wait for closing delay
            my_usleep(delay_ms * MILLISECOND);
            //Complete the close
            pthread_mutex_lock(&shm->mutex);
            if(strcmp(shm->status, "Closing") == 0) {
                strcpy(shm->status, "Closed");
                pthread_cond_broadcast(&shm->cond);
            }
            pthread_mutex_unlock(&shm->mutex);
            send_status_update();
            return;
        }
    }

    if(shm->open_button == 1) {
        shm->open_button = 0;
        //Trigger open sequenecv if the dorrs are closed or clsoing and the car is stationary

        if (strcmp(shm->current_floor, shm->destination_floor) == 0 && (strcmp(shm->status, "Closing") == 0 || strcmp(shm->status, "Closed") == 0)) {
            pthread_mutex_unlock(&shm->mutex);
            open_door_sequence();
            return; //The seuqnece was handled so we can exit function early
        }
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
        
        handle_buttons();
        
        pthread_mutex_lock(&shm->mutex);
        
        // Handle mode changes
        if (shm->individual_service_mode == 1) {
            if (strcmp(shm->status, "Closed") == 0 || strcmp(shm->status, "Open") == 0) {
                if (controller_fd != -1) {
                    pthread_mutex_unlock(&shm->mutex);
                    pthread_mutex_lock(&controller_mutex);
                    send_message(controller_fd, "INDIVIDUAL SERVICE");
                    close(controller_fd);
                    controller_fd = -1;
                    pthread_mutex_unlock(&controller_mutex);
                    pthread_mutex_lock(&shm->mutex);
                }
                
                // Handle manual movement in individual service mode
                if (strcmp(shm->status, "Closed") == 0 && strcmp(shm->current_floor, shm->destination_floor) != 0) {
                    if (!is_in_range(shm->destination_floor)) {
                        strncpy(shm->destination_floor, shm->current_floor, sizeof(shm->destination_floor) - 1);
                    } else {
                        strcpy(shm->status, "Between");
                        pthread_cond_broadcast(&shm->cond);
                        pthread_mutex_unlock(&shm->mutex);
                        
                        my_usleep(delay_ms  * MILLISECOND);
                        
                        pthread_mutex_lock(&shm->mutex);
                        strncpy(shm->current_floor, shm->destination_floor, sizeof(shm->current_floor) - 1);
                        strcpy(shm->status, "Closed");
                        pthread_cond_broadcast(&shm->cond);
                    }
                }
            }
            pthread_mutex_unlock(&shm->mutex);
            continue;
        }
        
        if (shm->emergency_mode == 1) {
            pthread_mutex_unlock(&shm->mutex);
            continue;
        }
        
        // Normal operation
        if (strcmp(shm->status, "Closed") == 0) {
            int cmp = floor_compare(shm->current_floor, shm->destination_floor);
            
            if (cmp != 0) {
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
                            //Check if we have arrived 
                            if (floor_compare(shm->current_floor, shm->destination_floor) ==0) {
                                //Destination has been reached and we need to unlock and start the door sequenece
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
    
    // Cleanup
    if (shm) {
        pthread_mutex_destroy(&shm->mutex);
        pthread_cond_destroy(&shm->cond);
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
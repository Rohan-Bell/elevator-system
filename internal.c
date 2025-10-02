
/*
Open sets the open_button in shared memory to 1
Close sets close_button to 1
stop sets emergency_stop to 1
service_on sets individual_service_mode in shared memory segment to 1 and emergency_mode to 0
service_off sets individual_service_mode in the sharede memory segment to 0
up sets the destination floor to the enxt floor up from current floor. useabl when individyyal service node, elevator not moving and door closed
down sets the dest floor to the next down from current. Usable in service mode, elevtor not nmoving and door closed
*/

#include "shared.h"

//Check to see if it is a basement or normal floor
int is_basement_floor(const char* floor) {
    return floor[0] == 'B';
}

int get_floor_number(const char* floor) {
    if (is_basement_floor(floor)) {
        return atoi(floor + 1);
    } else {
        return atoi(floor);
    }
}



/// @brief Get the enxt floor up. Need to consider that 0 is not a floor (ground)
/// @param current The current floor on
/// @param next  The desired floor
/// @param n size of next buffer as was getting warnings
void get_next_floor_up(const char* current, char* next, size_t n) {
    //Basemenet logic is that the number decreases. But no 0 so would go to 1
    if (is_basement_floor(current)) {
        int basement_num = get_floor_number(current);
        if (basement_num == 1) {
            snprintf(next , n, "1");
        } else {
            snprintf(next, n, "B%d", basement_num - 1);
        }
    } else {
        //Is a regular floor
        int floor_num = get_floor_number(current);
        snprintf(next, n, "%d", floor_num + 1);
    }
}


/// @brief Get the next floor down. Need to consider that 0 is not a floor (ground)
/// @param current The current floor
/// @param next  The desired floor
void get_next_floor_down(const char* current, char* next, size_t n) {
    //Basemenet logic is that the number decreases. down to 99
    if (is_basement_floor(current)) {
        int basement_num = get_floor_number(current);
        snprintf(next, n, "B%d", basement_num + 1);
    } else {
        //Is a regular floor
        int floor_num = get_floor_number(current);
        if (floor_num == 1) {
            snprintf(next, n, "B1");
        }
        else {
            snprintf(next, n, "%d", floor_num - 1);
        }
    }
}



int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Not correct number of arguments");
        exit(1);
    }

    const char* car_name = argv[1];
    const char* operation = argv[2];

    //Build the shared memory object name
    char shm_name[256];
    snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);

    //Open the shared memory segment 
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1) {
        //Shared memory failed, print debugging
        printf("Unable to access car %s.\n", car_name);
        exit(1);
    }

    //Now that we have opened it, lets map the shared mem
    car_shared_mem *shm = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        printf("Unable to access car %s.\n", car_name);
        close(fd); //Close the file descript up as it failed
        exit(1);
    }

    //We don't need the file descriptor anymore as the shared mem is mapped, lets close it up
    close(fd);

    //Lock the mutext before accessiog shread memory
    pthread_mutex_lock(&shm->mutex);

    //Procdess the operation
    if(strcmp(operation, "open") == 0) {
        shm->open_button = 1;
    }
    else if (strcmp(operation, "close") == 0) {
        shm->close_button = 1;
    }
    else if (strcmp(operation, "stop") == 0) {
        shm->emergency_stop = 1;
    }
     else if (strcmp(operation, "service_on") == 0) {
        shm->individual_service_mode = 1;
        shm->emergency_mode = 0;
     }
     else if (strcmp(operation, "service_off") == 0) {
        shm->individual_service_mode = 0;
     } else if (strcmp(operation, "up") == 0) {
        //We want to go up. Lets see if we are even allowed to go up./
        if(!shm -> individual_service_mode) {
            pthread_mutex_unlock(&shm->mutex); //We open the mutex before exiting so other processes don't deadlock
            //operation is only allowed in service mode
            printf("Operation only allowed in service mode.\n");
            munmap(shm, sizeof(car_shared_mem));
            exit(1);
        }
        //Ensure not in a place where it is open in any means
        if (strcmp(shm->status, "Open") == 0 || strcmp(shm->status, "Opening") ==0 || strcmp(shm->status, "Closing") == 0) {
            pthread_mutex_unlock(&shm->mutex);
            printf("Operation not allowed while doors are open.\n");
            munmap(shm, sizeof(car_shared_mem));
            exit(1);
        }
        if (strcmp(shm->status, "Between") == 0)  {
            pthread_mutex_unlock(&shm->mutex);
            printf("Operation not allowed while elevator is moving.\n");
            munmap(shm, sizeof(car_shared_mem));
            exit(1);
        }
        //We have passed all our checks
        //Set the desitation to next floor up
        char next_floor[12];
        get_next_floor_up(shm->current_floor, next_floor, sizeof(next_floor));
        strcpy(shm->destination_floor, next_floor);

     } else if (strcmp(operation, "down") == 0) {
        //Maybe we do these checks in a function?
        //We want to go up. Lets see if we are even allowed to go up./
        if(!shm -> individual_service_mode) {
            pthread_mutex_unlock(&shm->mutex); //We open the mutex before exiting so other processes don't deadlock
            //operation is only allowed in service mode
            printf("Operation only allowed in service mode.\n");
            munmap(shm, sizeof(car_shared_mem));
            exit(1);
        }
        //Ensure not in a place where it is open in any means
        if (strcmp(shm->status, "Open") == 0 || strcmp(shm->status, "Opening") ==0 || strcmp(shm->status, "Closing") == 0) {
            pthread_mutex_unlock(&shm->mutex);
            printf("Operation not allowed while doors are open.\n");
            munmap(shm, sizeof(car_shared_mem));
            exit(1);
        }
        if (strcmp(shm->status, "Between") == 0)  {
            pthread_mutex_unlock(&shm->mutex);
            printf("Operation not allowed while elevator is moving.\n");
            munmap(shm, sizeof(car_shared_mem));
            exit(1);
        }
        //We have passed all our checks
        //Set the desitation to next floor up
        char next_floor[12];
        get_next_floor_down(shm->current_floor, next_floor, sizeof(next_floor));
        strcpy(shm->destination_floor, next_floor);
        
     } else {
        //Something else that we are not considering was inputted into the terminal
        pthread_mutex_unlock(&shm->mutex);
        printf("Invalid operation.\n");
        munmap(shm, sizeof(car_shared_mem));
        exit(1);
     }
     
     //Send a signal out
     pthread_cond_broadcast(&shm->cond);
     //Unlock the mutex as data does not need to be locekd down aynmore
     pthread_mutex_unlock(&shm->mutex);

     //Clean uo the memory 
     munmap(shm, sizeof(car_shared_mem));
     return 0;
}
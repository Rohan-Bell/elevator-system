/*
*   Safety System considerations
*
*   1. Using MISRA C Standards the use of printf() should not be done. These can cause buffering issues. Yet, because we need to printf as we are marking through the console we need to be explict about where
*   we printf. To mitigate this we will use fflush() to ensre an immediate output to the terminal
*   2. Use of exit(), the MISRA C standard discourages abrupt termination. The exit function should only be used in initialization failures where continued operation would be unsafe.
*   3. The main loop runs indefinitly using pthread_cond_wait(). This is because it should continiously supervise until external termination
*   4. All inputs must have a comprehensive validation, thus that all shared memory fields cannot be affected by bad inputs
*   5. As it is safety-critical any unknown events or unwanted events will trigger emergency mode.
*   6. No dynamic memory allocation to avoid heap-related failures
*   7. Minimising the use of external dependenices as this will reduce overhead but also ensure that the system is easy to read and understand
*   8. Have explicit safety checks with explicit error handling
    9. The use of atoi() according to MISRA C can lead to undefined behavour, yet as our input is pre-validated through character checking. This ensures we can use atoi() as it receives only valid numeric strings
*   
*   Race Condition Prevention:
*   As the system had multiple processes where the car, internal and safety system all accesses the shared memory concurrently. 
*   Because we don't any race conditions a couple methods are going to be put in place to stop this. 
*   1. All safety checks are done whilst holding the mutex ensuring that no one else can perform read/modify/write operations on shared memory fields
*   2. Will be using pthread_cond_wait() to avoid polling, this will ensure that there is no risk of any state changes between polling intervals
*    3. Each safety check will examine the complete state rather than individual fields across multiple lock acquisitions
*    4. Safety checks are performed in an order to prevent time-of-check-time-of-use vulnerabilities
*   
    Timing Considerations:
    1. The  safety system must respond immediatly to condition varaible signals. It must ensure minial latency between the detection and the response
    2. Priority inversion: Although this system isn't using RTOS,  I want to design the system so that it minimizes mutex gold time to reduce any blocking
    3. The safety checks must have bounded execution time that does not dynamically allocate or with unbounded loops to ensure deterministic behaviour

    Faults:
    1. Any detected unwanted changes to the system must trigger emergency mode. This is the safest possible sate when there is uncertainty in the system. 
    2. A heartbeat is used through safety_system heartbeat field for a simple watchdog to prevent saferty system failures or communication breakdowmns
    3. Data validation must be performed on all shared memory fields to detect corruption, buffer overflow or manipulation
    4. All systems must be redundant and independant thus ensuring that there is no single point of failure

    Input / Security Considerations
    1. all of the shared memory fields must be valdiated before use. This will prevent exploitation or mistakes for example, entering in the over 999 floors or nmore than one argument
    2. String operations are bounded functions that have checks for length to prevent any buffer overflow
    3. All numeric comparisons must check for a valid range to prevent wrap arounds
*
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include "shared_mem.h"

//Constants that are predefined for saferty critical values
#define SAFETY_SYSTEM_ACTIVE_VALUE 1U
#define BOOLEAN_TRUE_VALUE 1U
#define BOOLEAN_FALSE_VALE 0U
#define MAX_FLOOR_STRING_LENGTH 3U
#define MAX_STATUS_STRING_LENGTHJ 7U

/*Valid status strings for checking*/

static const char* const VALID_STATUSES[] = {
    "Opening",
    "Open",
    "Closing",
    "Closed",
    "Between"
};

#define NUM_VALID_STATUSES 5U

/* FUnction prototypes */

static int validate_floor_string(const char* floor);
static int validate_status_string(const char* status);
static int check_boolean_field(uint8_t field_value);
static void handle_safety_system_heartbeat(car_shared_mem* shm);
static void put_car_in_emergency_mode(car_shared_mem* shm);
static void handle_door_obstruction(car_shared_mem* shm);
static void handle_emergency_stop(car_shared_mem* shm);
static void handle_overload(car_shared_mem* shm);
static void handle_data_consistency_error(car_shared_mem* shm);
static int check_data_consistency(car_shared_mem* shm);

int main(int argc, char **argv){
    if (argc != 2) {
        fprintf(stderr, "Invalid Inputs");
        exit(1);
    }
    const char* car_name = argv[1];
    char shm_name[256]; //Build a shared memory object name
    int name_result = snprintf(shm_name, sizeof(shm_name), "/car%s", car_name);
    if (name_result < 0 || (size_t)name_result >= sizeof(shm_name)){ //force sizing and check if car nme exists
        printf("Unable to access car %s.\n", car_name);
        exit(1);
    }

    //Lets open up the shared memory]
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1){
        printf("Unable to access car %s.\n", car_name);
    }

    // Now map the shared memory and close the file descriptor
    car_shared_mem* shm = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        printf("Unable to access car %s.\n", car_name);
        close(fd);
        exit(1);
    }
    close(fd);

    //Now we have mapped the memory and everything is setup. Can now enter safety monitoring loop
    while(1) {
        pthread_mutex_lock(&shm->mutex); //lock down the mutex so can only access when we want,
        //Now we want to wait for a change in the shared memory
        pthread_cond_wait(&shm->cond, &shm->mutex); //Dooing this way elimintates risk of change between polling

        //handle a heartbeat check to ensure everything is all good
        handle_safety_system_heartbeat(shm);

        //Ensure that there is no door obstruction
        handle_door_obstruction(shm);
        
        //Check the e stop
        handle_emergency_stop(shm);

        //Check for any overload
        handle_overload(shm);

        //Check the data consistency is correct
        if(check_data_consistency(shm) == 0) {
            handle_data_consistency_error(shm); // did not return true handle errro
        }
        pthread_mutex_unlock(&shm->mutex); // All checks have passed unlock the mutex bbefore repeating
    }
    //The code should never reach here. But just in case unmap the memory and return
    munmap(shm, sizeof(car_shared_mem));

}

//Safety check functions below

static void handle_safety_system_heartbeat(car_shared_mem* shm) {
    if (shm-> safety_system != SAFETY_SYSTEM_ACTIVE_VALUE) {
            //Upadte the shared memory with the new value
        shm->safety_system = SAFETY_SYSTEM_ACTIVE_VALUE;
        
    }
}

static void handle_door_obstruction(car_shared_mem* shm) {
    if ((shm->door_obstruction == BOOLEAN_TRUE_VALUE) && (strcmp(shm->status, "Closing") == 0)) {
        strcpy(shm->status, "Opening"); // Something got stuck in door open the door
    }
}

static void handle_emergency_stop(car_shared_mem* shm) {
    if ((shm->emergency_stop == BOOLEAN_TRUE_VALUE) && (shm->emergency_mode == BOOLEAN_FALSE_VALE)) {
        //If e stop is hit and not already in e stop mode 
        printf("The emergency stop button has been pressed!\n");
        fflush(stdout);
        put_car_in_emergency_mode(shm);
        shm->emergency_stop = BOOLEAN_FALSE_VALE;
    }
}

static void handle_overload(car_shared_mem* shm) {
    if ((shm->overload == BOOLEAN_TRUE_VALUE) && (shm->emergency_mode == BOOLEAN_FALSE_VALE)) {
        printf("The overload sensor has been tripped!\n");
        fflush(stdout);
        put_car_in_emergency_mode(shm);
    }
}

static void handle_data_consistency_error(car_shared_mem* shm) {
    printf("Data consistency error!\n");
    fflush(stdout);
    put_car_in_emergency_mode(shm);
}

static void put_car_in_emergency_mode(car_shared_mem* shm) {
    shm->emergency_mode = BOOLEAN_TRUE_VALUE;
}

static int check_data_consistency(car_shared_mem* shm) {
    /* Skip the data check if in emergency mode as data cannot break anyhting*/
    if(shm->emergency_mode == BOOLEAN_TRUE_VALUE) return 1; //Check passed
    
    //Check floor strings
    if(validate_floor_string(shm->current_floor) == 0) return 0; //Failed
    if(validate_floor_string(shm->destination_floor) == 0) return 0; //Failed

    //check status stirng
    if(validate_status_string(shm->status) == 0) return 0; //failed

    //check the boolean fields for values >=- 2
    if(check_boolean_field(shm->open_button) == 0) return 0;
    if(check_boolean_field(shm->close_button) == 0) return 0;
    if(check_boolean_field(shm->door_obstruction) == 0) return 0;
    if(check_boolean_field(shm->overload) == 0) return 0;
    if(check_boolean_field(shm->emergency_stop) == 0) return 0;
    if(check_boolean_field(shm->individual_service_mode) == 0) return 0;
    if(check_boolean_field(shm->emergency_mode) == 0) return 0;

    //check the door obstructuon logic
    if(shm->door_obstruction == BOOLEAN_TRUE_VALUE) {
        if ((strcmp(shm->status, "Opening") != 0) && (strcmp(shm->status, "Closing") != 0)) return 0;
    }
    //All checks have passed return true
    return 1;
}

static int validate_floor_string(const char* floor) {
    if (floor == NULL) return 0; //needs to be a floor
    size_t len = strlen(floor);
    if(len == 0U || len > MAX_FLOOR_STRING_LENGTH) return 0; //Improper floor size.
    if(floor[0] == 'B') {
        //It is a basement floor 
        if(len < 2U) return 0; // check character sizing
        for (size_t i = 1U; i < len; i++) {
            if (floor[i] < '0' || floor[i] > '9') {
                return 0; // Is a cahracter and not a number
            }
        }
        int basement_num = atoi(floor +1); //safe to use as must be numbers
        return (basement_num >= 1 && basement_num <= 99) ? 1 : 0; //If 1 - 99 return true else return false

    } else {
        //regular floor
        for (size_t i = 0U; i < len; i++)
        {
            if (floor[i] < '0' || floor[i] > '9') return 0; //check if character
        }
        int floor_num = atoi(floor); // safe to use as must be numbers
        return (floor_num >= 1 && floor_num <= 999) ? 1 : 0; // Between 1 and 999
    }
}

static int validate_status_string(const char* status) {
    if (status == NULL) return 0;
    for (size_t i = 0U; i < NUM_VALID_STATUSES; i++) {
        if (strcmp(status, VALID_STATUSES[i]) == 0) {
            return 1; //The status is a valid satus
        }
    }
    return 0; //an invalid status
}

static int check_boolean_field(uint8_t field_value) {
    return (field_value < 2U) ? 1 : 0; //Is it true or false? 1 or 0?
}
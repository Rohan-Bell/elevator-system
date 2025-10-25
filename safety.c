/*
*   Safety System considerations
*
*   1. Using MISRA C Standards the use of printf() should not be done. These can cause buffering issues. Therefore write() will be used instead of printf
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
//Forcing the declaration as part of the POSIX.1-2000 standard for the use of strnlen 
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include "shared_mem.h"

//Constants that are predefined for saferty critical values
#define SAFETY_SYSTEM_ACTIVE_VALUE 1U
#define BOOLEAN_TRUE_VALUE 1U
#define BOOLEAN_FALSE_VALUE 0U
#define MAX_FLOOR_STRING_LENGTH 3U
#define MAX_STATUS_STRING_LENGTH 7U

#define EXPECTED_ARGC 2
#define SHM_NAME_BUFFER_SIZE 256
#define FILE_PERMISSIONS 438
#define STDOUT_FD 1
#define STDERR_FD 2
#define BASEMENT_MIN_LEVEL 1
#define BASEMENT_MAX_LEVEL 99
#define FLOOR_MIN_LEVEL 1
#define FLOOR_MAX_LEVEL 999

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
static void safe_write(int fd, const char *message); // This is to not use printf
static int construct_shm_name(char *dest, size_t dest_size, const char *car_name);

int main(int argc, const char * const *argv){
    if (argc != EXPECTED_ARGC) {
        safe_write(STDERR_FD, "Invalid Number of Args.\n");
        //Exit early, this is permissable as this is a critical init failure continuing would be unsafe
        exit(EXIT_FAILURE);
    }
    const char* car_name = argv[1];
    char shm_name[SHM_NAME_BUFFER_SIZE]; //Build a shared memory object name
    if (construct_shm_name(shm_name, sizeof(shm_name), car_name) != 0) {
        safe_write(STDERR_FD, "Error: Car name is too long or invalid.\n");
        exit(EXIT_FAILURE);
    }

    //Lets open up the shared memory
    int fd = shm_open(shm_name, O_RDWR, FILE_PERMISSIONS);
    if (fd == -1){
        safe_write(STDERR_FD, "Unable to open shared memory.\n");
        exit(EXIT_FAILURE); //Permissable as init failure again
    }

    // Now map the shared memory and close the file descriptor
    car_shared_mem* shm = mmap(NULL, sizeof(car_shared_mem), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        safe_write(STDERR_FD, "Unable to access car.\n");
        (void)close(fd); //Attempt to close but if failed it doesn't cahnge the exit status
        exit(EXIT_FAILURE);
    }
    (void)close(fd);

    //Now we have mapped the memory and everything is setup. Can now enter safety monitoring loop
    while(1) {
            /* Acquire the mutex and check return code. If lock fails, escalate to
               emergency mode and retry after a short sleep to avoid spinning. */
            int rc = pthread_mutex_lock(&shm->mutex);
            if (rc != 0) {
                safe_write(STDERR_FD, "Mutex lock failed in safety system.\n");
                put_car_in_emergency_mode(shm);
                /* Back off briefly to avoid tight loop */
                (void)sleep(1);
                continue;
            }

            /* Wait for a change in shared memory. Handle interrupts (EINTR) by
               retrying; any other error escalates to emergency mode. */
            int wait_rc = pthread_cond_wait(&shm->cond, &shm->mutex);
            while (wait_rc != 0) {
                if (wait_rc == EINTR) {
                    /* Interrupted by a signal, retry waiting */
                    wait_rc = pthread_cond_wait(&shm->cond, &shm->mutex);
                    continue;
                }
                safe_write(STDERR_FD, "Condition wait failed in safety system.\n");
                put_car_in_emergency_mode(shm);
                break;
            }

        //handle a heartbeat check to ensure everything is all good
        handle_safety_system_heartbeat(shm);

        //Ensure that there is no door obstruction
        handle_door_obstruction(shm);
        
        //Check the e stop
        handle_emergency_stop(shm);

        //Check for any overload
        handle_overload(shm);

        //Check the data consistency is correct
            if (check_data_consistency(shm) == 0) {
                handle_data_consistency_error(shm); /* did not return true -> handle error */
            }

            /* Unlock the mutex; ignore unlock return for compatibility with the
               rest of the code, but call is performed. */
            (void)pthread_mutex_unlock(&shm->mutex);
    }
    //The code should never reach here. But just in case unmap the memory and return
    (void)munmap(shm, sizeof(car_shared_mem));

}

//Safety check functions below

static void handle_safety_system_heartbeat(car_shared_mem* shm) {
    if (shm-> safety_system != SAFETY_SYSTEM_ACTIVE_VALUE) {
            //update the shared memory with the new value
        shm->safety_system = SAFETY_SYSTEM_ACTIVE_VALUE;
        
    }
}

static void handle_door_obstruction(car_shared_mem* shm) {
    if ((shm->door_obstruction == BOOLEAN_TRUE_VALUE) && (strcmp(shm->status, "Closing") == 0)) {
        /* Using strncpy for bounded string copy rule 21.6 */
        strncpy(shm->status, "Opening", sizeof(shm->status) - 1U); /* Something got stuck in door open the door */
        shm->status[sizeof(shm->status) - 1U] = '\0'; /* Null-termination is ensured by forcing it. */
    }
}

static void handle_emergency_stop(car_shared_mem* shm) {
    if ((shm->emergency_stop == BOOLEAN_TRUE_VALUE) && (shm->emergency_mode == BOOLEAN_FALSE_VALUE)) {
        //If e stop is hit and not already in e stop mode 
        safe_write(STDERR_FD,"The emergency stop button has been pressed!\n");
        put_car_in_emergency_mode(shm);
        shm->emergency_stop = BOOLEAN_FALSE_VALUE;
    }
}

static void handle_overload(car_shared_mem* shm) {
    if ((shm->overload == BOOLEAN_TRUE_VALUE) && (shm->emergency_mode == BOOLEAN_FALSE_VALUE)) {
        safe_write(STDERR_FD,"The overload sensor has been tripped!\n");
        put_car_in_emergency_mode(shm);
    }
}

static void handle_data_consistency_error(car_shared_mem* shm) {
    safe_write(STDERR_FD,"Data consistency error!\n");
    put_car_in_emergency_mode(shm);
}

static void put_car_in_emergency_mode(car_shared_mem* shm) {
    shm->emergency_mode = BOOLEAN_TRUE_VALUE;
}

static int check_data_consistency(car_shared_mem* shm) {
    int result = 1; /* Assume success */

    /* Skip the data check if in emergency mode as data cannot break anything*/
    if(shm->emergency_mode == BOOLEAN_TRUE_VALUE) {
        result = 1; /* Check passed */
    } else {
        /* Check floor strings */
        if(validate_floor_string(shm->current_floor) == 0) {
            result = 0; /* Failed */
        } else if(validate_floor_string(shm->destination_floor) == 0) {
            result = 0; /* Failed */
        } else if(validate_status_string(shm->status) == 0) {
            result = 0; /* Failed */
        } else if(check_boolean_field(shm->open_button) == 0) {
            result = 0;
        } else if(check_boolean_field(shm->close_button) == 0) {
            result = 0;
        } else if(check_boolean_field(shm->door_obstruction) == 0) {
            result = 0;
        } else if(check_boolean_field(shm->overload) == 0) {
            result = 0;
        } else if(check_boolean_field(shm->emergency_stop) == 0) {
            result = 0;
        } else if(check_boolean_field(shm->individual_service_mode) == 0) {
            result = 0;
        } else if(check_boolean_field(shm->emergency_mode) == 0) {
            result = 0;
        } else {
            /* Check the door obstruction logic */
            if(shm->door_obstruction == BOOLEAN_TRUE_VALUE) {
                if ((strcmp(shm->status, "Opening") != 0) && (strcmp(shm->status, "Closing") != 0)) {
                    result = 0;
                }
            }
        }
    }

    return result;
}

static int validate_floor_string(const char* floor) {
    int result = 0;

    if (floor == NULL) {
        result = 0;
    } else {
        size_t len = strnlen(floor, MAX_FLOOR_STRING_LENGTH + 2U); /* Check against a slightly larger value to detect overflow */
        if((len == 0U) || (len > MAX_FLOOR_STRING_LENGTH)) {
            result = 0; /* Improper floor size. */
        } else {
            long converted_num;
            char *endptr;
            if(floor[0] == 'B') {
                /* It is a basement floor */
                if(len < 2U) {
                    result = 0; /* check character sizing */
                } else {
                    errno = 0; /* Reset errno before the call */
                    converted_num = strtol(&floor[1], &endptr, 10);
                    /* Check for conversion errors (4.7) */
                    if ((endptr == &floor[1]) || (*endptr != '\0') || (errno == ERANGE)) {
                        result = 0; /* Not a valid number or out of range */
                    } else {
                        result = ((converted_num >= BASEMENT_MIN_LEVEL) && (converted_num <= BASEMENT_MAX_LEVEL)) ? 1 : 0;
                    }
                }
            } else {
                /* regular floor */
                errno = 0; /* Reset errno before the call */
                converted_num = strtol(floor, &endptr, 10);

                /* Check for conversion errors */
                if ((endptr == floor) || (*endptr != '\0') || (errno == ERANGE)) {
                    result = 0; /* Not a valid number or out of range */
                } else {
                    result = ((converted_num >= FLOOR_MIN_LEVEL) && (converted_num <= FLOOR_MAX_LEVEL)) ? 1 : 0;
                }
            }
        }
    }

    return result;
}

static int validate_status_string(const char* status) {
    int result = 0;

    if (status == NULL) {
        result = 0;
    } else {
        result = 0; /* Assume invalid */
        for (size_t i = 0U; i < NUM_VALID_STATUSES; i++) {
            if (strcmp(status, VALID_STATUSES[i]) == 0) {
                result = 1; /* The status is a valid status */
                break;
            }
        }
    }

    return result;
}

static int check_boolean_field(uint8_t field_value) {
    return (field_value < 2U) ? 1 : 0; //Is it true or false? 1 or 0?
}
static void safe_write(int fd, const char *message) {
    /* Capture the return value to satisfy MISRA rationale for checking
       functions that can fail; value intentionally discarded after assign. */
    ssize_t r = write(fd, message, strlen(message));
    (void)r;
}

/**
 * @brief Safely constructs the shared memory object name.
 *
 * This function is a MISRA-compliant replacement for snprintf for the specific
 * task of creating a name like "/car<name>". It performs explicit bounds
 * checking to prevent buffer overflows.
 *
 * @param dest The destination buffer to write the name into.
 * @param dest_size The total size of the destination buffer.
 * @param car_name The name of the car to append.
 * @return 0 on success, -1 on failure (e.g., buffer too small).
 */
static int construct_shm_name(char *dest, size_t dest_size, const char *car_name) {
    int result = 0;

    const char *prefix = "/car";
    size_t prefix_len = strlen(prefix);
    size_t car_name_len = strlen(car_name);

    /* Safety Check 1: Ensure the combined length fits in the buffer.
       We need space for the prefix, the name, and the null terminator ('\0'). */
    if ((prefix_len + car_name_len + 1) > dest_size) {
        result = -1; /* Failure: Buffer is too small. */
    } else {
        /* If checks pass, it is now safe to copy the strings. */
        (void)memcpy(dest, prefix, prefix_len);
        (void)memcpy(dest + prefix_len, car_name, car_name_len);

        /* Manually add the null terminator. */
        dest[prefix_len + car_name_len] = '\0';

        result = 0; /* Success */
    }

    return result;
}
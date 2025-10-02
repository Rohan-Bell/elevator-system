CC = gcc
#I am going to enable all common warnings, enable additional warnings, compile using the C99 standard and enable multithreading support with POSIX
CFLAGS = -Wall -Wextra -std=c99 -pthread
#Link realtime library, link POSIX thread library
LDFLAGS = -lrt -lpthread

#Object files
SHARED_OBJS = shared_utils.o
# Executables
TARGETS = car call_car internal_controls safety controller

#Create all 5 executables
all: $(TARGETS)

# Shared utilities
shared_utils.o: shared_utils.c shared.h shared_mem.h
	$(CC) $(CFLAGS) -c shared_utils.c -o shared_utils.o


#Builds "car" compiling car.c with given flags will output exect named car (same struct below just copied and pasted)

# Car component
car: car.o $(SHARED_OBJS)
	$(CC) $(CFLAGS) car.o $(SHARED_OBJS) -o car $(LDFLAGS)

car.o: car.c shared.h shared_mem.h
	$(CC) $(CFLAGS) -c car.c -o car.o

controller: controller.o $(SHARED_OBJS)
	$(CC) $(CFLAGS) controller.o $(SHARED_OBJS) -o controller $(LDFLAGS)

controller.o: controller.c shared.h shared_mem.h
	$(CC) $(CFLAGS) -c controller.c -o controller.o

call: call.o $(SHARED_OBJS)
	$(CC) $(CFLAGS) call.o $(SHARED_OBJS) -o call $(LDFLAGS)

call.o: call.c shared.h shared_mem.h
	$(CC) $(CFLAGS) -c call.c -o call.o


internal: internal.o $(SHARED_OBJS)
	$(CC) $(CFLAGS) internal.o $(SHARED_OBJS) -o internal $(LDFLAGS)

internal.o: internal.c shared.h shared_mem.h
	$(CC) $(CFLAGS) -c internal.c -o internal.o

safety: safety.o $(SHARED_OBJS)
	$(CC) $(CFLAGS) safety.o $(SHARED_OBJS) -o safety $(LDFLAGS)

safety.o: safety.c shared.h shared_mem.h
	$(CC) $(CFLAGS) -c safety.c -o safety.o


#I only think I would ened a basic clean, but this can be changed later if need be
clean:
	rm -f $(TARGETS) *.o
	rm -f /dev/shm/car*

#all and clean aren't files, don't want any cofnusion
.PHONY: all clean
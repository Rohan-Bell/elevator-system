CC = gcc
#I am going to enable all common warnings, enable additional warnings, compile using the C99 standard and enable multithreading support with POSIX
CFLAGS = -Wall -Wextra -std=c99 -pthread
#Link realtime library, link POSIX thread library
LDFLAGS = -lrt -lpthread

#Create all 5 executables
all: car controller call internal safety

#Builds "car" compiling car.c with given flags will output exect named car (same struct below just copied and pasted)
car: car.c
	$(CC) $(CFLAGS) -o car car.c $(LDFLAGS)

controller: controller.c
	$(CC) $(CFLAGS) -o controller controller.c $(LDFLAGS)

call: call.c
	$(CC) $(CFLAGS) -o call call.c $(LDFLAGS)

internal: internal.c
	$(CC) $(CFLAGS) -o internal internal.c $(LDFLAGS)

safety: safety.c
	$(CC) $(CFLAGS) -o safety safety.c $(LDFLAGS)

#I only think I would ened a basic clean, but this can be changed later if need be
clean:
	rm -f car controller call internal safety 

#all and clean aren't files, don't want any cofnusion
.PHONY: all clean
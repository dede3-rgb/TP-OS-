CC = gcc
CFLAGS = -Wall -Wextra -Werror -g -pthread
MLFLAGS = -Wall -Wextra -Werror -g -O0 -pthread

all: biceps

biceps: biceps.c gescom.c creme.c gescom.h creme.h
	$(CC) $(CFLAGS) -o biceps biceps.c gescom.c creme.c

memory-leak: biceps-memory-leaks

biceps-memory-leaks: biceps.c gescom.c creme.c gescom.h creme.h
	$(CC) $(MLFLAGS) -o biceps-memory-leaks biceps.c gescom.c creme.c

clean:
	rm -f biceps biceps-memory-leaks *.o
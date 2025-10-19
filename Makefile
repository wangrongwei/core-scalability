CFLAGS = -O3 -DNDEBUG -pthread
CC = gcc

icl: icl.c
	$(CC) $(CFLAGS) icl.c -o icl

clean:
	rm -f icl

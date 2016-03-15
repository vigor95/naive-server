CC=c99
CFLAGS = -Wall -o2

all: server

server: server.c
	$(CC) $(CFLAGS) -o start server.c $(LIB)

clean:
	rm -f *.o start *~

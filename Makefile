CC = gcc
CFLAGS = -Wall -pedantic -std=c11 -g
LDFLAGS = -pthread

all: server client

server: server.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: client.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f *.o server client

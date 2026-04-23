CC = g++
CFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -O2

server: server.cpp
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f server

.PHONY: clean

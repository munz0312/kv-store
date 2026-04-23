CXX      = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -O2

all: server client

server: server.o hashtable.o
	$(CXX) $(CXXFLAGS) -o $@ $^

client: 07_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

server.o: server.cpp hashtable.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

hashtable.o: hashtable.cpp hashtable.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f server client *.o

.PHONY: all clean

CXX = g++
CXXFLAGS = -g -Wall -std=c++20 -pthread

all: proxy

proxy: proxy_server_with_cache.cpp proxy_parse.cpp
	$(CXX) $(CXXFLAGS) -o proxy_parse.o -c proxy_parse.cpp
	$(CXX) $(CXXFLAGS) -o proxy.o -c proxy_server_with_cache.cpp
	$(CXX) $(CXXFLAGS) -o proxy proxy_parse.o proxy.o

clean:
	rm -f proxy *.o

.PHONY: all clean

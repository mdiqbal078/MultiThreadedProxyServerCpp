CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -pthread -O3

# Files
SRCS = proxy_server_with_cache.cpp proxy_parse.cpp
OBJS = $(SRCS:.cpp=.o)
HEADERS = buffer.hpp metrics.hpp lru_cache.hpp event_loop.hpp dns_resolver.hpp proxy_parse.hpp

TARGET = proxy

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# AddressSanitizer build for memory safety verification
asan: CXXFLAGS += -fsanitize=address -g -O1
asan: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean asan

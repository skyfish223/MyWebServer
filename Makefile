CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Isrc -pthread
LDFLAGS  = -pthread

TARGET = server

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp \
       src/EpollHelper.cpp \
       src/HttpConnection.cpp \
       src/ThreadPool.cpp

OBJS = $(SRCS:.cpp=.o)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
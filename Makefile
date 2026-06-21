CXX      = g++
CXXFLAGS = -std=c++17 -Wall -pthread -Isrc
LDFLAGS  = -lmysqlclient -pthread

SRCS = src/main.cpp \
       src/ServerConfig.cpp \
       src/Log.cpp \
       src/ThreadPool.cpp \
       src/HttpParser.cpp \
       src/HttpResponse.cpp \
       src/HttpHandler.cpp \
       src/HttpConnection.cpp \
       src/EpollHelper.cpp \
       src/SqlPool.cpp \
       src/FormParser.cpp

server: $(SRCS)
	$(CXX) $(CXXFLAGS) -o server $(SRCS) $(LDFLAGS)

clean:
	rm -f server

.PHONY: clean server
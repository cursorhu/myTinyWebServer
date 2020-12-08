CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

LIBS = -lpthread -lmysqlclient -L /usr/lib64/mysql
SRCS = $(wildcard *.cpp ./*/*.cpp)

server: $(SRCS)
	$(CXX) $(CXXFLAGS) -o server $^ $(LIBS)  

.PHONY: clean
clean:
	rm  -f server

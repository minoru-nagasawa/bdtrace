CC  = gcc
CXX = g++

# DEBUG=1 make for debug build
ifdef DEBUG
  CFLAGS   = -Wall -g -O0
  CXXFLAGS = -Wall -g -O0 -std=c++03 -Ivendor -Isrc
else
  CFLAGS   = -Wall -O2
  CXXFLAGS = -Wall -O2 -std=c++03 -Ivendor -Isrc
endif

LDFLAGS  = -lpthread -ldl

# Sources
COMMON_SRCS = src/common/log.cpp src/common/string_util.cpp
DB_SRCS     = src/db/database.cpp src/db/schema.cpp
TRACER_SRCS = src/tracer/ptrace_backend.cpp src/tracer/trace_session.cpp
SQLITE_SRC  = vendor/sqlite3.c

# Objects
COMMON_OBJS  = $(COMMON_SRCS:.cpp=.o)
DB_OBJS      = $(DB_SRCS:.cpp=.o)
TRACER_OBJS  = $(TRACER_SRCS:.cpp=.o)
SQLITE_OBJ   = vendor/sqlite3.o

ALL_OBJS = $(COMMON_OBJS) $(DB_OBJS) $(TRACER_OBJS) $(SQLITE_OBJ)

# Targets
.PHONY: all clean test fetch_sqlite

all: bdtrace bdview

bdtrace: src/bdtrace_main.o $(ALL_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

bdview: src/bdview_main.o $(COMMON_OBJS) $(DB_OBJS) $(SQLITE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# SQLite compiled as C
vendor/sqlite3.o: vendor/sqlite3.c
	$(CC) $(CFLAGS) -DSQLITE_THREADSAFE=0 -c -o $@ $<

# Generic rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Tests
TEST_LIBS = $(COMMON_OBJS) $(DB_OBJS) $(SQLITE_OBJ)
TEST_SRCS = tests/test_main.cpp tests/test_database.cpp tests/test_trace_simple.cpp \
            tests/test_trace_fork.cpp tests/test_trace_file_io.cpp tests/test_rebuild.cpp \
            tests/test_transparency.cpp
TEST_OBJS = $(TEST_SRCS:.cpp=.o)

test_all: $(TEST_OBJS) $(TEST_LIBS) $(TRACER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test: test_all
	./test_all

fetch_sqlite:
	bash scripts/fetch_sqlite.sh

clean:
	rm -f bdtrace bdview test_all
	find . -name '*.o' -delete
	rm -f *.db

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
HELPER_SRCS = src/common/view_helpers.cpp
DB_SRCS     = src/db/database.cpp src/db/schema.cpp
TRACER_SRCS = src/tracer/ptrace_backend.cpp src/tracer/trace_session.cpp
SQLITE_SRC  = vendor/sqlite3.c

# Objects
COMMON_OBJS  = $(COMMON_SRCS:.cpp=.o)
HELPER_OBJS  = $(HELPER_SRCS:.cpp=.o)
DB_OBJS      = $(DB_SRCS:.cpp=.o)
TRACER_OBJS  = $(TRACER_SRCS:.cpp=.o)
SQLITE_OBJ   = vendor/sqlite3.o
MONGOOSE_OBJ = vendor/mongoose.o

ALL_OBJS = $(COMMON_OBJS) $(DB_OBJS) $(TRACER_OBJS) $(SQLITE_OBJ)

# Targets
.PHONY: all clean test fetch_sqlite fetch_mongoose embed_static

all: bdtrace bdview bdview-web

bdtrace: src/bdtrace_main.o $(ALL_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

bdview: src/bdview_main.o $(COMMON_OBJS) $(HELPER_OBJS) $(DB_OBJS) $(SQLITE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

bdview-web: src/web/bdview_web.o $(COMMON_OBJS) $(HELPER_OBJS) $(DB_OBJS) $(SQLITE_OBJ) $(MONGOOSE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# SQLite compiled as C
vendor/sqlite3.o: vendor/sqlite3.c
	$(CC) $(CFLAGS) -DSQLITE_THREADSAFE=0 -c -o $@ $<

# Mongoose compiled as C
vendor/mongoose.o: vendor/mongoose.c
	$(CC) $(CFLAGS) -DMG_ENABLE_LINES=0 -c -o $@ $<

# Static asset embedding
src/web/static_assets.h: static/index.html static/app.js static/app.css static/timeline.js
	bash scripts/embed_static.sh

src/web/bdview_web.o: src/web/bdview_web.cpp src/web/static_assets.h

# Generic rules
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Tests
TEST_LIBS = $(COMMON_OBJS) $(HELPER_OBJS) $(DB_OBJS) $(SQLITE_OBJ)
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

fetch_mongoose:
	bash scripts/fetch_mongoose.sh

embed_static: src/web/static_assets.h

clean:
	rm -f bdtrace bdview bdview-web test_all
	find . -name '*.o' -delete
	rm -f *.db

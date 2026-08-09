EXEC := demd
CXX ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=

# Resolve dependencies through their own config tools rather than hard-coding
# include paths, which differ between distributions and between GDAL 2 and 3.
GDAL_CFLAGS := $(shell gdal-config --cflags 2>/dev/null)
GDAL_LIBS := $(shell gdal-config --libs 2>/dev/null || echo -lgdal)
DEP_CFLAGS := $(shell pkg-config --cflags libevent json-c 2>/dev/null)
DEP_LIBS := $(shell pkg-config --libs libevent json-c 2>/dev/null || echo -levent -ljson-c)

INCLUDES := $(GDAL_CFLAGS) $(DEP_CFLAGS)
LIBS ?= $(GDAL_LIBS) $(DEP_LIBS)

SRCS := $(wildcard *.cpp)
# Objs are all the sources, with .cpp replaced by .o
OBJS := $(SRCS:.cpp=.o)
DEPS := $(SRCS:.cpp=.d)

DEM := dem
HGT := $(DEM)/N23E120.hgt

PORT ?= 8082
STRESS_ARG ?= -c 10

all: $(EXEC)

dem: $(HGT)

$(HGT):
	$(MAKE) -C $(DEM)

$(EXEC): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<

serve: $(EXEC) $(HGT)
	./$(EXEC) -p $(PORT) $(DEM)

profile: $(EXEC) $(HGT)
	@which valgrind || sudo apt-get install valgrind
	valgrind --leak-check=full ./$(EXEC) -p $(PORT) $(DEM)

stress:
	cd stress && go run . $(STRESS_ARG)

query:
	curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:$(PORT)/v1/elevations

# End-to-end tests against a freshly built binary. See test/README.md.
test: $(EXEC)
	python3 test/run.py ./$(EXEC)

# Same tests under AddressSanitizer/UBSan, which is what actually catches the
# memory errors -- plain -Wall -Wextra does not.
test/sanitize:
	$(MAKE) clean
	$(MAKE) $(EXEC) CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 python3 test/run.py ./$(EXEC)
	$(MAKE) clean

clean:
	@rm -f $(EXEC) $(OBJS) $(DEPS)

-include $(DEPS)

.PHONY: all dem serve profile stress query test test/sanitize clean

include docker.mk

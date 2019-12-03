EXEC := demd
CC := g++
LDFLAGS ?=
LIBS ?= -lgdal -levent -ljson-c
SRCS := $(wildcard *.cpp)
# Objs are all the sources, with .cpp replaced by .o
OBJS := $(SRCS:.cpp=.o)

DEM := dem
HGT := $(DEM)/N23E120.hgt

STRESS_ARG ?=

all: $(EXEC)

dem: $(HGT)

$(HGT):
	make -C $(DEM)

$(EXEC): $(OBJS)
	$(CC) -o $@ $(strip $(CFLAGS) )$^ $(strip $(LDFLAGS) $(LIBS))

%.o: %.cpp
	$(CC) -o $@ $(strip $(CFLAGS) $(INCLUDES) )-c $<

run: $(EXEC) $(HGT)
	./$(EXEC) -p 8082 $(DEM)

profile: $(EXEC) $(HGT)
	@which valgrind || sudo apt-get install valgrind
	valgrind --leak-check=full ./$(EXEC) -p 8082 $(DEM)

stress:
	cd stress; go run . $(STRESS_ARG)

query:
	curl -XPOST --data '[[120.957283,23.47]]' http://127.0.0.1:8082/v1/elevations

clean:
	@rm -f $(EXEC) $(OBJS)

.PHONY: all dem clean run profile stress query

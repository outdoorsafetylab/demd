EXEC := demd
CC := g++
LDFLAGS ?=
LIBS ?= -lgdal -levent -ljson-c
SOURCES := $(wildcard *.cpp)
# Objs are all the sources, with .cpp replaced by .o
OBJS := $(SOURCES:.cpp=.o)

DEM := dem
HGT := $(DEM)/N23E120.hgt

all: $(EXEC)

dem: $(HGT)

$(HGT):
	make -C $(DEM)

$(EXEC): $(OBJS)
	$(CC) $(strip $(CFLAGS) )$^ -o $@ $(strip $(LDFLAGS) $(LIBS))

%.o: %.cpp
	$(CC) $(strip $(CFLAGS) $(INCLUDES) )-c $< -o $@

run: $(EXEC) $(HGT)
	./$(EXEC) -p 8082 $(DEM)

profile: $(EXEC) $(HGT)
	@which valgrind || sudo apt-get install valgrind
	valgrind ./$(EXEC) -p 8082 $(DEM)

clean:
	@rm -f $(EXEC) $(OBJS)

.PHONY: all dem clean run

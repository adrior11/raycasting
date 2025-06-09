# Name of the compiler
CC := gcc

# Pull in SDL2 include- and link-flags
CFLAGS  := $(shell sdl2-config --cflags)
LDFLAGS := $(shell sdl2-config --libs)

# Your source and target executable
SRC     := main.c
OBJ     := $(SRC:.c=.o)
TARGET  := sdl_app

.PHONY: all clean run

# Default target: build the executable
all: $(TARGET)

# Link step
$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) -Wl, -w

# Compile step
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Handy shortcut to build & run
run: all
	./$(TARGET)

# Clean out generated files
clean:
	rm -f $(OBJ) $(TARGET)

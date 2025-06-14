CC := gcc

# SDL2 core
CFLAGS  := $(shell sdl2-config --cflags)
LDFLAGS := $(shell sdl2-config --libs)

# SDL2_image
CFLAGS += $(shell pkg-config --cflags SDL2_image)
LDFLAGS += $(shell pkg-config --libs SDL2_image)

SRC     := main.c
OBJ     := $(SRC:.c=.o)
TARGET  := raycasting

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) -Wl, -w

# Compile step
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: all
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

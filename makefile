.PHONY: all clean

# List of source files and corresponding object files
SRCS := main.c
OBJS := $(SRCS:.c=.o)

# Compiler and flags
CC := gcc
CFLAGS := -Wall -Wextra -std=c99 -pthread

# Default target
all: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o my_program

# Target to perform git pull

# Compile C source files
%.o: %.c %.h
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f $(OBJS) my_program
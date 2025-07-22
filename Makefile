# Compiler and flags
CC      := gcc
CFLAGS  := -Wall -Wextra -g -std=c11 -Iinclude -pthread

# Libraries
LDLIBS  := -lcurl -pthread

# Directories
SRCDIR  := src
INCDIR  := include

# Program name
TARGET  := findpng2

# Only the source your crawler needs
SOURCES := findpng2.c

# Object files go in the same tree under SRCDIR
OBJS    := $(patsubst %.c,$(SRCDIR)/%.o,$(SOURCES))

.PHONY: all clean

all: $(TARGET)

# Link step
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Compile step
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	# delete every .o, the executable, and all .txt files
	find . -type f -name '*.o' -delete
	rm -f $(TARGET)
	find . -type f -name '*.txt' -delete
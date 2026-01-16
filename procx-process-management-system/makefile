CC := gcc
CFLAGS := -Wall -Wextra -pthread
TARGET := procx

all: $(TARGET)

$(TARGET): procx.c
	$(CC) $(CFLAGS) procx.c -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude

TARGET = client
SRCS = src/client.c

all: $(TARGET)

$(TARGET): $(SRCS) include/sbcp.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET) *.o
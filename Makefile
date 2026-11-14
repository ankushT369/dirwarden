CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -O2
TARGET = dirwarden
SRC = dirwarden.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)


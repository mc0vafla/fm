CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags gtk+-3.0 gio-2.0)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0 gio-2.0)

TARGET = fm
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) -g $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install

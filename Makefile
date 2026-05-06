CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -D_GNU_SOURCE
LDFLAGS = -lpthread -ldl -lm

# NDI headers — adjust if installed elsewhere
NDI_INCLUDE ?= /usr/local/include
CFLAGS += -I$(NDI_INCLUDE)

TARGET = ndi-led-cli
SRCS = main.c config.c ndi_receiver.c colorlight_output.c frame_convert.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean install

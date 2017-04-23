


OBJS := xkeys_mosquitto.o xkeys.o xkeys_udev.o
CFLAGS := -Ivendor/libuv/include -O0 -ggdb
LDFLAGS := -lpthread -lmosquitto -ludev

xkeys: $(OBJS)
	libtool --tag=CC --mode=link $(CC) -o $@ -static vendor/libuv/libuv.la $(LDFLAGS) $(OBJS)

.PHONY: clean
clean:
	rm xkeys $(OBJS)

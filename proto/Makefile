# The NV12 prototype: make, then ./fanvid-proto <file> [output...]
PKGS     = libavformat libavcodec libavfilter libavutil libdrm wayland-client
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Igen $(shell pkg-config --cflags $(PKGS))
LDLIBS   = $(shell pkg-config --libs $(PKGS)) -lm

WL_PROTODIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WL_PROTOCOLS = \
    $(WL_PROTODIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml \
    $(WL_PROTODIR)/stable/viewporter/viewporter.xml \
    $(WL_PROTODIR)/stable/xdg-shell/xdg-shell.xml \
    $(WL_PROTODIR)/staging/color-representation/color-representation-v1.xml \
    ../protocols/wlr-layer-shell-unstable-v1.xml
WL_GEN_H = $(patsubst %.xml,gen/%-client-protocol.h,$(notdir $(WL_PROTOCOLS)))
WL_GEN_O = $(patsubst %.xml,gen/%-protocol.o,$(notdir $(WL_PROTOCOLS)))
vpath %.xml $(sort $(dir $(WL_PROTOCOLS)))

fanvid-proto: fanvid-proto.o $(WL_GEN_O)
	$(CC) -o $@ $^ $(LDLIBS)

fanvid-proto.o: fanvid-proto.c $(WL_GEN_H)

gen/%-client-protocol.h: %.xml
	@mkdir -p gen
	wayland-scanner client-header $< $@

gen/%-protocol.c: %.xml
	@mkdir -p gen
	wayland-scanner private-code $< $@

gen/%-protocol.o: gen/%-protocol.c
	$(CC) -c $(CFLAGS) -o $@ $<

clean:
	rm -rf gen fanvid-proto fanvid-proto.o

.PHONY: clean
.PRECIOUS: gen/%-protocol.c

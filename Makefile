CLANG ?= clang
ARCH := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

BPF_CFLAGS = -O2 -target bpf -D__TARGET_ARCH_$(ARCH)
BPF_CFLAGS += -Wall -Wno-unused-value -Wno-pointer-sign
BPF_CFLAGS += -Wno-compare-distinct-pointer-types
BPF_CFLAGS += -Wno-gnu-variable-sized-type-not-at-end
BPF_CFLAGS += -Wno-address-of-packed-member -Wno-tautological-compare
BPF_CFLAGS += -Wno-unknown-warning-option -Wno-unused-variable
BPF_CFLAGS += -I/usr/include/$(shell uname -m)-linux-gnu

all: ipv6_nat.o loader

ipv6_nat.o: ipv6_nat.c
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

loader: loader.c
	gcc -o loader loader.c -lbpf

clean:
	rm -f *.o loader

install: all
	sudo ./loader

.PHONY: all clean install

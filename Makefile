CFLAGS=-march=native -O2 -DDEBUG=0
WFLAGS=-Wall -Wextra

INCS=$(shell pkg-config --cflags x11)
LIBS=-ldl $(shell pkg-config --libs x11)

COMPILE=$(CC) -fPIC -shared $(INCS) $(LIBS) $(WFLAGS) $(CFLAGS)

all: sssp_32.so sssp_64.so

sssp_32.so: sssp.c
	$(COMPILE) -m32 $< -o $@

sssp_64.so: sssp.c
	$(COMPILE) -m64 $< -o $@

test: test.c
	$(CC) -m32 -L. -lsteam_api $(WFLAGS) $(CFLAGS) $< -o $@

clean:
	rm sssp_??.so

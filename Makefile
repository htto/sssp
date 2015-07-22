CFLAGS=-march=native -O2 -DDEBUG=2 -ggdb
WFLAGS=-Wall -Wextra

INCS=$(shell pkg-config --cflags x11 xcomposite xdamage xfixes xrender) -Icontrib/include
LIBS=-ldl $(shell pkg-config --libs x11 xcomposite xdamage xfixes xrender) -lrt

COMPILE=$(CC) -fPIC -shared $(INCS) $(LIBS) $(WFLAGS) $(CFLAGS)

all: sssp_32.so sssp_64.so

sssp_32.so: sssp.c
	$(COMPILE) -m32 $< -o $@

sssp_64.so: sssp.c
	$(COMPILE) -m64 $< -o $@

test: test.c sssp_32.so
	$(CC) -m32 -Lcontrib/lib32 -lsteam_api $(WFLAGS) $(CFLAGS) $< -o $@

test_simple: test
	env LD_LIBRARY_PATH=contrib/lib32/ LD_PRELOAD=./sssp_32.so ./test

test_glxgears: all
	env LD_LIBRARY_PATH=contrib/lib64/ LD_PRELOAD=./sssp_64.so glxgears -geometry 1000x700

test_xterm: all
	env LD_LIBRARY_PATH=contrib/lib64/ LD_PRELOAD=./sssp_64.so xterm

clean:
	rm -f sssp_??.so test

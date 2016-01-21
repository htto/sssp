CFLAGS=-march=native -Og -ggdb
DEFINES=-DDFLT_LOG_LEVEL=2 -D_GNU_SOURCE
SHFLAGS=-fPIC -shared
WFLAGS=-Wall -Wextra


SYSTEM_LIBS=-ldl -lrt
X11_LIBS=x11 xcomposite xdamage xfixes xrender


INCS=$(shell pkg-config --cflags $(X11_LIBS)) -Icontrib/include
LIBS=$(shell pkg-config --libs $(X11_LIBS)) $(SYSTEM_LIBS)

HDRS=src/sssp.h
SRCS=src/misc.c src/sssp.c

COMPILE_FLAGS=$(SHFLAGS) $(DEFINES) $(INCS) $(LIBS) $(WFLAGS) $(CFLAGS)

all: sssp_32.so sssp_64.so

sssp_32.so: $(HDRS) $(SRCS)
	$(CC) -m32 $^ -o $@ $(COMPILE_FLAGS)

sssp_64.so: $(HDRS) $(SRCS)
	$(CC) -m64 $^ -o $@ $(COMPILE_FLAGS)

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

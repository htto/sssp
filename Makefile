ARCH=$(shell uname -m)
CFLAGS=-march=native -Og -ggdb
DEFINES=-DDFLT_LOG_LEVEL=3 -D_GNU_SOURCE
SHFLAGS=-fPIC -shared
WFLAGS=-Wall -Wextra

SYSTEM_LIBS=-ldl -lrt
X11_LIBS=x11 xcomposite xdamage xfixes xrender

# Multilib/-arch specifics
ifeq ($(ARCH),x86_64)
	ARCH=64
else
	ARCH=32
endif
A32_CONTRIB=contrib/lib32
A32_FLAGS=-m32
A32_TARGET=sssp_32.so
A64_CONTRIB=contrib/lib64
A64_FLAGS=-m64
A64_TARGET=sssp_64.so


INCS=$(shell pkg-config --cflags $(X11_LIBS)) -Icontrib/include
LIBS=$(shell pkg-config --libs $(X11_LIBS)) $(SYSTEM_LIBS)

HDRS=src/sssp.h
SRCS=src/misc.c src/sssp.c

COMPILE_FLAGS=$(SHFLAGS) $(DEFINES) $(INCS) $(LIBS) $(WFLAGS) $(CFLAGS)

all: $(A32_TARGET) $(A64_TARGET)

$(A32_TARGET): $(HDRS) $(SRCS)
	$(CC) $(A32_FLAGS) $^ -o $@ $(COMPILE_FLAGS)

$(A64_TARGET): $(HDRS) $(SRCS)
	$(CC) $(A64_FLAGS) $^ -o $@ $(COMPILE_FLAGS)

test: test.c $(A$(ARCH)_TARGET)
	$(CC) $(A$(ARCH)_FLAGS) $^ -o $@ $(WFLAGS) $(CFLAGS) -L$(A$(ARCH)_CONTRIB) -lsteam_api 

test_simple: test
	env LD_LIBRARY_PATH=$(A$(ARCH)_CONTRIB) LD_PRELOAD=./$(A$(ARCH)_TARGET) ./test

test_glxgears: $(A$(ARCH)_TARGET)
	env LD_LIBRARY_PATH=$(A$(ARCH)_CONTRIB) LD_PRELOAD=./$(A$(ARCH)_TARGET) glxgears -geometry 1000x700

test_xterm: $(A$(ARCH)_TARGET)
	env LD_LIBRARY_PATH=$(A$(ARCH)_CONTRIB) LD_PRELOAD=./$(A$(ARCH)_TARGET) xterm

clean:
	rm -f sssp_??.so test

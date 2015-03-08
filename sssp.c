/**
 *
 * A small LD_PRELOAD library for steam games, that captures a game screenshot
 * when the hotkey (XK_F12) is hit.
 *
 * gcc -m32 -o sssp_32.so sssp.c -shared -fPIC `pkg-config --cflags --libs x11`
 * gcc -m64 -o sssp_64.so sssp.c -shared -fPIC `pkg-config --cflags --libs x11`
 *
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define UNUSED __attribute__((unused))

/**
 *
 * Steam SDK info
 *   (c) Valve
 *
 */
/* The lowest user interface version I've encountered so far. */
#define STEAMUSER_INTERFACE_VERSION "SteamUser014"
/* The utils interface version I've encountered so far. */
#define STEAMUTILS_INTERFACE_VERSION "SteamUtils006"
/* The screenshot interface version. */
#define STEAMSCREENSHOTS_INTERFACE_VERSION "STEAMSCREENSHOTS_INTERFACE_VERSION002"

/* SteamID is same as in SDK and 64 bits total */
typedef union
{
    struct SteamIDComponent_t
    {
	uint32_t    accountID : 32;	    /* unique account identifier */
	uint32_t    accountInstance : 20;   /* dynamic instance ID */
	uint8_t	    eAccountType : 4;	    /* type of account - can't show as EAccountType, due to signed / unsigned difference */
	uint8_t	    eUniverse : 8;	    /* universe this account belongs to */
    } asComponent;

    uint64_t as64Bit;
} SteamID;

/* Basically the all-pure-virtual ISteam* handle is the virtual table. */

typedef struct
{
    struct {
	/* pure RGB (8bit per chan) data, data size (3 * w * h), image width, image height */
	uint32_t (*WriteScreenshot)(void *thiz, void *pubRGB, uint32_t cubRGB, int w, int h);
	/* ... */
    } *vtab;
} ISteamScreenshots;

typedef struct
{
    struct {
	void *funcs1[2];
	SteamID (*GetSteamID)(void *thiz);
	/* ... */
    } *vtab;
} ISteamUser;

typedef struct
{
    struct {
	void *funcs1[9];
	uint32_t (*GetAppID)(void *thiz);
	/* ... */
    } *vtab;
} ISteamUtils;

typedef struct
{
    struct {
	void *funcs1[5];
	ISteamUser *(*GetISteamUser)(void *thiz, int32_t user, int32_t pipe, const char *);
	void *funcs2[3];
	ISteamUtils *(*GetISteamUtils)(void *thiz, int32_t pipe, const char *);
	void *funcs3[8];
	ISteamScreenshots *(*GetISteamScreenshots)(void *thiz, int32_t user, int32_t pipe, const char *);
	/* ... */
    } *vtab;
} ISteamClient;

extern ISteamClient *SteamClient(void);
extern int32_t SteamAPI_GetHSteamPipe(void);
extern int32_t SteamAPI_GetHSteamUser(void);
extern const char *SteamAPI_GetSteamInstallPath(void);

/**
 *
 *  Initialization and hooking stuff.
 *
 */

typedef int (*hookFunc)(void);
typedef int (*hookPFunc)(void *, ...);
hookFunc _realSteamAPI_Init;
hookFunc _realSteamAPI_InitSafe;
hookPFunc _realXEventsQueued;
hookPFunc _realXLookupString;
hookPFunc _realXPending;

static SteamID steamUserID;
static uint32_t steamAppID;
static Bool steamInitialized = False;
static ISteamScreenshots *iscrshot = NULL;

static void *
findHook(const char *mod, const char *name)
{
    void *m = mod ? dlopen(mod, RTLD_NOW) : RTLD_NEXT;
    void * h = dlsym(m, name);

    if (!h)
	fprintf(stderr, "Unable to hook %s!\n", name);

    return h;
}

/* Initialization */
__attribute__((constructor)) void
init(void)
{
    _realXEventsQueued = (hookPFunc)findHook(NULL, "XEventsQueued");
    _realXLookupString = (hookPFunc)findHook(NULL, "XLookupString");
    _realXPending = (hookPFunc)findHook(NULL, "XPending");
    /* We need symbols from libsteam_api, so require it to be loaded. */
    _realSteamAPI_Init = (hookFunc)findHook("libsteam_api.so", "SteamAPI_Init");
    _realSteamAPI_InitSafe = (hookFunc)findHook("libsteam_api.so", "SteamAPI_InitSafe");

    if (!(_realXEventsQueued && _realXLookupString && _realXPending &&
	    _realSteamAPI_Init && _realSteamAPI_InitSafe))
    {
	fprintf(stderr, "ERROR: Unable to set up hooks. Won't work this way. "
			"Please disable this module from being LD_PRELOAD'ed.\n");
	/* TODO don't fail? */
	exit(1);
    }
    fprintf(stderr, "sssp_xy.so initialized.\n");
}

/* Finalization */
__attribute__((destructor)) static void
deinit(void)
{
    /* TODO */
    fprintf(stderr, "sssp_xy.so being unloaded from program '%s' (%s).\n",
		    program_invocation_short_name, program_invocation_name);
}

/**
 *
 *  Screenshot handling.
 *
 */

static uint8_t
mask32To8(uint32_t val, uint32_t mask)
{
    switch (mask)
    {
	case 0xFF:
	    return (val & mask);
	case 0xFF00:
	    return (val & mask) >> 8;
	case 0xFF0000:
	    return (val & mask) >> 16;
	case 0xFF000000:
	    return (val & mask) >> 24;
    }
    return 0;
}

/* Acquire/write Screenshot */
static void *
captureScreenShot(Display *dpy, Window win, int *w, int *h)
{
    XImage *image;
    XWindowAttributes attrs;
    uint8_t *data;
    int i;

    /* XGrabServer(dpy); */
    if (XGetWindowAttributes(dpy, win, &attrs) == 0 ||
	    (image = XGetImage(dpy, win, 0, 0, attrs.width, attrs.height,
				AllPlanes, ZPixmap)) == NULL)
    {
	/* XUngrabServer(dpy); */
	fprintf(stderr, "failed to acquire window attributes/screenshot!");
	return NULL;
    }
    /* XUngrabServer(dpy); */

    /* Convert to plain RGB as required by steam. */
    *h = attrs.height;
    *w = attrs.width;
    data = (uint8_t *)malloc(3 * *w * *h);

    /* TrueColor (which we assume) has got 4 bytes per pixel. */
    /* TODO assert depth */
    for (i = 0; i < *w * *h; i++)
    {
	uint32_t p = *(uint32_t *)(image->data + 4 * i);
	data[3 * i + 0] = mask32To8(p, image->red_mask);
	data[3 * i + 1] = mask32To8(p, image->green_mask);
	data[3 * i + 2] = mask32To8(p, image->blue_mask);
    }
    
    XDestroyImage(image);

    return (void *)data;
}

static void
handleScreenShot(Display *dpy, Window win)
{
    int w, h;
#if 0
    /* Hand made fallback disabled in favour of Steam's screenshot writer. */
    static uint32_t count = 0;
    char path[512];
    char date[11];
    const time_t t = time(NULL);
    struct tm lt;
    
    localtime_r(&t, &lt);
    strftime(date, sizeof(date), "%F", &lt);
    count++;

    /* STEAM_DIR/userdata/114244302/760/remote/49520/screenshots/2015-02-24_00002.jpg */
    snprintf(path, sizeof(path), "%s/userdata/%d/760/remote/%d/screenshots/%s_%05d.jpg",
	SteamAPI_GetSteamInstallPath(), steamUserID.asComponent.accountID,
	steamAppID, date, count);
#ifdef DEBUG
    fprintf(stderr, "screenshot file: '%s'\n", path);
#endif

    /* TODO mkdir -p */
#endif

    /* Image grabbed through X11 and converted to RGB */
    void *image = captureScreenShot(dpy, win, &w, &h);
    /* Issue the RGB image directly to steam. */
    if (image && !iscrshot->vtab->WriteScreenshot(iscrshot, image, 3 * w * h, w, h))
    {
	fprintf(stderr, "Failed to issue screenshot to steam.\n");
    }
    free(image);
}

/* Filter XEvent */
static Bool
filter(Display *dpy UNUSED, XEvent *event, XPointer arg UNUSED)
{
    if (event->type == KeyPress /*|| event->type == KeyRelease*/)
    {
	XKeyEvent *ke = (XKeyEvent *)event;
	if (!ke->send_event && !ke->state /* modifiers */)
	{
	    KeySym keysym = XLookupKeysym(ke, 0);
	    if (keysym == XK_F12)
	    {
		static Time t = 0;
		/* Let's not run havoc on too many events (KeyRepeat?). Allow every 50ms */
		if (ke->time - t > 50)
		{
		    t = ke->time;
#ifdef DEBUG
		    fprintf(stderr, "keysym: 0x%lx\n", keysym);
#endif
		    return True;
		}
#ifdef DEBUG
		fprintf(stderr, "keysym skipped: 0x%lx\n", keysym);
#endif
	    }
	}

    }
    return False;
}

static void
handleRequest(Display *dpy)
{
    XEvent e;

    if (!steamInitialized)
	return;

    /* TODO reduce eventqueue search */
    if (XCheckIfEvent(dpy, &e, filter, NULL))
    {
	handleScreenShot(dpy, ((XAnyEvent*)&e)->window);
    }
}

/* Grab the various handles from the interfaces. Actually userid and appid aren't needed. */
static void
steamSetup(void)
{
    int32_t hsp, hsu;
    ISteamClient *sc;
    ISteamUser *su;
    ISteamUtils *sut;

    sc = SteamClient();

    if (!sc)
    {
	fprintf(stderr, "ERROR: SteamClient is NULL!\n");
	return;
    }

    hsp = SteamAPI_GetHSteamPipe();
    hsu = SteamAPI_GetHSteamUser();
#ifdef DEBUG
    fprintf(stderr, "client: %p\n", sc);
    fprintf(stderr, "hpipe: %d\n", hsp);
    fprintf(stderr, "huser: %d\n", hsu);
#endif

    su = sc->vtab->GetISteamUser(sc, hsu, hsp, STEAMUSER_INTERFACE_VERSION);
    if (!su)
    {
	fprintf(stderr, "ERROR: SteamUser is NULL! Check interface version SteamUser0xy in libsteam_api.so.\n");
	return;
    }

    steamUserID = su->vtab->GetSteamID(su);
#ifdef DEBUG
    fprintf(stderr, "id: %llu %u\n", steamUserID.as64Bit, steamUserID.asComponent.accountID);
    fflush(stderr);
#endif

    sut = sc->vtab->GetISteamUtils(sc, hsp, STEAMUTILS_INTERFACE_VERSION);
    if (!sut)
    {
	fprintf(stderr, "ERROR: SteamUtils is NULL! Check interface version SteamUtils0xy in libsteam_api.so.\n");
	return;
    }
    steamAppID = sut->vtab->GetAppID(sut);
#ifdef DEBUG
    fprintf(stderr, "aid: %u\n", steamAppID);
    fflush(stderr);
#endif

    iscrshot = sc->vtab->GetISteamScreenshots(sc, hsu, hsp, STEAMSCREENSHOTS_INTERFACE_VERSION);
    if (!iscrshot)
    {
	fprintf(stderr, "ERROR: SteamScreenshots is NULL! "
			"Check interface version STEAMSCREENSHOTS_INTERFACE_VERSION0xy in libsteam_api.so.\n");
	return;
    }

    steamInitialized = True;
}

/**
 *
 * Overloads for LD_PRELOAD
 *
 */
extern int
XEventsQueued(Display *dpy, int mode)
{
#ifdef DEBUG
    fprintf(stderr, "XEventsQueued\n");
#endif
    handleRequest(dpy);
    return _realXEventsQueued(dpy, mode);
}

extern int
XLookupString(XKeyEvent *ke, char *bufret, int bufsiz, KeySym *keysym, XComposeStatus *status_in_out)
{
#ifdef DEBUG
    fprintf(stderr, "XLookupString\n");
#endif
    if (filter(ke->display, (XEvent *)ke, NULL))
    {
	handleScreenShot(ke->display, ke->window);
    }
    return _realXLookupString(ke, bufret, bufsiz, keysym, status_in_out);
}

extern int
XPending(Display *dpy)
{
#ifdef DEBUG
    fprintf(stderr, "XPending\n");
#endif
    handleRequest(dpy);
    return _realXPending(dpy);
}

extern Bool
SteamAPI_Init(void)
{
    Bool r = _realSteamAPI_Init();

    if (r)
	steamSetup();

    return r;
}

extern Bool
SteamAPI_InitSafe(void)
{
    Bool r = _realSteamAPI_InitSafe();

    if (r)
	steamSetup();

    return r;
}

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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>

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
/* The unified messages interface version */
#define STEAMUNIFIEDMESSAGES_INTERFACE_VERSION "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001"

#define ERESULT_OK 1

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
	uint64_t (*SendMethod)(void *thiz, char *meth, void *buf, uint32_t siz, uint64_t unContext);
	Bool (*GetMethodResponseInfo)(void *thiz, uint64_t h, uint32_t *siz, uint32_t *rc);
	Bool (*GetMethodResponseData)(void *thiz, uint64_t h, void *buf, uint32_t siz, Bool autoRel);
	Bool (*ReleaseMethod)(void *thiz, uint64_t h);
	Bool (*SendNotification)(void *thiz, char *sn, void *buf, uint32_t siz);
	/* ... */
    } *vtab;
} ISteamUnifiedMessages;

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
	void *funcs4[4];
	// TODO #ifdef _PS3 void *funcs4ps[1]; #endif
	void *funcs5[1];
	ISteamUnifiedMessages *(*GetISteamUnifiedMessages)(void *thiz, int32_t user, int32_t pipe, const char *);
	/* ... */
    } *vtab;
} ISteamClient;

extern ISteamClient *SteamClient(void);
extern int32_t SteamAPI_GetHSteamPipe(void);
extern int32_t SteamAPI_GetHSteamUser(void);
extern const char *SteamAPI_GetSteamInstallPath(void);

extern void *SteamService_GetIPCServer(void);

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
static ISteamUnifiedMessages *iunimsg = NULL;
static timer_t userFbTimer;
static Window userFbWindow = 0;
static Display *_dpy;
static struct sigaction sigAction;
static struct sigaction oldSigAction;
static XWindowAttributes oldThumbAttrs;


extern Bool ssspRunning;
Bool ssspRunning = False;

static void
sigHandler(int sig, siginfo_t *info, void *c UNUSED)
{
    fprintf(stderr, "sigHandler\n");
    switch (sig)
    {
	case SIGALRM:
	    if (info->si_code == SI_TIMER &&
		info->si_value.sival_ptr == userFbTimer)
	    {
		if (userFbWindow) XUnmapWindow(_dpy, userFbWindow);
		return;
	    }
	/* Fallthrough */
    }

    oldSigAction.sa_handler(sig);
}

static void *
findHook(const char *mod, const char *name)
{
    void *h = NULL;
    void *m = mod ? dlopen(mod, RTLD_NOW) : RTLD_NEXT;

    if (!m)
	fprintf(stderr, "Unable to query module %s!\n", mod);

    h = m ? dlsym(m, name) : NULL;

    if (!h)
	fprintf(stderr, "Unable to hook %s!\n", name);

    return h;
}

/* Initialization */
__attribute__((constructor)) static void
init(void)
{
    fprintf(stderr, "sssp_xy.so loaded into program '%s' (%s).\n",
		    program_invocation_short_name, program_invocation_name);

    /* TODO blacklist steam* executables */
    /* TODO don't do anything if gameoverlayrenderer.so is loaded? */

    if (ssspRunning)
    {
	fprintf(stderr, "SSSP already loaded! Check your LD_PRELOAD.\n");
	/* TODO don't fail? */
	exit(1);
    }

    ssspRunning = True;
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

    sigAction.sa_sigaction = sigHandler;
    sigAction.sa_flags = SA_SIGINFO;
    sigemptyset(&sigAction.sa_mask);
    sigaction(SIGALRM, &sigAction, &oldSigAction);
    /**
     * man 2 timer_create
     * Specifying sevp as NULL is equivalent to specifying a pointer to a
     * sigevent structure in which sigev_notify is SIGEV_SIGNAL, sigev_signo
     * is SIGALRM, and sigev_value.sival_int is the timer ID */
    timer_create(CLOCK_MONOTONIC, NULL, &userFbTimer);

    fprintf(stderr, "sssp_xy.so initialized.\n");
}

/* Finalization */
__attribute__((destructor)) static void
deinit(void)
{
    /* TODO */
    fprintf(stderr, "sssp_xy.so being unloaded from program '%s' (%s).\n",
		    program_invocation_short_name, program_invocation_name);

    // timer_delete(&userFbTimer); invalid pointer?
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

    fprintf(stderr, "Grabbed image of size %dx%d and depth %d.\n", image->width, image->height, image->depth);

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
    /* User feedback size */
    int w, h;
    XWindowAttributes attrs;

    /* Image grabbed through X11 and converted to RGB */
    void *image = captureScreenShot(dpy, win, &w, &h);
    if (!image)
	return;

    /* User feedback */
#if 1
    if (XGetWindowAttributes(dpy, win, &attrs) != 0)
    {
	const int fbb = 2, fbh = 100, fbw = fbh * (attrs.width * 1.0 / attrs.height);
	double s = (1.0 * fbh) / attrs.height;
	XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, attrs.visual);

	/* Hide thumb if it's there */
	if (userFbWindow)
	{
	    XUnmapWindow(dpy, userFbWindow);
	    /* And destroy on size change */
	    if (attrs.width != oldThumbAttrs.width || attrs.height != oldThumbAttrs.height)
	    {
		XDestroyWindow(dpy, userFbWindow);
		userFbWindow = 0;
	    }
	}

	/* (Re-)init if needed */
	if (!userFbWindow)
	{
	    userFbWindow = XCreateSimpleWindow(dpy, win,
		attrs.width - fbw - fbb - fbb, attrs.height - fbh - fbb - fbb, fbw, fbh,
		fbb, 0x45323232, 0);
	    oldThumbAttrs = attrs;
	}

	/* Redirect src and thumb window to offscreen */
	XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
	XCompositeRedirectWindow(dpy, userFbWindow, CompositeRedirectAutomatic);
	/* Save a reference to the current pixmap */
	Pixmap pix = XCompositeNameWindowPixmap(dpy, win);
	Picture picture = XRenderCreatePicture(dpy, pix, fmt, 0, 0);
	/* Scale to the thumb size */
	XTransform scale = {{{XDoubleToFixed(1), 0, 0}, {0, XDoubleToFixed(1), 0}, {0, 0, XDoubleToFixed(s)}}};
	XRenderSetPictureTransform(dpy, picture, &scale);
	/* Xrender target for thumb */
	Picture pic2 = XRenderCreatePicture(dpy, userFbWindow, fmt, 0, 0);
	XMapWindow(dpy, userFbWindow);

	/* Compose into the thumb window */
	XRenderComposite(dpy, PictOpSrc, picture, None, pic2, 0, 0, 0, 0, 0, 0, fbw, fbh);
	/* Free */
	XRenderFreePicture(dpy, picture);
	XRenderFreePicture(dpy, pic2);

	/* Start unmap timer */
	_dpy = dpy;
	const struct itimerspec tval = { { 0, 0 }, { 5, 0} };
	timer_settime(&userFbTimer, 0, &tval, NULL);
    }
#endif

    /* Issue the RGB image directly to steam. */
    if (steamInitialized && !iscrshot->vtab->WriteScreenshot(iscrshot, image, 3 * w * h, w, h))
    {
	fprintf(stderr, "Failed to issue screenshot to steam.\n");
    }
    else
    {
	// TODO
	fprintf(stderr, "Steam not initialized, doing custom save.\n");
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

	/* STEAM_DIR/userdata/accountID/760/remote/appID/screenshots/2015-02-24_00002.jpg */
	snprintf(path, sizeof(path), "%s/userdata/%d/760/remote/%d/screenshots/%s_%05d.jpg",
		SteamAPI_GetSteamInstallPath(), steamUserID.asComponent.accountID,
		steamAppID, date, count);
#if DEBUG > 2
	fprintf(stderr, "screenshot file: '%s'\n", path);
#endif

	/* TODO mkdir -p */
#endif

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
	if (!ke->send_event && !(ke->state & 0xFF/* kbd modifiers */))
	{
	    KeySym keysym = XLookupKeysym(ke, 0);
	    if (keysym == XK_F12)
	    {
		static Time t = 0;
		/* Let's not run havoc on too many events (KeyRepeat?). Allow every 50ms */
		if (ke->time - t > 50)
		{
		    t = ke->time;
#if DEBUG > 1
		    fprintf(stderr, "keysym: 0x%lx\n", keysym);
#endif
		    return True;
		}
#if DEBUG > 1
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
#if DEBUG > 1
    fprintf(stderr, "client: %p\n", sc);
    fprintf(stderr, "hpipe: %d\n", hsp);
    fprintf(stderr, "huser: %d\n", hsu);
#endif

    su = sc->vtab->GetISteamUser(sc, hsu, hsp, STEAMUSER_INTERFACE_VERSION);
    if (!su)
    {
	fprintf(stderr, "ERROR: SteamUser is NULL! "
			"Check interface version SteamUser0xy in libsteam_api.so.\n");
	return;
    }

    steamUserID = su->vtab->GetSteamID(su);
#if DEBUG > 1
    fprintf(stderr, "id: %llu %u\n", steamUserID.as64Bit, steamUserID.asComponent.accountID);
    fflush(stderr);
#endif

    sut = sc->vtab->GetISteamUtils(sc, hsp, STEAMUTILS_INTERFACE_VERSION);
    if (!sut)
    {
	fprintf(stderr, "ERROR: SteamUtils is NULL! "
			"Check interface version SteamUtils0xy in libsteam_api.so.\n");
	return;
    }
    steamAppID = sut->vtab->GetAppID(sut);
#if DEBUG > 1
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

    iunimsg = sc->vtab->GetISteamUnifiedMessages(sc, hsu, hsp, STEAMUNIFIEDMESSAGES_INTERFACE_VERSION);
    if (!iunimsg)
    {
	fprintf(stderr, "ERROR: SteamUnifiedMessages is NULL! "
			"Check interface version STEAMUNIFIEDMESSAGES_INTERFACE_VERSION0xy in libsteam_api.so.\n");
	return;
    }

#if 0
    Bool b;

#if 0
    sleep(1);
    b = iunimsg->vtab->SendNotification(iunimsg, "Notification.ScreenshotTaken#1", "Test", 5);
    fprintf(stderr, "SendNotification: %d\n", b);
    sleep(1);
    b = iunimsg->vtab->SendNotification(iunimsg, ".Notifications_ShowMessage#1", "lalelu", 6);
    fprintf(stderr, "SendNotification: %d\n", b);
    sleep(1);
    b = iunimsg->vtab->SendNotification(iunimsg, "MsgTest.NotifyServer#Notification", "lalelu", 6);
    fprintf(stderr, "SendNotification: %d\n", b);
    sleep(1);
    b = iunimsg->vtab->SendNotification(iunimsg, "MsgTest.NotifyClient#Notification", "lalelu", 6);
    fprintf(stderr, "SendNotification: %d\n", b);
    sleep(1);
#endif
    sleep(1);
    //b = iunimsg->vtab->SendNotification(iunimsg, "PlayerClient.NotifyLastPlayedTimes#1", "1234", 4);
    b = iunimsg->vtab->SendNotification(iunimsg, "GetLastAchievementUnlocked", 0, 0);
    fprintf(stderr, "SendNotification: %d\n", b);
#endif

#if 0
    uint64_t i;
    uint32_t siz, rc;

    sleep(1);
    //i = iunimsg->vtab->SendMethod(iunimsg, "Player.GetGameBadgeLevels#1", 0, 0, 0);
    //i = iunimsg->vtab->SendMethod(iunimsg, "Player.ClientGetLastPlayedTimes#1", 0, 0, 0);
    //i = iunimsg->vtab->SendMethod(iunimsg, "GameNotifications.GetSessionDetails#1", 0, 0, 0);

    //i = iunimsg->vtab->SendMethod(iunimsg, "GameNotificationsClient.OnNotificationsRequested#1", "test", 4, 1);
    //i = iunimsg->vtab->SendMethod(iunimsg, "PlayerClient.NotifyLastPlayedTimes#1", 0, 0, 0);

    //i = iunimsg->vtab->SendMethod(iunimsg, "GameNotifications.UpdateNotificationSettings#1", 0, 0, 1);
    //i = iunimsg->vtab->SendMethod(iunimsg, "Notification.ScreenshotTaken#1", "lala", 4, 0);
    i = iunimsg->vtab->SendMethod(iunimsg, "GetLastAchievementUnlocked", 0, 0, 1);
    fprintf(stderr, "SendMethod: %llu\n", i);
    sleep(1);
    b = iunimsg->vtab->GetMethodResponseInfo(iunimsg, i, &siz, &rc);
    fprintf(stderr, "GetMethodResponseInfo: %d %d %d\n", b, siz, rc);
    //if (b && rc == ERESULT_OK)
    {
	char a[siz];
        b = iunimsg->vtab->GetMethodResponseData(iunimsg, i, a, siz, True);
	fprintf(stderr, "GetMethodResponseInfo: %d < ", b);
	uint32_t k;
	for (k = 0; k < siz; k++)
	{
	    fprintf(stderr, "%d ", a[k]);
	}
	fprintf(stderr, "/>\n");
    }

#endif

#if 0
    hookFunc pf = (hookFunc)findHook("steamservice.so", "SteamService_GetIPCServer");
    if (pf)
    {
    void *p = (void *)pf();
    fprintf(stderr, "SteamService_GetIPCServer: %p\n", p);
    }
#endif

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
#if DEBUG > 2
    fprintf(stderr, "XEventsQueued\n");
#endif
    handleRequest(dpy);
    return _realXEventsQueued(dpy, mode);
}

extern int
XLookupString(XKeyEvent *ke, char *bufret, int bufsiz, KeySym *keysym, XComposeStatus *status_in_out)
{
#if DEBUG > 2
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
#if DEBUG > 2
    fprintf(stderr, "XPending\n");
#endif
    handleRequest(dpy);
    return _realXPending(dpy);
}

extern Bool
SteamAPI_Init(void)
{
    Bool r;

#if DEBUG > 1
    fprintf(stderr, "SteamAPI_Init\n");
#endif
    r = _realSteamAPI_Init();

    if (r)
	steamSetup();

    return r;
}

extern Bool
SteamAPI_InitSafe(void)
{
    Bool r;

#if DEBUG > 1
    fprintf(stderr, "SteamAPI_InitSafe\n");
#endif
    r = _realSteamAPI_InitSafe();

    if (r)
	steamSetup();

    return r;
}

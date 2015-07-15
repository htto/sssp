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
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#define UNUSED __attribute__((unused))
#define CPPSTR(s) #s
#define ISTEAMERROR(i, v) "ERROR: " #i " is NULL! " \
	"Check interface version " CPPSTR(v) " in libsteam_api.so."

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
		uint32_t accountID : 32;		/* unique account identifier */
		uint32_t accountInstance : 20;	/* dynamic instance ID */
		uint8_t eAccountType : 4;		/* type of account - can't show as EAccountType, due to signed / unsigned difference */
		uint8_t	eUniverse : 8;			/* universe this account belongs to */
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

/* Steam functions in libsteam_api */
extern ISteamClient *SteamClient(void);
extern int32_t SteamAPI_GetHSteamPipe(void);
extern int32_t SteamAPI_GetHSteamUser(void);
extern const char *SteamAPI_GetSteamInstallPath(void);
extern void *SteamService_GetIPCServer(void);

/* Hooks */
typedef int (*hookFunc)(void);
typedef int (*hookCPFunc)(const void *, ...);
typedef int (*hookPFunc)(void *, ...);
typedef void (*hookVPFunc)(void *, ...);
typedef void *(*hookPCPFunc)(const void *, ...);
hookFunc g_realSteamAPI_Init;
hookFunc g_realSteamAPI_InitSafe;
hookPFunc g_realXEventsQueued;
hookPFunc g_realXLookupString;
hookPCPFunc g_realXOpenDisplay;
hookPFunc g_realXPending;

/* Steam variables */
static SteamID g_steamUserID;
static uint32_t g_steamAppID;
static Bool g_steamInitialized = False;
static ISteamScreenshots *g_steamIScreenshot = NULL;
static ISteamUnifiedMessages *g_steamIUnifiedMessage = NULL;

/* X11 */
static Display *g_xDisplay;
static KeyCode g_xKeyCodeF11;
static KeyCode g_xKeyCodeF12;

/* Screenshot handling */
timer_t g_screenshotTimer;
static Window g_shotWin = 0;

/* User feedback (aka thumb view) */
timer_t g_userFbTimer;
static Window g_userFbWin = 0;
static XWindowAttributes g_oldFbAttrs;

/* Internal duplicate loading check */
extern Bool ssspRunning;
Bool ssspRunning = False;

/**
 *
 * Timer handlers.
 *
 */

static void doScreenShot(Display *dpy, Window win);
static void screenshotTimerHandler(union sigval val UNUSED)
{
	if (g_shotWin)
	{
		// Issue capturing.
		doScreenShot(g_xDisplay, g_shotWin);
	}
}

static void userFbTimerHandler(union sigval val UNUSED)
{
	if (g_userFbWin)
	{
		/* Disappears on next repaint of parent */
		XUnmapWindow(g_xDisplay, g_userFbWin);
		/* Try to trigger repaint by flushing events */
		XFlush(g_xDisplay);
	}
}

/**
 *
 * Initialization and hooking stuff.
 *
 */

static void *findHook(const char *mod, const char *name)
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
__attribute__((constructor)) static void init(void)
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
	g_realXEventsQueued = (hookPFunc)findHook(NULL, "XEventsQueued");
	g_realXOpenDisplay = (hookPCPFunc)findHook(NULL, "XOpenDisplay");
	g_realXLookupString = (hookPFunc)findHook(NULL, "XLookupString");
	g_realXPending = (hookPFunc)findHook(NULL, "XPending");
	/* We need symbols from libsteam_api, so require it to be loaded. */
	g_realSteamAPI_Init = (hookFunc)findHook("libsteam_api.so", "SteamAPI_Init");
	g_realSteamAPI_InitSafe = (hookFunc)findHook("libsteam_api.so", "SteamAPI_InitSafe");

	if (!(g_realXEventsQueued && g_realXLookupString && g_realXPending &&
				g_realSteamAPI_Init && g_realSteamAPI_InitSafe))
	{
		fprintf(stderr, "ERROR: Unable to set up hooks. Won't work this way. "
				"Please disable this module from being LD_PRELOAD'ed.\n");
		/* TODO don't fail? */
		//exit(1);
	}

	struct sigevent sevp;
	sevp.sigev_notify = SIGEV_THREAD;
	sevp.sigev_value.sival_ptr = &g_userFbTimer;
	sevp.sigev_notify_function = userFbTimerHandler;
	sevp.sigev_notify_attributes = NULL;
	int rc = timer_create(CLOCK_MONOTONIC, &sevp, &g_userFbTimer);
	if (rc)
		perror("timer_create(g_userFbTimer)");

	sevp.sigev_notify_function = screenshotTimerHandler;
	sevp.sigev_notify_attributes = NULL;
	timer_create(CLOCK_MONOTONIC, &sevp, &g_screenshotTimer);
	if (rc)
		perror("timer_create(g_screenshotTimer)");

	/* Init X11 thread support */
	XInitThreads();

	fprintf(stderr, "sssp_xy.so initialized.\n");
}

/* Finalization */
__attribute__((destructor)) static void deinit(void)
{
	/* TODO */
	fprintf(stderr, "sssp_xy.so being unloaded from program '%s' (%s).\n",
			program_invocation_short_name, program_invocation_name);

	timer_delete(g_userFbTimer);
}

/**
 *
 * Screenshot handling.
 *
 */

static inline uint8_t mask32To8(uint32_t val, uint32_t mask)
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
static void *captureScreenShot(Display *dpy, Window win, int *w, int *h)
{
	XImage *image;
	XWindowAttributes attrs;
	uint8_t *data;
	int i;

	/* XGrabServer(dpy); */
	if (XGetWindowAttributes(dpy, win, &attrs) == 0 ||
			/* TODO switch to XRenderCreatePicture */
			(image = XGetImage(dpy, win, 0, 0, attrs.width, attrs.height,
							   AllPlanes, ZPixmap)) == NULL)
	{
		/* XUngrabServer(dpy); */
		fprintf(stderr, "failed to acquire window attributes/screenshot!");
		return NULL;
	}
	/* XUngrabServer(dpy); */

#if DEBUG > 0
	fprintf(stderr, "Grabbed image of size %dx%d and depth %d.\n", image->width, image->height, image->depth);
#endif

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

static void handleScreenShot(Display *dpy, Window win)
{
	fprintf(stderr, "handleScreenShot\n");
	g_xDisplay = dpy;
	g_shotWin = win;

	struct itimerspec tval;
	tval.it_value.tv_sec = 0;
	tval.it_value.tv_nsec = 10000;
	tval.it_interval.tv_sec = 0;
	tval.it_interval.tv_nsec = 0;
	int rc = timer_settime(g_screenshotTimer, 0, &tval, NULL);
	if (rc)
		perror("timer_settime(g_screenshotTimer)");
}

static void doScreenShot(Display *dpy, Window win)
{
	/* User feedback size */
	int w, h;
	XWindowAttributes attrs;
	fprintf(stderr, "doScreenShot\n");

	if (g_userFbWin)
	{
		/* Hide feedback window */
		XUnmapWindow(dpy, g_userFbWin);
	}

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
		if (g_userFbWin)
		{
#if 0
			/* Issue damage notification, since we're going to capture it again right now */
			XserverRegion reg = XFixesCreateRegionFromWindow(dpy, g_userFbWin, 0);
			XDamageAdd(dpy, win, reg);
			XDamageAdd(dpy, g_userFbWin, reg);
			XFixesDestroyRegion(dpy, reg);
#endif

			/* And destroy on size change */
			if (attrs.width != g_oldFbAttrs.width || attrs.height != g_oldFbAttrs.height)
			{
				fprintf(stderr, "XDestroyWindow(g_userFbWin)\n");
				XDestroyWindow(dpy, g_userFbWin);
				g_userFbWin = 0;
			}
		}

		XFlush(dpy);

		/* (Re-)init if needed */
		if (!g_userFbWin)
		{
			g_userFbWin = XCreateSimpleWindow(dpy, win,
					attrs.width - fbw - fbb - fbb, attrs.height - fbh - fbb - fbb, fbw, fbh,
					fbb, 0x45323232, 0);
			g_oldFbAttrs = attrs;
		}

		/* Redirect src and thumb window to offscreen */
		XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
		XCompositeRedirectWindow(dpy, g_userFbWin, CompositeRedirectAutomatic);
		/* Save a reference to the current pixmap */
		Pixmap pix = XCompositeNameWindowPixmap(dpy, win);
		Picture picture = XRenderCreatePicture(dpy, pix, fmt, 0, 0);
		/* Scale to the thumb size */
		XTransform scale = {{{XDoubleToFixed(1), 0, 0}, {0, XDoubleToFixed(1), 0}, {0, 0, XDoubleToFixed(s)}}};
		XRenderSetPictureTransform(dpy, picture, &scale);

		/* Xrender target for thumb */
		Picture pic2 = XRenderCreatePicture(dpy, g_userFbWin, fmt, 0, 0);
		XMapWindow(dpy, g_userFbWin);

		/* Compose into the thumb window */
		XRenderComposite(dpy, PictOpSrc, picture, None, pic2, 0, 0, 0, 0, 0, 0, fbw, fbh);
		/* Free */
		XRenderFreePicture(dpy, picture);
		XRenderFreePicture(dpy, pic2);
		XCompositeUnredirectWindow(dpy, win, CompositeRedirectAutomatic);
		XCompositeUnredirectWindow(dpy, g_userFbWin, CompositeRedirectAutomatic);

		/* Start unmap timer */
		g_xDisplay = dpy;
		struct itimerspec tval;
		tval.it_value.tv_sec = 5;
		tval.it_value.tv_nsec = 0;
		tval.it_interval.tv_sec = 0;
		tval.it_interval.tv_nsec = 0;
		int rc = timer_settime(g_userFbTimer, 0, &tval, NULL);
		if (rc)
			perror("timer_settime(g_userFbTimer)");
	}
#endif

	/* Issue the RGB image directly to steam. */
	if (g_steamInitialized && !g_steamIScreenshot->vtab->WriteScreenshot(g_steamIScreenshot, image, 3 * w * h, w, h))
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
				SteamAPI_GetSteamInstallPath(), g_steamUserID.asComponent.accountID,
				g_steamAppID, date, count);
#if DEBUG > 2
		fprintf(stderr, "screenshot file: '%s'\n", path);
#endif

		/* TODO mkdir -p */
#endif

	}
	free(image);
}

/* Filter XEvent */
static Bool filter(Display *dpy UNUSED, XEvent *event, XPointer arg UNUSED)
{
	XKeyEvent *ke = NULL;
	Bool rc = False;
	static Time t = 0;

#if DEBUG > 4
	fprintf(stderr, "filter()\n");
#endif
	if (event->type == KeyPress /*|| event->type == KeyRelease*/)
	{
#if DEBUG > 4
		fprintf(stderr, "key press/release\n");
#endif
		ke = (XKeyEvent *)event;
		if (!ke->send_event && !(ke->state & 0xFF/* kbd modifiers */))
		{
#if DEBUG > 3
			fprintf(stderr, "got keycode: 0x%x\n", ke->keycode);
#endif
			if (ke->keycode == g_xKeyCodeF11)
			{
#if DEBUG > 2
				fprintf(stderr, "Stats key recognized\n");
#endif
				rc = True;
				/* TODO delay/call through thread */
				doStatsUpdate();
			}
			else if (ke->keycode == g_xKeyCodeF12)
			{
				/* Let's not run havoc on too many events (KeyRepeat?). Allow every 50ms */
				if (ke->time - t > 50)
				{
					t = ke->time;
					rc = True;
#if DEBUG > 2
					fprintf(stderr, "Screenshot key recognized\n");
#endif
				}
#if DEBUG > 1
				else
					fprintf(stderr, "Screenshot key skipped due to flooding (<50ms)\n");
#endif
			}
		}

	}
	return rc;
}

static void handleRequest(Display *dpy)
{
	XEvent e;

	if (!g_steamInitialized)
		return;

	/* TODO reduce eventqueue search */
	if (XCheckIfEvent(dpy, &e, filter, NULL))
	{
		handleScreenShot(dpy, ((XAnyEvent*)&e)->window);
	}
}

/* Grab the various handles from the interfaces. Actually userid and appid aren't needed. */
static void steamSetup(void)
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
		fprintf(stderr, ISTEAMERROR(SteamUser, STEAMUSER_INTERFACE_VERSION));
		return;
	}

	g_steamUserID = su->vtab->GetSteamID(su);
#if DEBUG > 1
	fprintf(stderr, "id: %llu %u\n", g_steamUserID.as64Bit, g_steamUserID.asComponent.accountID);
	fflush(stderr);
#endif

	sut = sc->vtab->GetISteamUtils(sc, hsp, STEAMUTILS_INTERFACE_VERSION);
	if (!sut)
	{
		fprintf(stderr, ISTEAMERROR(SteamUtils, STEAMUTILS_INTERFACE_VERSION));
		return;
	}
	g_steamAppID = sut->vtab->GetAppID(sut);
#if DEBUG > 1
	fprintf(stderr, "aid: %u\n", g_steamAppID);
	fflush(stderr);
#endif

	g_steamIScreenshot = sc->vtab->GetISteamScreenshots(sc, hsu, hsp, STEAMSCREENSHOTS_INTERFACE_VERSION);
	if (!g_steamIScreenshot)
	{
		fprintf(stderr, ISTEAMERROR(SteamScreenshots, STEAMSCREENSHOTS_INTERFACE_VERSION));
		return;
	}

	g_steamIUnifiedMessage = sc->vtab->GetISteamUnifiedMessages(sc, hsu, hsp, STEAMUNIFIEDMESSAGES_INTERFACE_VERSION);
	if (!g_steamIUnifiedMessage)
	{
		fprintf(stderr, ISTEAMERROR(SteamUnifiedMessages, STEAMUNIFIEDMESSAGES_INTERFACE_VERSION));
		return;
	}

#if 0
	Bool b;

#if 0
	sleep(1);
	b = g_steamIUnifiedMessage->vtab->SendNotification(g_steamIUnifiedMessage, "Notification.ScreenshotTaken#1", "Test", 5);
	fprintf(stderr, "SendNotification: %d\n", b);
	sleep(1);
	b = g_steamIUnifiedMessage->vtab->SendNotification(g_steamIUnifiedMessage, ".Notifications_ShowMessage#1", "lalelu", 6);
	fprintf(stderr, "SendNotification: %d\n", b);
	sleep(1);
	b = g_steamIUnifiedMessage->vtab->SendNotification(g_steamIUnifiedMessage, "MsgTest.NotifyServer#Notification", "lalelu", 6);
	fprintf(stderr, "SendNotification: %d\n", b);
	sleep(1);
	b = g_steamIUnifiedMessage->vtab->SendNotification(g_steamIUnifiedMessage, "MsgTest.NotifyClient#Notification", "lalelu", 6);
	fprintf(stderr, "SendNotification: %d\n", b);
	sleep(1);
#endif
	sleep(1);
	//b = g_steamIUnifiedMessage->vtab->SendNotification(g_steamIUnifiedMessage, "PlayerClient.NotifyLastPlayedTimes#1", "1234", 4);
	b = g_steamIUnifiedMessage->vtab->SendNotification(g_steamIUnifiedMessage, "GetLastAchievementUnlocked", 0, 0);
	fprintf(stderr, "SendNotification: %d\n", b);
#endif

#if 0
	uint64_t i;
	uint32_t siz, rc;

	sleep(1);
	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "Player.GetGameBadgeLevels#1", 0, 0, 0);
	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "Player.ClientGetLastPlayedTimes#1", 0, 0, 0);
	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "GameNotifications.GetSessionDetails#1", 0, 0, 0);

	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "GameNotificationsClient.OnNotificationsRequested#1", "test", 4, 1);
	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "PlayerClient.NotifyLastPlayedTimes#1", 0, 0, 0);

	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "GameNotifications.UpdateNotificationSettings#1", 0, 0, 1);
	//i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "Notification.ScreenshotTaken#1", "lala", 4, 0);
	i = g_steamIUnifiedMessage->vtab->SendMethod(g_steamIUnifiedMessage, "GetLastAchievementUnlocked", 0, 0, 1);
	fprintf(stderr, "SendMethod: %llu\n", i);
	sleep(1);
	b = g_steamIUnifiedMessage->vtab->GetMethodResponseInfo(g_steamIUnifiedMessage, i, &siz, &rc);
	fprintf(stderr, "GetMethodResponseInfo: %d %d %d\n", b, siz, rc);
	//if (b && rc == ERESULT_OK)
	{
		char a[siz];
		b = g_steamIUnifiedMessage->vtab->GetMethodResponseData(g_steamIUnifiedMessage, i, a, siz, True);
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

	g_steamInitialized = True;
}

/**
 *
 * Overloads for LD_PRELOAD
 *
 */
extern Display *XOpenDisplay(const char *name)
{
#if DEBUG > 2
	fprintf(stderr, "%s()\n", __FUNCTION__);
#endif
	Display *dpy = g_realXOpenDisplay(name);

	if (dpy)
	{
		/* Initialize and get key codes for filter(). Should also reduce
		 * possibility of dead-locking during runtime for apps doing excessive
		 * display locking. */
		g_xKeyCodeF12 = XKeysymToKeycode(dpy, XK_F11);
		g_xKeyCodeF12 = XKeysymToKeycode(dpy, XK_F12);
	}

#if DEBUG > 2
	fprintf(stderr, "%s() returning %p\n", __FUNCTION__, dpy);
#endif
	return dpy;
}

extern int XEventsQueued(Display *dpy, int mode)
{
#if DEBUG > 2
	fprintf(stderr, "%s()\n", __FUNCTION__);
#endif
	handleRequest(dpy);
#if DEBUG > 3
	fprintf(stderr, "%s() calling real\n", __FUNCTION__);
#endif
	int rc = g_realXEventsQueued(dpy, mode);
#if DEBUG > 2
	fprintf(stderr, "%s() returning %d\n", __FUNCTION__, rc);
#endif
	return rc;
}

extern int XLookupString(XKeyEvent *ke, char *bufret, int bufsiz,
		KeySym *keysym, XComposeStatus *status_in_out)
{
#if DEBUG > 2
	fprintf(stderr, "%s()\n", __FUNCTION__);
#endif
	if (filter(ke->display, (XEvent *)ke, NULL))
	{
		handleScreenShot(ke->display, ke->window);
	}
#if DEBUG > 3
	fprintf(stderr, "%s() calling real\n", __FUNCTION__);
#endif
	int rc = g_realXLookupString(ke, bufret, bufsiz, keysym, status_in_out);
#if DEBUG > 2
	fprintf(stderr, "%s() returning %d\n", __FUNCTION__, rc);
#endif
	return rc;
}

extern int XPending(Display *dpy)
{
#if DEBUG > 2
	fprintf(stderr, "%s()\n", __FUNCTION__);
#endif
	handleRequest(dpy);
#if DEBUG > 3
	fprintf(stderr, "%s() calling real\n", __FUNCTION__);
#endif
	int rc = g_realXPending(dpy);
#if DEBUG > 2
	fprintf(stderr, "%s() returning %d\n", __FUNCTION__, rc);
#endif
	return rc;
}

extern Bool SteamAPI_Init(void)
{
	Bool r;

#if DEBUG > 1
	fprintf(stderr, "SteamAPI_Init\n");
#endif
	r = g_realSteamAPI_Init();

	if (r)
		steamSetup();

	return r;
}

extern Bool SteamAPI_InitSafe(void)
{
	Bool r;

#if DEBUG > 1
	fprintf(stderr, "SteamAPI_InitSafe\n");
#endif
	r = g_realSteamAPI_InitSafe();

	if (r)
		steamSetup();

	return r;
}

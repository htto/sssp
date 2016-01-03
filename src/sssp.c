/**
 *
 * A small LD_PRELOAD library for steam games, that captures a game screenshot
 * when the hotkey (XK_F12) is hit.
 *
 * gcc -m32 -o sssp_32.so sssp.c -shared -fPIC `pkg-config --cflags --libs x11`
 * gcc -m64 -o sssp_64.so sssp.c -shared -fPIC `pkg-config --cflags --libs x11`
 *
 */
#include <dlfcn.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#include "steam_sdk.h"
#include "sssp.h"

/* Hooks */
hookFunc g_realSteamAPI_Init;
hookFunc g_realSteamAPI_InitSafe;
hookPFunc g_realXEventsQueued;
hookPFunc g_realXLookupString;
hookPCPFunc g_realXOpenDisplay;
hookPFunc g_realXPending;

hookPPFunc g_realDlsym = NULL;

/* Steam variables */
static SteamID g_steamUserID;
static SteamAppID g_steamAppID;
static Bool g_steamInitialized = False;
static ISteamScreenshots *g_steamIScreenshot = NULL;
static ISteamUnifiedMessages *g_steamIUnifiedMessage = NULL;
static ISteamUserStats *g_steamIUserStats = NULL;

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

enum LogLevel g_logLevel = DFLT_LOG_LEVEL;

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
		/* FIXME handle error (BadWindow?) */
		/* Disappears on next repaint of parent */
		XUnmapWindow(g_xDisplay, g_userFbWin);
		/* Try to trigger repaint by flushing events */
		XFlush(g_xDisplay);
		usleep(50000);
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
	void *m = mod ? dlopen(mod, RTLD_NOW | RTLD_GLOBAL) : RTLD_NEXT;

	if (mod && !m)
		log(LOG_ERROR, "Unable to query module %s!\n", mod);

	h = m ? g_realDlsym(m, name) : NULL;

	if (!h)
		log(LOG_ERROR, "Unable to hook %s!\n", name);

	return h;
}

/* Look up the real dlsym, to filter and redirect dlsym calls. */
static Bool findDlSym(void)
{
#ifdef _GNU_SOURCE
	ElfW(Sym) *sym;
	ElfW(Addr) base = 0, strTab = 0, symTab = 0;
	struct link_map *dli = NULL;

	void *mm = dlopen("libdl.so.2", RTLD_NOW);
	if (dlinfo(mm, RTLD_DI_LINKMAP, &dli) == 0 && dli)
		base = dli->l_addr;

	if (base)
	{
		for (ElfW(Dyn) *dyn = dli->l_ld; dyn->d_tag != DT_NULL; ++dyn)
		{
			switch (dyn->d_tag)
			{
				case DT_STRTAB:
					strTab = dyn->d_un.d_ptr;
					break;
				case DT_SYMTAB:
					symTab = dyn->d_un.d_ptr;
					break;
			}
		}

		for (sym = (ElfW(Sym) *)symTab; sym < (ElfW(Sym) *)strTab; sym++)
		{
			if (strncmp((char *)(strTab + sym->st_name), "dlsym", 5) == 0)
			{
				g_realDlsym = (hookPPFunc)(base + sym->st_value);
				break;
			}
		}
	}

	dlclose(mm);
#else
	log(LOG_ERROR, "No dlsym hooking possible. Expect issues.\n");
	g_realDlsym = (hookPPFunc)dlsym;
#endif
	return g_realDlsym != NULL;
}

/* Initialization */
__attribute__((constructor)) static void init(void)
{
#ifndef _GNU_SOURCE
	/* Try some shell setting */
	program_invocation_name = getenv("_");
	if (program_invocation_name)
		program_invocation_short_name = basename(program_invocation_name);
#endif

	if (
			// System paths /bin, /sbin are filtered
			strncmp(program_invocation_name, "/bin", 4) == 0 ||
			strncmp(program_invocation_name, "/sbin", 5) == 0 ||
			// Steam executables are filtered
			strncmp(program_invocation_short_name, "steam", 5) == 0 ||
			strncmp(program_invocation_short_name, "steamwebhelper", 14) == 0 ||
			strncmp(program_invocation_short_name, "streaming_client", 16) == 0
	)
		return;


	log(LOG_NOTICE, "sssp_xy.so loaded into program '%s' (%s).\n",
			program_invocation_short_name, program_invocation_name);

	/* TODO blacklist steam* executables */
	/* TODO don't do anything if gameoverlayrenderer.so is loaded? */

	if (ssspRunning)
	{
		log(LOG_ERROR, "SSSP already loaded! Check your LD_PRELOAD.\n");
		return;
	}

	ssspRunning = True;

	if (!findDlSym())
	{
		log(LOG_ERROR, "Unable to set up dlsym hook. Won't work this way. "
				"Please disable this module from being LD_PRELOAD'ed.\n");
		return;
	}

	g_realXEventsQueued = (hookPFunc)findHook(NULL, "XEventsQueued");
	g_realXLookupString = (hookPFunc)findHook(NULL, "XLookupString");
	g_realXOpenDisplay = (hookPCPFunc)findHook(NULL, "XOpenDisplay");
	g_realXPending = (hookPFunc)findHook(NULL, "XPending");

	if (!(g_realXEventsQueued && g_realXLookupString && g_realXOpenDisplay && g_realXPending))
	{
		log(LOG_ERROR, "Unable to set up X11 hooks. Won't work this way. "
				"Please disable this module from being LD_PRELOAD'ed.\n");
		return;
	}

	struct sigevent sevp;
	sevp.sigev_notify = SIGEV_THREAD;
	sevp.sigev_value.sival_ptr = &g_userFbTimer;
	sevp.sigev_notify_function = userFbTimerHandler;
	sevp.sigev_notify_attributes = NULL;
	int rc = timer_create(CLOCK_MONOTONIC, &sevp, &g_userFbTimer);
	if (rc)
		log(LOG_ERROR, "timer_create(g_userFbTimer): %s\n", strerror(errno));

	sevp.sigev_notify_function = screenshotTimerHandler;
	sevp.sigev_notify_attributes = NULL;
	timer_create(CLOCK_MONOTONIC, &sevp, &g_screenshotTimer);
	if (rc)
		log(LOG_ERROR, "timer_create(g_screenshotTimer): %s\n", strerror(errno));

	/* Init X11 thread support */
	XInitThreads();

	log(LOG_NOTICE, "sssp_xy.so initialized.\n");
}

/* Finalization */
__attribute__((destructor)) static void deinit(void)
{
	/* TODO */
	if (ssspRunning)
	{
		log(LOG_NOTICE, "sssp_xy.so being unloaded from program '%s' (%s).\n",
				program_invocation_short_name, program_invocation_name);

		if (g_userFbTimer)
			timer_delete(g_userFbTimer);
	}
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
static void *captureScreenShot(Display *dpy, Window *win, int *w, int *h)
{
	XWindowAttributes attrs, cattrs;
	Window c, p;
	uint8_t *data;
	int dx = -1, dy = -1;
	int i;
	XImage *image;

	if (XGetWindowAttributes(dpy, *win, &attrs) == 0)
	{
		log(LOG_ERROR, "failed to acquire window attributes!");
		return NULL;
	}

	/* Can't directly use win, because SDL1 does have three windows, but only
	 * one for the content. Instead we translate from the root window and
	 * let X hand us the appropriate mapped child window that's probably the
	 * one we want. */
	c = p = attrs.root;
	while (1/*dx != 0 || dy != 0*/)
	{
		if (!XTranslateCoordinates(dpy, *win, p, 0, 0, &dx, &dy, &c) ||
			c == None || XGetWindowAttributes(dpy, c, &cattrs) == 0 ||
			cattrs.height < attrs.height || cattrs.width < attrs.width)
		{
			break;
		}

		log(LOG_INFO, "XTranslateCoordinates: %d/%d %d/%d 0x%lx %d/%d\n", attrs.x, attrs.y, attrs.width, attrs.height, c, dx, dy);
		p = c;
	}

	/* Update win to the one we grab from and we can display the feedback in. */
	*win = p;
	/* TODO switch to XRenderCreatePicture */
	if ((image = XGetImage(dpy, *win, 0, 0, attrs.width, attrs.height, AllPlanes, ZPixmap)) == NULL)
	{
		log(LOG_ERROR, "failed to acquire window screenshot!");
		return NULL;
	}

	log(LOG_NOTICE, "Grabbed image of window 0x%lx (size %dx%d, depth %d).\n", *win, image->width, image->height, image->depth);

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
	g_xDisplay = dpy;
	g_shotWin = win;

	struct itimerspec tval;
	tval.it_value.tv_sec = 0;
	tval.it_value.tv_nsec = 10000;
	tval.it_interval.tv_sec = 0;
	tval.it_interval.tv_nsec = 0;

	log(LOG_NOTICE, "%s()\n", __FUNCTION__);

	int rc = timer_settime(g_screenshotTimer, 0, &tval, NULL);
	if (rc)
		log(LOG_ERROR, "timer_settime(g_screenshotTimer): %s\n", strerror(errno));
}

static void doScreenShot(Display *dpy, Window win)
{
	/* User feedback size */
	int w, h;
	XWindowAttributes attrs;
	union sigval unused;

	log(LOG_NOTICE, "doScreenShot(%p, 0x%lx)\n", dpy, win);

	/* Hide feedback window */
	userFbTimerHandler(unused);

	/* Image grabbed through X11 and converted to RGB */
	void *image = captureScreenShot(dpy, &win, &w, &h);
	if (!image)
		return;

	/* User feedback */
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
				log(LOG_NOTICE, "XDestroyWindow(g_userFbWin)\n");
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
			log(LOG_ERROR, "timer_settime(g_userFbTimer): %s\n", strerror(errno));
	}

	/* Issue the RGB image directly to steam. */
	if (g_steamInitialized)
	{
		if (!g_steamIScreenshot->vtab->WriteScreenshot(g_steamIScreenshot, image, 3 * w * h, w, h))
			log(LOG_ERROR, "Failed to issue screenshot to steam.\n");
	}
	else
	{
		// TODO
		log(LOG_ERROR, "Steam not initialized, no screenshot saved.\n");
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
		log(LOG_INFO, "screenshot file: '%s'\n", path);

		/* TODO mkdir -p */
#endif

	}
	free(image);
}

static void doStatsUpdate()
{
	if (!g_steamIUserStats)
		return;

#if USE_OLD_USERSTATS
	if (g_steamIUserStats->vtab->RequestCurrentStats(g_steamIUserStats, g_steamAppID))
	{
		uint32_t scount = g_steamIUserStats->vtab->GetNumStats(g_steamIUserStats, g_steamAppID);
		uint32_t i = 0;

		while (scount && i++ < scount - 1)
		{
			const char *sname = g_steamIUserStats->vtab->GetStatName(g_steamIUserStats, g_steamAppID, i);
			log(LOG_INFO, "stats[%d].sname: %s\n", i, sname);
			Bool sachieved = False;
			uint32_t tstamp = 0;
			if (g_steamIUserStats->vtab->GetAchievementAndUnlockTime(g_steamIUserStats, g_steamAppID, sname, &sachieved, &tstamp))
			{
				log(LOG_INFO, "stats[%d].achieved: %d\n", i, sachieved);
				if (sachieved)
					log(LOG_INFO, "stats[%d].tsamp: 0x%x\n", i, tstamp);
			}

			int32_t sdata = -1;
			if (g_steamIUserStats->vtab->GetStat(g_steamIUserStats, g_steamAppID, sname, &sdata))
			{
				log(LOG_INFO, "stats[%d].data: 0x%x\n", i, sdata);
			}
		}
	}
#else
	if (g_steamIUserStats->vtab->RequestCurrentStats(g_steamIUserStats))
	{
		uint32_t scount = g_steamIUserStats->vtab->GetNumAchievements(g_steamIUserStats);
		uint32_t i = 0;
		while (scount && i++ < scount - 1)
		{
			const char *sname = g_steamIUserStats->vtab->GetAchievementName(g_steamIUserStats, i);
			log(LOG_INFO, "stats[%d].sname: %s\n", i, sname);
			Bool sachieved = False;
			uint32_t tstamp = 0;
			if (g_steamIUserStats->vtab->GetAchievementAndUnlockTime(g_steamIUserStats, sname, &sachieved, &tstamp))
			{
				log(LOG_INFO, "stats[%d].achieved: %d\n", i, sachieved);
				if (sachieved)
					log(LOG_INFO, "stats[%d].tsamp: 0x%x\n", i, tstamp);
			}

			log(LOG_INFO, "NAME: %s\n", g_steamIUserStats->vtab->GetAchievementDisplayAttribute(g_steamIUserStats, sname, "name"));
			log(LOG_INFO, "DESC: %s\n", g_steamIUserStats->vtab->GetAchievementDisplayAttribute(g_steamIUserStats, sname, "desc"));
			log(LOG_INFO, "SVAL: %s\n", g_steamIUserStats->vtab->GetAchievementDisplayAttribute(g_steamIUserStats, sname, "statvalue"));
			int32_t sdata = -1;
			if (g_steamIUserStats->vtab->GetStat(g_steamIUserStats, sname, &sdata))
			{
				log(LOG_INFO, "stats[%d].data: %x\n", i, sdata);
			}
		}
	}
#endif
}

/* Filter XEvent */
static Bool filter(Display *dpy UNUSED, XEvent *event, XPointer arg UNUSED)
{
	XKeyEvent *ke = NULL;
	Bool rc = False;
	static Time t = 0;

	if (event->type == KeyPress /*|| event->type == KeyRelease*/)
	{
		log(LOG_INFO, "key press/release\n");

		ke = (XKeyEvent *)event;
		if (!ke->send_event && !(ke->state & 0xFF/* kbd modifiers */))
		{
			log(LOG_INFO, "got keycode: 0x%x\n", ke->keycode);
			if (ke->keycode == g_xKeyCodeF11)
			{
				log(LOG_NOTICE, "Stats key recognized\n");
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
					log(LOG_NOTICE, "Screenshot key recognized\n");
				}
				else
					log(LOG_WARN, "Screenshot key skipped due to flooding (<50ms)\n");
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

static Bool steamPrepare(void)
{
	/* We need symbols from libsteam_api, so require it to be loaded. */
	g_realSteamAPI_Init = (hookFunc)findHook("libsteam_api.so", "SteamAPI_Init");
	g_realSteamAPI_InitSafe = (hookFunc)findHook("libsteam_api.so", "SteamAPI_InitSafe");

	if (!(g_realSteamAPI_Init && g_realSteamAPI_InitSafe))
	{
		log(LOG_ERROR, "Unable to set up steam hooks. Won't work this way. "
				"Please disable this module from being LD_PRELOAD'ed.\n");
		return False;
	}

	return True;
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
		log(LOG_ERROR, "SteamClient is NULL!\n");
		return;
	}

	hsp = SteamAPI_GetHSteamPipe();
	hsu = SteamAPI_GetHSteamUser();
	log(LOG_INFO, "client=%p hpipe=%d huser=%d\n", sc, hsp, hsu);

	su = sc->vtab->GetISteamUser(sc, hsu, hsp, STEAMUSER_INTERFACE_VERSION);
	if (!su)
	{
		log(LOG_ERROR, ISTEAMERROR(SteamUser, STEAMUSER_INTERFACE_VERSION));
		return;
	}

	g_steamUserID = su->vtab->GetSteamID(su);
	log(LOG_WARN, "UserID+AccountID: %ju %u\n", g_steamUserID.as64Bit, g_steamUserID.asComponent.accountID);

	sut = sc->vtab->GetISteamUtils(sc, hsp, STEAMUTILS_INTERFACE_VERSION);
	if (!sut)
	{
		log(LOG_ERROR, ISTEAMERROR(SteamUtils, STEAMUTILS_INTERFACE_VERSION));
		return;
	}
	g_steamAppID.appId = sut->vtab->GetAppID(sut);
	g_steamAppID.modId = 0;
	g_steamAppID.type = 0;
	log(LOG_WARN, "AppID: %u\n", g_steamAppID.appId);

	g_steamIScreenshot = sc->vtab->GetISteamScreenshots(sc, hsu, hsp, STEAMSCREENSHOTS_INTERFACE_VERSION);
	if (!g_steamIScreenshot)
	{
		log(LOG_ERROR, ISTEAMERROR(SteamScreenshots, STEAMSCREENSHOTS_INTERFACE_VERSION));
		return;
	}

	g_steamIUnifiedMessage = sc->vtab->GetISteamUnifiedMessages(sc, hsu, hsp, STEAMUNIFIEDMESSAGES_INTERFACE_VERSION);
	if (!g_steamIUnifiedMessage)
	{
		log(LOG_ERROR, ISTEAMERROR(SteamUnifiedMessages, STEAMUNIFIEDMESSAGES_INTERFACE_VERSION));
		return;
	}

	g_steamIUserStats = sc->vtab->GetISteamUserStats(sc, hsu, hsp, STEAMUSERSTATS_INTERFACE_VERSION);
	if (!g_steamIUserStats)
	{
		log(LOG_ERROR, ISTEAMERROR(SteamUserStats, STEAMUSERSTATS_INTERFACE_VERSION));
		return;
	}

#if 0
	doStatsUpdate();
#endif

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
	Display *dpy;

	log(LOG_DEBUG, "%s(%s)\n", __FUNCTION__, name);

	dpy = (Display *)g_realXOpenDisplay(name);

	if (dpy)
	{
		/* Initialize and get key codes for filter(). Should also reduce
		 * possibility of dead-locking during runtime for apps doing excessive
		 * display locking. */
		g_xKeyCodeF11 = XKeysymToKeycode(dpy, XK_F11);
		log(LOG_NOTICE, "Handling KeyCode %d as KeySym %d\n", g_xKeyCodeF11, XK_F11);
		g_xKeyCodeF12 = XKeysymToKeycode(dpy, XK_F12);
		log(LOG_NOTICE, "Handling KeyCode %d as KeySym %d\n", g_xKeyCodeF12, XK_F12);
	}

	log(LOG_DEBUG, "%s() returning %p\n", __FUNCTION__, dpy);
	return dpy;
}

#if 1
extern int XGrabKeyboard(Display *dpy, Window win, Bool oe, int pm, int km, Time t)
{
	log(LOG_DEBUG, "%s(%p, 0x%lx, %d, 0x%x, 0x%x, 0x%lx)\n", __FUNCTION__, dpy, win, oe, pm, km, t);
	return GrabSuccess;
}

extern int XUngrabKeyboard(Display *dpy, Time t)
{
	log(LOG_DEBUG, "%s(%p, 0x%lx)\n", __FUNCTION__, dpy, t);
	return GrabSuccess;
}
#endif

#if 0
extern int XGrabPointer(Display *dpy, Window win, Bool oe, uint32_t em, int pm, int km, Window ct, Cursor c, Time t)
{
	log(LOG_DEBUG, "%s(%p, 0x%lx, %d, 0x%x, 0x%x, 0x%x, 0x%lx, 0x%lx, 0x%lx)\n", __FUNCTION__, dpy, win, oe, em, pm, km, ct, c, t);
	return GrabSuccess;
}

extern int XUngrabPointer(Display *dpy, Time t)
{
	log(LOG_DEBUG, "%s(%p, 0x%lx)\n", __FUNCTION__, dpy, t);
	return GrabSuccess;
}
#endif

#if 0
extern int XGrabServer(Display *dpy)
{
	log(LOG_DEBUG, "%s(%p)\n", __FUNCTION__, dpy);
	return AlreadyGrabbed;
}

extern int XUngrabServer(Display *dpy)
{
	log(LOG_DEBUG, "%s(%p)\n", __FUNCTION__, dpy);
	return 0;
}
#endif

extern int XEventsQueued(Display *dpy, int mode)
{
	log(LOG_DEBUG, "%s()\n", __FUNCTION__);
	handleRequest(dpy);
	log(LOG_DEBUG, "%s() calling real\n", __FUNCTION__);
	int rc = g_realXEventsQueued(dpy, mode);
	log(LOG_DEBUG, "%s() returning %d\n", __FUNCTION__, rc);
	return rc;
}

extern int XLookupString(XKeyEvent *ke, char *bufret, int bufsiz,
		KeySym *keysym, XComposeStatus *status_in_out)
{
	log(LOG_DEBUG, "%s()\n", __FUNCTION__);
	if (filter(ke->display, (XEvent *)ke, NULL))
	{
		handleScreenShot(ke->display, ke->window);
	}
	log(LOG_DEBUG, "%s() calling real\n", __FUNCTION__);
	int rc = g_realXLookupString(ke, bufret, bufsiz, keysym, status_in_out);
	log(LOG_DEBUG, "%s() returning %d\n", __FUNCTION__, rc);
	return rc;
}

extern int XPending(Display *dpy)
{
	log(LOG_DEBUG, "%s()\n", __FUNCTION__);
	handleRequest(dpy);
	log(LOG_DEBUG, "%s() calling real\n", __FUNCTION__);
	int rc = g_realXPending(dpy);
	log(LOG_DEBUG, "%s() returning %d\n", __FUNCTION__, rc);
	return rc;
}

extern Bool SteamAPI_Init(void)
{
	Bool r;

	log(LOG_DEBUG, "%s()\n", __FUNCTION__);

	if (g_steamInitialized)
		return True;

	r = steamPrepare();

	if (r)
		r = g_realSteamAPI_Init();

	if (r)
		steamSetup();

	return r;
}

extern Bool SteamAPI_InitSafe(void)
{
	Bool r;

	log(LOG_DEBUG, "%s()\n", __FUNCTION__);

	if (g_steamInitialized)
		return True;

	r = steamPrepare();

	if (r)
		r = g_realSteamAPI_InitSafe();

	if (r)
		steamSetup();

	return r;
}

#ifdef _GNU_SOURCE
extern void *dlsym(void *handle, const char *symbol)
{
	log(LOG_DEBUG, "%s(%p, %s)\n", __FUNCTION__, handle, symbol);

	/* Redirect these symbols through our ones */
	if (
		strcmp(symbol, "SteamAPI_Init") == 0 ||
		strcmp(symbol, "SteamAPI_InitSafe") == 0 ||
		strcmp(symbol, "XCloseDisplay") == 0 ||
		strcmp(symbol, "XCreateWindow") == 0 ||
		strcmp(symbol, "XEventsQueued") == 0 ||
		strcmp(symbol, "XGrabKeyboard") == 0 ||
		strcmp(symbol, "XGrabPointer") == 0 ||
		strcmp(symbol, "XLookupString") == 0 ||
		strcmp(symbol, "XOpenDisplay") == 0 ||
		strcmp(symbol, "XPending") == 0 ||
		strcmp(symbol, "XRaiseWindow") == 0 ||
		strcmp(symbol, "XReparentWindow") == 0 ||
		strcmp(symbol, "XUngrabKeyboard") == 0 ||
		strcmp(symbol, "XUngrabPointer") == 0
	)
	{
		handle = NULL;
		log(LOG_INFO, "Intercepting dlsym call for symbol %s\n", symbol);
	}

	return g_realDlsym ? g_realDlsym(handle, symbol) : NULL;
}
#endif

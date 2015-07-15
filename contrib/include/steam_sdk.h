/**
 *
 * Steam SDK information
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
/* the user stats interface version */
#if USE_OLD_USERSTATS
#	define STEAMUSERSTATS_INTERFACE_VERSION "STEAMUSERSTATS_INTERFACE_VERSION002"
#else
#	define STEAMUSERSTATS_INTERFACE_VERSION "STEAMUSERSTATS_INTERFACE_VERSION011"
#endif

#define ERESULT_OK 1

/* App id */
typedef struct {
	uint32_t appId : 24;
	uint32_t type : 8;
	uint32_t modId : 32;
} SteamAppID;

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
#if USE_OLD_USERSTATS
		uint32_t (*GetNumStats)(void *thiz, SteamAppID appId);
		const char *(*GetStatName)(void *thiz, SteamAppID appId, uint32_t idx);
		uint32_t (*GetStatType)(void *thiz, SteamAppID appId, const char *name);
		uint32_t (*GetNumAchievements)(void *thiz, SteamAppID appId);
		const char *(*GetAchievementName)(void *thiz, SteamAppID appId, uint32_t idx);
		Bool (*RequestCurrentStats)(void *thiz, SteamAppID appId);
		Bool (*GetStat)(void *thiz, SteamAppID appId, const char *name, int32_t *pData);
		Bool (*GetStatF)(void *thiz, SteamAppID appId, const char *name, float *pData);
		void *funcs1[3];
		Bool (*GetAchievementAndUnlockTime)(void *thiz, SteamAppID appId, const char *name, Bool *achieved, uint32_t *tstamp);
#else
		Bool (*RequestCurrentStats)(void *thiz);
		Bool (*GetStat)(void *thiz, const char *name, int32_t *pData);
		Bool (*GetStatF)(void *thiz, const char *name, float *pData);
		void *funcs1[3];
		Bool (*GetAchievement)(void *thiz, const char *name, Bool *achieved);
		void *funcs2[2];
		Bool (*GetAchievementAndUnlockTime)(void *thiz, const char *name, Bool *achieved, uint32_t *tstamp);
		void *funcs3[2];
		const char *(*GetAchievementDisplayAttribute)(void *thiz, const char *name, const char *key);
		void *funcs4[1];
		uint32_t (*GetNumAchievements)(void *thiz);
		const char *(*GetAchievementName)(void *thiz, uint32_t idx);
#endif
		/* ... */
	} *vtab;
} ISteamUserStats;

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
		void *funcs3[3];
		ISteamUserStats *(*GetISteamUserStats)(void *thiz, int32_t user, int32_t pipe, const char *);
		void *funcs4[4];
		ISteamScreenshots *(*GetISteamScreenshots)(void *thiz, int32_t user, int32_t pipe, const char *);
		void *funcs5[4];
		// TODO #ifdef _PS3 void *funcs5ps[1]; #endif
		void *funcs6[1];
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

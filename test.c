#include <unistd.h>

extern int SteamAPI_Init(void);
extern void SteamAPI_RunCallbacks(void);
extern void doStatsUpdate(void);

int main(int c, char **argv)
{
    int rc = SteamAPI_Init();

    sleep(1);
    doStatsUpdate();
    sleep(1);
    SteamAPI_RunCallbacks();
    sleep(1);
    SteamAPI_RunCallbacks();
    sleep(1);

    return rc;
}

#include <unistd.h>

extern int SteamAPI_Init(void);

int main(int c, char **argv)
{
    int rc = SteamAPI_Init();

    sleep(5);
    return rc;
}

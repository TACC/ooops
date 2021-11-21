#include <time.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
        int nMillSecond=100;
        struct timespec tim1, tim2;

	if(argc == 2)	{
		nMillSecond = atoi(argv[1]);
	}
        tim1.tv_sec = nMillSecond / 1000;
	tim1.tv_nsec = (nMillSecond % 1000)*1000000;
	nanosleep(&tim1 , &tim2);

        return 0;
}



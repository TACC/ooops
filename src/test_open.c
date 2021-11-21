#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	int i, nfile, fd;
	struct stat file_stat;

	nfile = atoi(argv[2]);
	for(i=0; i<nfile; i++)	{
		fd = open(argv[1], O_RDONLY);
		if(fd > 0)	close(fd);
	}

	return 0;
}



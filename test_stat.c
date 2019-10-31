#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>


int main(int argc, char *argv[])
{
	int i, nfile;
	struct stat file_stat;

	lstat(argv[1], &file_stat);
	nfile = atoi(argv[2]);
	for(i=0; i<nfile; i++)	{
		lstat(argv[1], &file_stat);
	}

	return 0;
}



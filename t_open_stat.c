/*************************************************************************
--------------------------------------------------------------------------
--  ooops License
--------------------------------------------------------------------------
--
--  ooops is licensed under the terms of the MIT license reproduced below.
--  This means that ooops is free software and can be used for both academic
--  and commercial purposes at absolutely no cost.
--
--  ----------------------------------------------------------------------
--
--  Copyright (C) 2018-2019 Lei Huang
--
--  Permission is hereby granted, free of charge, to any person obtaining
--  a copy of this software and associated documentation files (the
--  "Software"), to deal in the Software without restriction, including
--  without limitation the rights to use, copy, modify, merge, publish,
--  distribute, sublicense, and/or sell copies of the Software, and to
--  permit persons to whom the Software is furnished to do so, subject
--  to the following conditions:
--
--  The above copyright notice and this permission notice shall be
--  included in all copies or substantial portions of the Software.
--
--  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
--  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
--  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
--  NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
--  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
--  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
--  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
--  THE SOFTWARE.
--
--------------------------------------------------------------------------
*************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


char szDir[]=".";
FILE *fOut_open;
FILE *fOut_stat;
uint64_t t_0;
float freq;

float Get_Freq(void);

unsigned long int rdtscp(void)
{
    unsigned long int rax, rdx;
    asm volatile ( "rdtscp\n" : "=a" (rax), "=d" (rdx) : : );
    return (rdx << 32) + rax;
}

int nData=0;
char szWorkDir[512];

int main(int argc, char *argv[])
{
	int fd, i;
	struct stat file_stat;
	int root_uid, user_uid;
	char szBuff[]="Hello World!", szOutBuff[64], szName[64];
	struct timespec tim1, tim2;
	uint64_t t1, t2, t3, t4;
	double inv_freq, nHour;
	
	if(argc != 3)	{
		printf("Usage: t_open_stat fs_path n_hours\nExample: t_open_stat /scratch/gropu/uid/test 24\n");
		exit(1);
	}
	
	if( stat(argv[1], &file_stat) != 0 )	{
		printf("Error to get the stat of %s\nQuit\n", argv[1]);
		exit(1);
	}
	if( (file_stat.st_mode & S_IFMT) != S_IFDIR )	{
		printf("%s is NOT a directory.\nQuit\n", argv[1]);
		exit(1);
	}
	
	nHour = atof(argv[2]);
	
	tim1.tv_sec = 0;
	tim1.tv_nsec = 200000000;       // sleep 0.2 second. Five files per second.
	nData = (int)(5*3600*nHour);
	
	fOut_open = fopen("t_log_open.txt", "w");
	fOut_stat = fopen("t_log_stat.txt", "w");
	
	freq = Get_Freq();
	inv_freq = 1.0/freq;
	t_0 = rdtscp();
	
	for(i=0; i<nData; i++) {       // close to 48 hours
		sprintf(szName, "%s/test_%d.txt", szWorkDir, i);
		t1 = rdtscp();
		fd = open(szName, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
		t2 = rdtscp();

		sprintf(szOutBuff, "%d\n", i);
		write(fd, szOutBuff, strlen(szOutBuff));
		close(fd);

		t3 = rdtscp();
		stat(szName, &file_stat);
		t4 = rdtscp();

		fprintf(fOut_open, "%9.2lf %8.2lf\n", (double)(t1 - t_0)*inv_freq, (double)(t2-t1)*inv_freq*1000000);
		fprintf(fOut_stat, "%9.2lf %8.2lf\n", (double)(t3 - t_0)*inv_freq, (double)(t4-t3)*inv_freq*1000000);
		if(i % 10000 == 0)      {
			fflush(fOut_open);
			fflush(fOut_stat);
		}
		
		nanosleep(&tim1 , &tim2);
		unlink(szName);
	}
	
	fclose(fOut_open);
	fclose(fOut_stat);

	return 0;
}

#define BUFF_SIZE       (160)
float Get_Freq(void)
{
	int fd, i;
	char szBuff[BUFF_SIZE], *pMax, *p;
	int num_read, nReadItem, uid;
	float freq;
	
	fd = open("/proc/cpuinfo", O_RDONLY, 0);
	if(fd == -1)    return (-1);
	
	num_read = read(fd, szBuff, BUFF_SIZE);
	close(fd);
	
	pMax = szBuff + BUFF_SIZE - 1;
	for(p=szBuff; p<=pMax; p++)     {
		if( (*p) == '@' )       {
			for(i=1; i<16; i++) {
				if(p[i] == 'G') p[i] = 0;
			}
			num_read = sscanf(p+1, "%f", &freq);
			if(num_read == 1) {
				//                      printf("freq = %f GHz\n", freq);
				return (freq*1.0E9);
			}
		}
	}
	
	printf("Error to get CPU frequency.\n");
	
	return 0;
}


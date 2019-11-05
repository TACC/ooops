/*************************************************************************
 * --------------------------------------------------------------------------
 *  --  ooops License
 *  --------------------------------------------------------------------------
 *  --
 *  --  ooops is licensed under the terms of the MIT license reproduced below.
 *  --  This means that ooops is free software and can be used for both academic
 *  --  and commercial purposes at absolutely no cost.
 *  --
 *  --  ----------------------------------------------------------------------
 *  --
 *  --  Copyright (C) 2018-2019 Lei Huang
 *  --
 *  --  Permission is hereby granted, free of charge, to any person obtaining
 *  --  a copy of this software and associated documentation files (the
 *  --  "Software"), to deal in the Software without restriction, including
 *  --  without limitation the rights to use, copy, modify, merge, publish,
 *  --  distribute, sublicense, and/or sell copies of the Software, and to
 *  --  permit persons to whom the Software is furnished to do so, subject
 *  --  to the following conditions:
 *  --
 *  --  The above copyright notice and this permission notice shall be
 *  --  included in all copies or substantial portions of the Software.
 *  --
 *  --  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  --  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *  --  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  --  NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 *  --  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 *  --  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 *  --  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  --  THE SOFTWARE.
 *  --
 *  --------------------------------------------------------------------------
 *  *************************************************************************/

// gcc -O1 -o set_io_param set_io_param.c -lrt

#include <stdio.h>
#include <execinfo.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <malloc.h>

#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <fcntl.h>

#define MAX_FS_SERVER	(8)
#define MAX_LEN_FS_NAME	(16)

uint64_t t_Updated_Param=0;
int UpdateFlag[MAX_FS_SERVER];
float Max_open_Freq[MAX_FS_SERVER];
float Max_lxstat_Freq[MAX_FS_SERVER];
float t_threshold_open[MAX_FS_SERVER];
float t_threshold_lxstat[MAX_FS_SERVER];
uint64_t t_threshold_open_int64[MAX_FS_SERVER];
uint64_t t_threshold_lxstat_int64[MAX_FS_SERVER];

float Max_open_Freq_Default[MAX_FS_SERVER];
float Max_lxstat_Freq_Default[MAX_FS_SERVER];
float t_threshold_open_Default[MAX_FS_SERVER];
float t_threshold_lxstat_Default[MAX_FS_SERVER];

float scale=1.0;
int n_fs_server=0;

uint64_t *p_t_Updated_Param=NULL;
uint64_t *p_t_threshold_open_int64=NULL;
uint64_t *p_t_threshold_lxstat_int64=NULL;
float *p_Max_open_Freq_Share=NULL;
float *p_Max_lxstat_Freq_Share=NULL;
uint64_t *p_Disabled=NULL;
uint64_t *p_Param_Mem_Ready=NULL;
char *p_FS_Tag_List[MAX_FS_SERVER];	// 16 bytes per record
int *p_FS_Tag_Len;

char *szConfigName=NULL;


int shm_param_fd;	// fd for mutex
void *p_param_shm=NULL;	// ptr to shared memory
int nSize_Shared_Param_Data=0;// the number of bytes of shared data

float freq;

/*__attribute__((naked)) unsigned long int rdtscp(void)
{
    __asm {
        rdtscp
        shl rdx,0x20
        add rax,rdx
        ret
    }
}
*/

unsigned long int rdtscp(void)
{
    unsigned long int rax, rdx;
    asm volatile ( "rdtscp\n" : "=a" (rax), "=d" (rdx) : : );
    return (rdx << 32) + rax;
}

void Publish_Parameters(void);
void Report_Parameters(void);
float Get_Freq(void);
void Read_Default_Param(void);
void Print_Default_Param(void);
void Print_Usage_Info(void);

void Print_Usage_Info(void)
{
	printf("Usage:\n");
	printf("1. set_io_param    # Show the status and parameters.\n");
	printf("2. set_io_param 0  # Disable OOOPS\n");
	printf("3. set_io_param 1  # Enable OOOPS.\n");
	printf("4. set_io_param [ server_idx t_open max_open_freq t_stat max_stat_freq ]\n");
	printf("5. set_io_param server_idx [ low / medium / high / unlimit ]\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int nset, i, j, fs_idx, uid, bEnabled;
	char mutex_name[64];

	memset(UpdateFlag, 0, sizeof(int)*MAX_FS_SERVER);

	freq = Get_Freq();
	Read_Default_Param();


	if( ( (argc-1) % 5) == 0 )	{	// [ server_idx, t_open, max_open_freq, t_lxstat, max_stat_freq ]
		nset = (argc-1) / 5;

		for(i=0; i<nset; i++)	{
			j = 1 + i*5;
			fs_idx = atoi(argv[j]);
			UpdateFlag[fs_idx] = 1;
			t_threshold_open[fs_idx] = atof(argv[j+1]);
			Max_open_Freq[fs_idx] =    atof(argv[j+2]);
			t_threshold_lxstat[fs_idx] = atof(argv[j+3]);
			Max_lxstat_Freq[fs_idx] =    atof(argv[j+4]);
		}
	}
	else {
		if(argc == 2)	{
			if( (strcmp(argv[1], "0")==0) || (strcmp(argv[1], "1")==0) )	{
				bEnabled = atoi(argv[1]);
			}
//			else if( (strcmp(argv[1], "-h")==0) || (strcmp(argv[1], "-H")==0) || (strcmp(argv[1], "--help")==0) || (strcmp(argv[1], "--HELP")==0))	{
			else	{
				Print_Usage_Info();
			}
		}
		else if(argc == 3)	{
			if(szConfigName == NULL)	{
				printf("IO_LIMIT_CONFIG is NOT set.\nQuit.\n");
				exit(1);
			}

			fs_idx = atoi(argv[1]);	// get the FS server index
			UpdateFlag[fs_idx] = 1;

			t_threshold_open[fs_idx] = t_threshold_open_Default[fs_idx];
			Max_open_Freq[fs_idx] = Max_open_Freq_Default[fs_idx];
			t_threshold_lxstat[fs_idx] = t_threshold_lxstat_Default[fs_idx];
			Max_lxstat_Freq[fs_idx] = Max_lxstat_Freq_Default[fs_idx];

			if( strcmp(argv[2], "low")==0 )	{
				scale = 0.2;
			}
			else if( strcmp(argv[2], "medium")==0 )	{
				scale = 0.5;
			}
			else if( strcmp(argv[2], "high")==0 )	{
				scale = 1.0;
			}
			else if( strcmp(argv[2], "unlimit")==0 )	{
				t_threshold_open[fs_idx] = 1000000.0;
				t_threshold_lxstat[fs_idx] = 1000000.0;
				scale = 50.0;
			}
			Max_open_Freq[fs_idx] *= scale;
			Max_lxstat_Freq[fs_idx] *= scale;
		}
		else	{
			Print_Usage_Info();
			
			if(argc != 1)	{
				exit(1);
			}
		}
	}
	
	uid = getuid();
	sprintf(mutex_name, "/my_io_param_%d", uid);
	nSize_Shared_Param_Data = sizeof(uint64_t)*(MAX_FS_SERVER * 4 + 2 + 1) + (MAX_LEN_FS_NAME + sizeof(int))*MAX_FS_SERVER;
	shm_param_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_param_fd < 0) {	// failed
		printf("Fail to read %s. Did you run wrapper.so already?\nQuit.\n", mutex_name);
		exit(1);
	}
	if (shm_param_fd == -1) {
		perror("shm_open");
	}
	
	p_param_shm = mmap(NULL, nSize_Shared_Param_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_param_fd, 0);
	if (p_param_shm == MAP_FAILED) {
		perror("mmap");
	}
	
	p_t_Updated_Param = (uint64_t *)p_param_shm;
	p_t_threshold_open_int64 = (uint64_t *)(p_param_shm+8);
	p_t_threshold_lxstat_int64 = (uint64_t *)(p_param_shm+8+8*MAX_FS_SERVER);
	p_Max_open_Freq_Share = (float *)(p_param_shm+8+8*MAX_FS_SERVER*2);
	p_Max_lxstat_Freq_Share = (float *)(p_param_shm+8+8*MAX_FS_SERVER*3);
	p_Disabled = (uint64_t *)(p_param_shm+8+8*MAX_FS_SERVER*4);	
	p_Param_Mem_Ready = (uint64_t *)(p_param_shm+16+8*MAX_FS_SERVER*4);
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_FS_Tag_List[i] = (char *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + i*MAX_LEN_FS_NAME);	// 16 char per record
	}
	p_FS_Tag_Len = (int *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + MAX_FS_SERVER*MAX_LEN_FS_NAME);

	if( argc == 2 )	{
		if(bEnabled)	{
			*p_Disabled = 0;
			*p_t_Updated_Param = rdtscp();	// update time stampe!!!
		}
		else	{
			*p_Disabled = 1;
			*p_t_Updated_Param = rdtscp();	// update time stampe!!!
		}
	}
	else	{
		if(argc !=1)	Publish_Parameters();
	}
	Report_Parameters();

	Print_Default_Param();

	if ( munmap(p_param_shm, nSize_Shared_Param_Data) ) perror("munmap");
	p_param_shm = NULL;
	if (close(shm_param_fd)) perror("close");
	shm_param_fd = 0;


	return 0;
}

void Publish_Parameters(void)	// upload parameters to shared memory area
{
	int i;

	for(i=0; i<MAX_FS_SERVER; i++)	{
		if(UpdateFlag[i])	{
			t_threshold_open[i] = t_threshold_open[i] * 0.000001 * freq;	// tick number
			t_threshold_lxstat[i] = t_threshold_lxstat[i] * 0.000001 * freq;	// tick number
			t_threshold_open_int64[i] = (uint64_t)t_threshold_open[i];
			t_threshold_lxstat_int64[i] = (uint64_t)t_threshold_lxstat[i];

			p_t_threshold_open_int64[i] = t_threshold_open_int64[i] ;
			p_t_threshold_lxstat_int64[i] = t_threshold_lxstat_int64[i];
			p_Max_open_Freq_Share[i] = Max_open_Freq[i];
			p_Max_lxstat_Freq_Share[i] = Max_lxstat_Freq[i];
		}
	}
	*p_Disabled = 0;
	*p_t_Updated_Param = rdtscp();	// time stampe
}

void Report_Parameters(void)
{
	int i;

	if(*p_Disabled == 0)	{
		printf("Limit_IO is ON.\n");
		printf("                            t_open (us) freq_open t_stat (us) freq_stat\n");
		for(i=0; i<MAX_FS_SERVER; i++)	{
			if(p_t_threshold_open_int64[i] != 0)	{
				printf("Server %2d, %-15s : (%8.2lf, %8.2f, %8.2lf, %8.2f)\n", 
					i, p_FS_Tag_List[i], (double)(p_t_threshold_open_int64[i]*1000000.0/freq), p_Max_open_Freq_Share[i], 
					(double)(p_t_threshold_lxstat_int64[i]*1000000.0/freq), p_Max_lxstat_Freq_Share[i]);
			}
		}
	}
	else	{
		printf("Limit_IO is OFF.\n");
	}
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

#define MAX_LEN_CONFIG	(8192)
#define MAX_NUM_CONFIG	(4)
#define N_ITEM_PER_REC	(10)

void Read_Default_Param(void)
{

	int fd, i, Config_Use=-1;
	char szBuff[MAX_LEN_CONFIG], *p, *p_Use;
	int num_read, nReadItem;
	float freq_list[MAX_NUM_CONFIG], f, f_Use=0.0f, f_Sys, df, df_Min=10.0;
	int nConfig=0, PosList[MAX_NUM_CONFIG*2];
	int Tag_Start=0x6572663C, Tag_End=0x72662F3C;	// "<fre" and "</fr"
	char szItems[60][64];	// parameters
	int nParam, Offset;
	char szFSTag[MAX_FS_SERVER][256];

	n_fs_server = 0;
	f_Sys = freq * 0.000000001;	// GHz

	szConfigName=getenv("IO_LIMIT_CONFIG");
	if(szConfigName == NULL)	{
		return;
	}

	fd = open(szConfigName, O_RDONLY, 0);
	if(fd == -1)    {
		printf("Fail to open file: %s\n", szConfigName);
		return;
	}
	
	num_read = read(fd, szBuff, MAX_LEN_CONFIG);
	close(fd);

	p = szBuff;

	i=0;
	while(i<num_read)       {
		if( (*( (int*)(p+i)) == Tag_Start) ||  (*( (int*)(p+i)) == Tag_End)  )  {
			PosList[nConfig] = i;
			nConfig++;
		}
		i++;
	}

	for(i=0; i<nConfig; i+=2)	{	// find the closest frequency. 
		nReadItem = sscanf(szBuff + PosList[i] + 6, "%f", &f);
		if(nReadItem == 1)	{
			freq_list[i>>1] = f;
			df = (f > f_Sys) ? (f - f_Sys) : (f_Sys - f);
			if( df < df_Min)	{
				df_Min = df;
				f_Use = f;
				Config_Use = i;
			}
		}
	}

	p_Use = szBuff + PosList[Config_Use] + 6;
	while(1)	{
		if(*p_Use == 0xA)	{	// new line. Find the beginning of real parameter region
			p_Use = p_Use + 1;
			break;
		}
		p_Use = p_Use + 1;
	}
	szBuff[PosList[Config_Use + 1] - 1] = 0;	// Find the end of parameter region

	nParam = sscanf(p_Use, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", 
		szItems[0], szItems[1], szItems[2], szItems[3], szItems[4], szItems[5], szItems[6], szItems[7], szItems[8], szItems[9], 
		szItems[10], szItems[11], szItems[12], szItems[13], szItems[14], szItems[15], szItems[16], szItems[17], szItems[18], szItems[19], 
		szItems[20], szItems[21], szItems[22], szItems[23], szItems[24], szItems[25], szItems[26], szItems[27], szItems[28], szItems[29], 
		szItems[30], szItems[31], szItems[32], szItems[33], szItems[34], szItems[35], szItems[36], szItems[37], szItems[38], szItems[39]);

	n_fs_server = nParam / N_ITEM_PER_REC;


	for(i=0; i<n_fs_server; i++)	{
		Offset = N_ITEM_PER_REC*i;
		strcpy(szFSTag[i], szItems[Offset+1]);

		t_threshold_open_Default[i] = atof(szItems[Offset+3]);
		Max_open_Freq_Default[i] = atof(szItems[Offset+5]);
		t_threshold_lxstat_Default[i] = atof(szItems[Offset+7]);
		Max_lxstat_Freq_Default[i] = atof(szItems[Offset+9]);
		
//		printf("Server %2d, %-15s : (%8.2lf, %8.2f, %8.2lf, %8.2f)\n", 
//		printf("Server %2d, %-15s : (%8.2lf  %8.2f  %8.2lf  %8.2f )\n", 
//			i, p_FS_Tag_List[i], t_threshold_open_Default[i], Max_open_Freq_Default[i], t_threshold_lxstat_Default[i], Max_lxstat_Freq_Default[i]);
	}

	return;
}

void Print_Default_Param(void)
{
	int i;

	printf("\n---------------------------------------------------------------------\n");
	printf("Default parameters in file %s\n", szConfigName);
	printf("                            t_open (us) freq_open t_stat (us) freq_stat\n");

	for(i=0; i<n_fs_server; i++)	{
		printf("Server %2d, %-15s : (%8.2lf  %8.2f  %8.2lf  %8.2f )\n", 
			i, p_FS_Tag_List[i], t_threshold_open_Default[i], Max_open_Freq_Default[i], t_threshold_lxstat_Default[i], Max_lxstat_Freq_Default[i]);
	}
}


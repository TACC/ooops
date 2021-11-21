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
--  Copyright (C) 2017-2020 Lei Huang
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

// To compile, 
// gcc -O2 -fPIC -shared -o wrapper.so wrapper.c -lrt -ldl

#define __USE_GNU
#define _GNU_SOURCE

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
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>

#include <stdarg.h>

#include <dlfcn.h>
//#include <link.h>

#include <fcntl.h>
#include <linux/limits.h>

#include "hook.h"


#define SHELL_UNKNOWN	(-1)
#define SHELL_BASH	(1)
#define SHELL_TCSH	(2)
#define SHELL_ZSH	(3)

#define PTHREAD_MUTEXATTR_FLAG_PSHARED (0x80000000)	// int 
#define MAX_FS_SERVER	(8)	// max number of distinct file system server to be tracked. 

#define MAX_PATH	(1024)
#define MAX_REC	(512)	// number of record of file IO API call time stampe
#define MAX_REC_M	( (MAX_REC) - 1)
#define max(a,b)	( ( (a) >= (b) ) ? (a) : (b) )

#define MAX_LEN_FS_NAME	(16)

#define MAX_FUNC	(4)	// We are using only two right now for open() and stat().
#define CMDLINE_LEN  (2048)

static int size_pt_mutex_t=0;

static int n_fs_server=0;
static char szFSTag[MAX_FS_SERVER][256];
static int FS_Tag[MAX_FS_SERVER];
static int FS_Tag_Len[MAX_FS_SERVER];
static unsigned long int img_libc_base=0, img_libpthread_base=0;

static unsigned long page_size, filter;

static int iShell_Type=SHELL_UNKNOWN;
static int Inited=0;
static float freq;

static void Read_Config(void);

//static void Get_Exe_Name(int pid, char szName[]);
static void Get_Exe_Name_and_CmdLine(int pid, char szName[]);
static void Take_a_Short_Nap(int nsec);
static ssize_t read_all(int fd, void *buf, size_t count);

//void init_hook();
//void Register_A_Hook(char *module_name, char *Func_Name, void *NewFunc_Addr, long int *ptr_Call_Org);	// so file name, the function you want to intercept, the new function user supply, to store the pointer for calling original function
//void Install_Hook(void);
//void Uninstall_Hook(void);

__thread char szUpdatedDir[2048];

static int Shm_Ready=0;

static char szBatchScriptName_Org[128], szBatchScriptName_New[128];
static int Limit_IO_Debug=0, Is_in_Init_Bash=0;
static struct timespec linux_rate;
__thread int Idx_fs_Server, Idx_fs_Server_CWD;
__thread uint64_t t_list[MAX_FS_SERVER][MAX_FUNC];
static uint64_t CallCount[MAX_FS_SERVER][MAX_FUNC];
static uint64_t nCallOpenDelayed[MAX_FS_SERVER], nCallStatDelayed[MAX_FS_SERVER];



static float Get_Freq(void);
static void Update_CWD(void);
static void Check_FS_Server(char *szName);	// return the index of FS server
static void GetTime(int idx);
static void Update_open_Count(int IdxFunc);

void pre_lxstat(void);
static void post_lxstat(void);

inline unsigned long int rdtscp(void);

typedef int (*org_fchdir)(int fd);
static org_fchdir real_fchdir=NULL;

typedef char * (*org_getcwd)(char *buf, size_t size);
static org_getcwd real_getcwd_ld=NULL, real_getcwd_libc=NULL;

typedef int (*org_open)(const char *pathname, int oflags, ...);
org_open real_open_ld=NULL, real_open_libc=NULL, real_open_pthread=NULL;
org_open real_open64_ld=NULL, real_open64_libc=NULL, real_open64_pthread=NULL;

typedef int (*org_xstat)(int vers, const char *filename, struct stat *buf);
org_xstat real_xstat_ld=NULL, real_xstat_libc=NULL, real_lxstat_ld=NULL, real_lxstat_libc=NULL;

typedef int (*org_chdir)(const char *path);
static org_chdir real_chdir=NULL;

typedef int (*org_fflush)(FILE *stream);
static org_fflush real_fflush=NULL;

typedef int (*org_write)(int fd, const void *buf, size_t count);
static org_write real_write=NULL;

int my_chdir(const char *path)
{
	int ret;
	
	ret = real_chdir(path);
	Update_CWD();
	
	return ret;
}

int my_fchdir(int fd)
{
	int ret;
	
	ret = real_fchdir(fd);
	Update_CWD();
	
	return ret;
}

char *my_getcwd_ld(char *buf, size_t size)
{
	char *szPath=NULL;
	int nLen;

	nLen = strlen(szUpdatedDir);
	if( (nLen >= size) && (size>0) )        {
		errno = ERANGE;
		free(szPath);
		return NULL;
	}

	if( szUpdatedDir[0] != '/' ) {        // not initilized
		szPath = real_getcwd_ld(buf, size);
		if(szPath)	strcpy(szUpdatedDir, szPath);
//		printf("DBG> %s\n", szPath);
		return szPath;
	}

	if(buf == NULL) {
		szPath = malloc( max(size,MAX_PATH) + 1);

		if(szPath == NULL)      {
			printf("Fail to allocate memory for szPath in my_getcwd_ld().\nQuit\n");
			exit(1);
		}
		strcpy(szPath, szUpdatedDir);
//		printf("DBG> %s\n", szPath);
		return szPath;
	}
	else    {
		strcpy(buf, szUpdatedDir);
//		printf("DBG> %s\n", buf);
		return buf;
	}
}

char *my_getcwd_libc(char *buf, size_t size)
{
        char *szPath=NULL;
        int nLen;

        nLen = strlen(szUpdatedDir);
        if( (nLen >= size) && (size>0) )        {
                errno = ERANGE;
                free(szPath);
                return NULL;
        }

        if( szUpdatedDir[0] != '/' ) {        // not initilized
                szPath = real_getcwd_libc(buf, size);
                if(szPath)      strcpy(szUpdatedDir, szPath);
//                printf("DBG> %s\n", szPath);
                return szPath;
        }

        if(buf == NULL) {
                szPath = malloc( max(size,MAX_PATH) + 1);

                if(szPath == NULL)      {
                        printf("Fail to allocate memory for szPath in my_getcwd_libc().\nQuit\n");
                        exit(1);
                }
                strcpy(szPath, szUpdatedDir);
//                printf("DBG> %s\n", szPath);
                return szPath;
        }
        else    {
                strcpy(buf, szUpdatedDir);
//                printf("DBG> %s\n", buf);
                return buf;
        }
}



__thread int tid=-1;

typedef struct {
	double dT_Open_Avg[MAX_FS_SERVER], dT_Stat_Avg[MAX_FS_SERVER];
	double n_Open_Task[MAX_FS_SERVER], n_Stat_Task[MAX_FS_SERVER];	// NOT used any more!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	double dT_To_Sleep_in_Open[MAX_FS_SERVER], dT_To_Sleep_in_Stat[MAX_FS_SERVER];
}DATA_SLEEP, *PDATA_SLEEP;

//start	waper related data and functions
static int Active=1;
static uint64_t t_Updated_Param=0;
static float Max_open_Freq[MAX_FS_SERVER];
static float Max_lxstat_Freq[MAX_FS_SERVER];
static float t_threshold_open[MAX_FS_SERVER];
static float t_threshold_lxstat[MAX_FS_SERVER];
static uint64_t t_threshold_open_int64[MAX_FS_SERVER];
static uint64_t t_threshold_lxstat_int64[MAX_FS_SERVER];

static uint64_t *p_Param_Mem_Ready=NULL;
static uint64_t *p_Disabled=NULL;
static uint64_t *p_t_Updated_Param=NULL;
static uint64_t *p_t_threshold_open_int64=NULL;
static uint64_t *p_t_threshold_lxstat_int64=NULL;

static int *p_tid_Open_List[MAX_FS_SERVER];		// tid list
static int *p_tid_Stat_List[MAX_FS_SERVER];		// tid list
static int shm_dT_fd;
static void *p_dT_shm=NULL;	// ptr to shared memory
static int nSize_Shared_dT_Data=0;
DATA_SLEEP *p_Data_Sleep;

static float *p_param_freq=NULL;	// freq
//static int *p_param_func_addr=NULL;	// beginning address of four functions
//static int *p_param_func_len=NULL;	// lengths of three functions


static char *p_FS_Tag_List[MAX_FS_SERVER];	// 16 bytes per record
static int *p_FS_Tag_Len;
static float *p_Max_open_Freq_Share=NULL;
static float *p_Max_lxstat_Freq_Share=NULL;

static int shm_param_fd;	// fd for mutex
static void *p_param_shm=NULL;	// ptr to shared memory
static int nSize_Shared_Param_Data=0;// the number of bytes of shared data

static void Update_Parameters(void);	// update parameters from share memory area
static void Publish_Parameters(void);	// upload parameters to shared memory area

static struct timespec tim1, tim2;

static void *p_shm=NULL;	// ptr to shared memory
static pthread_mutex_t *p_futex_open=NULL;		// ptr for pthread_mutex_t
static pthread_mutex_t *p_futex_lxstat=NULL;		// ptr for pthread_mutex_t
static int *p_open_Count[MAX_FS_SERVER];
static int *p_lxstat_Count[MAX_FS_SERVER];
static int *p_open_Delayed_Count[MAX_FS_SERVER];
static int *p_lxstat_Delayed_Count[MAX_FS_SERVER];
static struct timeval *p_T_list_open[MAX_FS_SERVER];	// the pointor to list of time stampes
static struct timeval *p_T_list_lxstat[MAX_FS_SERVER];	// the pointor to list of time stampes
static char mutex_name[64];
static int uid;
static int shm_fd;	// fd for mutex
static int nSize_Shared_Data=0;// the number of bytes of shared data

static void Init_Shared_Mutex(void);
static void Close_Shared_Mutex(void);

inline void pre_lxstat(void)
{
	if(Idx_fs_Server < 0) return;	// not interested files. e.g., local disk I/O
	
	t_list[Idx_fs_Server][1] = rdtscp();       // get time now
	
	return;
}

inline unsigned long int rdtscp(void)
{
    unsigned long int rax, rdx;
    asm volatile ( "rdtscp\n" : "=a" (rax), "=d" (rdx) : : );
    return (rdx << 32) + rax;
}

static void Update_CWD(void)	// keep currect working directory updated!!!
{
	int i;
	char *szDir=NULL;
	
	szDir = real_getcwd_libc(szUpdatedDir, MAX_PATH);
	if(szDir == NULL)       {
		printf("Fail to get CWD with get_current_dir_name().\nQuit\n");
		exit(1);
	}
//	else    {
//		strcpy(szUpdatedDir, szDir);
//		free(szDir);
//	}
		
	Idx_fs_Server_CWD = -1;	
	for(i=0; i<n_fs_server; i++) {	// check file name path
		if( strncmp(szUpdatedDir, szFSTag[i], FS_Tag_Len[i]) == 0 ) {
			Idx_fs_Server_CWD = i;
			break;
		}
	}
	//	printf("Idx_fs_Server_CWD = %d\n", Idx_fs_Server_CWD);
}

#define MAX_LEN_CONFIG	(8192)
#define MAX_NUM_CONFIG	(4)
#define N_ITEM_PER_REC	(10)

#pragma GCC push_options
#pragma GCC optimize ("-O0")

static void Read_Config(void)
{
	int fd, i, Config_Use=-1;
	char szBuff[MAX_LEN_CONFIG], *p, *p_Use;
	int num_read, nReadItem;
	float freq_list[MAX_NUM_CONFIG], f, f_Use=0.0f, f_Sys, df, df_Min=10.0;
	char *szConfigName;
	int nConfig=0, PosList[MAX_NUM_CONFIG*2];
	int Tag_Start=0x6572663C, Tag_End=0x72662F3C;	// "<fre" and "</fr"
	char szItems[80][64];	// parameters
	int nParam, Offset;
	char szKey[128]="", szLen[8];
	int NAME_TAG_LEN=16;
	
	n_fs_server = 0;
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		Max_open_Freq[i] = 60000.0;	// set as a large number
		Max_lxstat_Freq[i] = 60000.0;	// set as a large number
	}
	
	szConfigName=getenv("IO_LIMIT_CONFIG");
	if(szConfigName == NULL)	{
		return;
	}
	
	freq = Get_Freq();	// ~260 us
	f_Sys = freq * 0.000000001;	// GHz
	
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
	
	nParam = sscanf(p_Use, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", 
		szItems[0], szItems[1], szItems[2], szItems[3], szItems[4], szItems[5], szItems[6], szItems[7], szItems[8], szItems[9], 
		szItems[10], szItems[11], szItems[12], szItems[13], szItems[14], szItems[15], szItems[16], szItems[17], szItems[18], szItems[19], 
		szItems[20], szItems[21], szItems[22], szItems[23], szItems[24], szItems[25], szItems[26], szItems[27], szItems[28], szItems[29], 
		szItems[30], szItems[31], szItems[32], szItems[33], szItems[34], szItems[35], szItems[36], szItems[37], szItems[38], szItems[39], 
		szItems[40], szItems[41], szItems[42], szItems[43], szItems[44], szItems[45], szItems[46], szItems[47], szItems[48], szItems[49], 
		szItems[50], szItems[51], szItems[52], szItems[53], szItems[54], szItems[55], szItems[56], szItems[57], szItems[58], szItems[59], 
		szItems[60], szItems[61], szItems[62], szItems[63], szItems[64], szItems[65], szItems[66], szItems[67], szItems[68], szItems[69], 
		szItems[70], szItems[71], szItems[72], szItems[73], szItems[74], szItems[75], szItems[76], szItems[77], szItems[78], szItems[79]
		);
	
	n_fs_server = nParam / N_ITEM_PER_REC;
	
	for(i=0; i<n_fs_server; i++)	{
		Offset = N_ITEM_PER_REC*i;
		strcpy(szFSTag[i], szItems[Offset+1]);
		FS_Tag_Len[i] = strlen(szFSTag[i]);
		FS_Tag_Len[i] = (FS_Tag_Len[i] >= MAX_LEN_FS_NAME) ? (MAX_LEN_FS_NAME-1) : (FS_Tag_Len[i]);
		
		t_threshold_open[i] = atof(szItems[Offset+3]);
		Max_open_Freq[i] = atof(szItems[Offset+5]);
		t_threshold_lxstat[i] = atof(szItems[Offset+7]);
		Max_lxstat_Freq[i] = atof(szItems[Offset+9]);
		
		if(Limit_IO_Debug) printf("Server %d: (%6.2f, %5.1f, %6.2f, %5.1f) on %s\n", 
			i, t_threshold_open[i], Max_open_Freq[i], t_threshold_lxstat[i], Max_lxstat_Freq[i], szFSTag[i]);
		
		t_threshold_open[i] = t_threshold_open[i] * 0.000001 * freq;	// tick number
		t_threshold_lxstat[i] = t_threshold_lxstat[i] * 0.000001 * freq;	// tick number
		t_threshold_open_int64[i] = (uint64_t)t_threshold_open[i];
		t_threshold_lxstat_int64[i] = (uint64_t)t_threshold_lxstat[i];
	}
	
	
	return;
}

#pragma GCC pop_options

#define N_SAMPLE_FOR_AVG	(3)	// very good!!!!!!!!!!!!!!!!!!!!!!!

static void post_lxstat(void)
{
	uint64_t dt, t_New;
	int idx_cur, idx_prev, nCall_New;
	long int t_ns_Sleep;
	
	if(t_Updated_Param != *p_t_Updated_Param)	Update_Parameters();
	
	if(Active == 0)	{
		return;
	}
	
	if( (Idx_fs_Server < 0) || (Shm_Ready == 0) ) return;	// not interested files. e.g., local disk I/O
	
	if(tid < 0)	{
		tid = syscall(SYS_gettid);
	}
	
	pthread_mutex_lock(p_futex_lxstat);
	nCall_New = *(p_lxstat_Count[Idx_fs_Server]);
	*(p_lxstat_Count[Idx_fs_Server]) = nCall_New + 1;	// update the counter
    CallCount[Idx_fs_Server][1]++;
	pthread_mutex_unlock(p_futex_lxstat);
	
	t_New = rdtscp();
	dt = t_New - t_list[Idx_fs_Server][1];
	idx_cur = nCall_New & MAX_REC_M;	// fast mod
	gettimeofday(&(p_T_list_lxstat[Idx_fs_Server][idx_cur]), NULL);
	
	if(tid < 0)	{
		tid = syscall(SYS_gettid);
	}
	
	p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server] = 0.0;	// default. No sleep. 
	p_tid_Stat_List[Idx_fs_Server][idx_cur] = tid;
	
	if(nCall_New > N_SAMPLE_FOR_AVG)	{
		idx_prev = (nCall_New - N_SAMPLE_FOR_AVG) & MAX_REC_M;	// fast mod
		p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server] = (N_SAMPLE_FOR_AVG/ Max_lxstat_Freq[Idx_fs_Server]) - ( (p_T_list_lxstat[Idx_fs_Server][idx_cur].tv_sec - p_T_list_lxstat[Idx_fs_Server][idx_prev].tv_sec) + 
			(p_T_list_lxstat[Idx_fs_Server][idx_cur].tv_usec - p_T_list_lxstat[Idx_fs_Server][idx_prev].tv_usec) * 0.000001 );


		if( (dt > t_threshold_lxstat_int64[Idx_fs_Server]) || (p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server] > 1.0E-7) )	{
			t_ns_Sleep = (long int)(1000000000.0*p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server]);
			
			tim1.tv_sec = t_ns_Sleep/1000000000;
			tim1.tv_nsec = t_ns_Sleep % 1000000000;
			
			pthread_mutex_lock(p_futex_lxstat);
			nanosleep(&tim1 , &tim2);	// please note that nanosleep can NOT sleep precisely with given time. Uncertainty is at the order of tens microseconds. 
			nCallStatDelayed[Idx_fs_Server] = nCallStatDelayed[Idx_fs_Server] + 1;
			nCall_New = *(p_lxstat_Delayed_Count[Idx_fs_Server]);
			*(p_lxstat_Delayed_Count[Idx_fs_Server]) = nCall_New + 1;	// update the counter
			pthread_mutex_unlock(p_futex_lxstat);
			gettimeofday(&(p_T_list_lxstat[Idx_fs_Server][idx_cur]), NULL);	// update the time stamp of this delayed operation!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		}
	}
	
	return;
}

static void Update_open_Count(int IdxFunc)
{
	uint64_t dt, t_New;
	int idx_cur, idx_prev, nCall_New;
	long int t_ns_Sleep;
	
	if(t_Updated_Param != *p_t_Updated_Param)	Update_Parameters();
	
	if(Active == 0)	{
		return;
	}
	if(Shm_Ready == 0)	return;
	if(Idx_fs_Server < 0)	return;
	
	pthread_mutex_lock(p_futex_open);
	nCall_New = *(p_open_Count[Idx_fs_Server]);
	*(p_open_Count[Idx_fs_Server]) = nCall_New + 1;	// update the counter
	CallCount[Idx_fs_Server][IdxFunc]++;	
	pthread_mutex_unlock(p_futex_open);
	
	t_New = rdtscp();
	dt = t_New - t_list[Idx_fs_Server][0];
	idx_cur = nCall_New & MAX_REC_M;	// fast mod
	gettimeofday(&(p_T_list_open[Idx_fs_Server][idx_cur]), NULL);

	
	if(tid < 0)	{
		tid = syscall(SYS_gettid);
	}
	
	p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server] = 0.0;
	p_tid_Open_List[Idx_fs_Server][idx_cur] = tid;
	
	if(nCall_New > N_SAMPLE_FOR_AVG)	{
		idx_prev = (nCall_New - N_SAMPLE_FOR_AVG) & MAX_REC_M;	// fast mod

		p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server] = (N_SAMPLE_FOR_AVG/ Max_open_Freq[Idx_fs_Server]) - ( (p_T_list_open[Idx_fs_Server][idx_cur].tv_sec - p_T_list_open[Idx_fs_Server][idx_prev].tv_sec) + 
			(p_T_list_open[Idx_fs_Server][idx_cur].tv_usec - p_T_list_open[Idx_fs_Server][idx_prev].tv_usec) * 0.000001 ) ;

		if( (dt > t_threshold_open_int64[Idx_fs_Server]) || (p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server] > 1.0E-7) )	{
			t_ns_Sleep = (long int)(1000000000.0*p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server]);
			
			tim1.tv_sec = t_ns_Sleep/1000000000;
			tim1.tv_nsec = t_ns_Sleep % 1000000000;

			pthread_mutex_lock(p_futex_open);
			nanosleep(&tim1 , &tim2);	// Sleep() needs to be serialized!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
			nCallOpenDelayed[Idx_fs_Server] = nCallOpenDelayed[Idx_fs_Server] + 1;
			nCall_New = *(p_open_Delayed_Count[Idx_fs_Server]);
			*(p_open_Delayed_Count[Idx_fs_Server]) = nCall_New + 1;	// update the counter
			pthread_mutex_unlock(p_futex_open);
			gettimeofday(&(p_T_list_open[Idx_fs_Server][idx_cur]), NULL);	// update the time stamp of this delayed operation!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
		}
	}
}

static void GetTime(int idx)
{
	int IdxFunc;
	
	if( (Idx_fs_Server < 0) || (n_fs_server < 1) ) return;	// not interested files. e.g., local disk I/O
	
	IdxFunc = idx >> 1;     // the index of function. IdxFunc = idx/2
	if( idx & 0x1 ) {       // even number
		Update_open_Count(IdxFunc);
	}
	else {
        t_list[Idx_fs_Server][IdxFunc] = rdtscp();       // get current time stamp
	}
	
	return;
}

int my_lxstat_ld(int __ver, const char *__filename, struct stat *__stat_buf)
{
	int ret;
	
	//if(Limit_IO_Debug) printf("DBG: lxstat(), file name = %s\n", __filename);
	Check_FS_Server((char*)__filename);
	pre_lxstat();
	ret = real_lxstat_ld(__ver, __filename, __stat_buf);
	post_lxstat();
	
	return ret;
}

int my_lxstat_libc(int __ver, const char *__filename, struct stat *__stat_buf)
{
	int ret;
	
	//if(Limit_IO_Debug) printf("DBG: lxstat(), file name = %s\n", __filename);
	Check_FS_Server((char*)__filename);
	pre_lxstat();
	ret = real_lxstat_libc(__ver, __filename, __stat_buf);
	post_lxstat();
	
	return ret;
}

int my_xstat_ld(int __ver, const char *__filename, struct stat *__stat_buf)
{
	int ret;
	
	//if(Limit_IO_Debug) printf("DBG: lxstat(), file name = %s\n", __filename);
	Check_FS_Server((char*)__filename);
	pre_lxstat();
	ret = real_xstat_ld(__ver, __filename, __stat_buf);
	post_lxstat();
	
	return ret;
}

int my_xstat_libc(int __ver, const char *__filename, struct stat *__stat_buf)
{
	int ret;
	
	//if(Limit_IO_Debug) printf("DBG: lxstat(), file name = %s\n", __filename);
	Check_FS_Server((char*)__filename);
	pre_lxstat();
	ret = real_xstat_libc(__ver, __filename, __stat_buf);
	post_lxstat();
	
	return ret;
}

static void Check_FS_Server(char *szName)	// handle everything needed here for open()!
{
	int i;
	
	Idx_fs_Server = -1;	
	for(i=0; i<n_fs_server; i++) {	// check file name path. absolute path?
		if( strncmp(szName, szFSTag[i], FS_Tag_Len[i]) == 0 ) {
			Idx_fs_Server = i;
			return;
		}
	}
	if(szName[0] != '/')	{	// Using relative path. check current working directory. 
		Idx_fs_Server = Idx_fs_Server_CWD;
	}
}

int my_open_ld(const char *pathname, int oflags, ...)
{
	int mode = 0, two_args=1;
	int ret, *pFileName=(int*)pathname;
	
	if (oflags & O_CREAT)	{
		va_list arg;
		va_start (arg, oflags);
		mode = va_arg (arg, int);
		va_end (arg);
		two_args=0;
	}
//	printf("DBG: my_open_ld(), filename = %s\n", pathname);
			
	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open_ld(pathname, oflags);
	}
	else	{
		ret = real_open_ld(pathname, oflags, mode);
	}
	GetTime(1);
	
	return ret;
}

int my_open_libc(const char *pathname, int oflags, ...)
{
	int mode = 0, two_args=1;
	int ret, *pFileName=(int*)pathname;
	
	if (oflags & O_CREAT)	{
		va_list arg;
		va_start (arg, oflags);
		mode = va_arg (arg, int);
		va_end (arg);
		two_args=0;
	}
//	printf("DBG: my_open_libc(), filename = %s\n", pathname);
	if(Is_in_Init_Bash)	{
		if(strcmp(pathname, szBatchScriptName_Org) == 0)	{
//			printf("Replacing %s with %s\n", szBatchScriptName_Org, szBatchScriptName_New);
			if(two_args)	{
				return real_open_libc(szBatchScriptName_New, oflags);
			}
			else	{
				return real_open_libc(szBatchScriptName_New, oflags, mode);
			}
		}
	}

	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open_libc(pathname, oflags);
	}
	else	{
		ret = real_open_libc(pathname, oflags, mode);
	}
	GetTime(1);
	
	return ret;
}

int my_open_pthread(const char *pathname, int oflags, ...)
{
	int mode = 0, two_args=1;
	int ret, *pFileName=(int*)pathname;
	
	if (oflags & O_CREAT)	{
		va_list arg;
		va_start (arg, oflags);
		mode = va_arg (arg, int);
		va_end (arg);
		two_args=0;
	}
//	printf("DBG: my_open_pthread(), filename = %s\n", pathname);
			
	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open_pthread(pathname, oflags);
	}
	else	{
		ret = real_open_pthread(pathname, oflags, mode);
	}
	GetTime(1);
	
	return ret;
}

int my_open64_ld(const char *pathname, int oflags, ...)
{
	int mode = 0, two_args=1;
	int ret, *pFileName=(int*)pathname;
	
	if (oflags & O_CREAT)	{
		va_list arg;
		va_start (arg, oflags);
		mode = va_arg (arg, int);
		va_end (arg);
		two_args=0;
	}
//	printf("DBG: my_open_ld(), filename = %s\n", pathname);
			
	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open64_ld(pathname, oflags);
	}
	else	{
		ret = real_open64_ld(pathname, oflags, mode);
	}
	GetTime(1);
	
	return ret;
}

int my_open64_libc(const char *pathname, int oflags, ...)
{
	int mode = 0, two_args=1;
	int ret, *pFileName=(int*)pathname;
	
	if (oflags & O_CREAT)	{
		va_list arg;
		va_start (arg, oflags);
		mode = va_arg (arg, int);
		va_end (arg);
		two_args=0;
	}
//	printf("DBG: my_open_libc(), filename = %s\n", pathname);
	if(Is_in_Init_Bash)	{
		if(strcmp(pathname, szBatchScriptName_Org) == 0)	{
//			printf("Replacing %s with %s\n", szBatchScriptName_Org, szBatchScriptName_New);
			if(two_args)	{
				return real_open64_libc(szBatchScriptName_New, oflags);
			}
			else	{
				return real_open64_libc(szBatchScriptName_New, oflags, mode);
			}
		}
	}

	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open64_libc(pathname, oflags);
	}
	else	{
		ret = real_open64_libc(pathname, oflags, mode);
	}
	GetTime(1);
	
	return ret;
}

int my_open64_pthread(const char *pathname, int oflags, ...)
{
	int mode = 0, two_args=1;
	int ret, *pFileName=(int*)pathname;
	
	if (oflags & O_CREAT)	{
		va_list arg;
		va_start (arg, oflags);
		mode = va_arg (arg, int);
		va_end (arg);
		two_args=0;
	}
//	printf("DBG: my_open_pthread(), filename = %s\n", pathname);
			
	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open64_pthread(pathname, oflags);
	}
	else	{
		ret = real_open64_pthread(pathname, oflags, mode);
	}
	GetTime(1);
	
	return ret;
}


/*
static void Print_Summary(void)
{
	int i;
	
	for(i=0; i<n_fs_server; i++)	{
		fprintf(stderr, "FS %-10s open_count = %7lld delayed_open_count = %7lld Accum_open_count = %7d lxstat_count = %7lld delayed_lxstat_count = %7lld Accum_lxstat_count = %7d\n",
			szFSTag[i], CallCount[i][0], nCallOpenDelayed[i], *(p_open_Count[i]), CallCount[i][1], nCallStatDelayed[i], *(p_lxstat_Count[i]));
	}
}
*/
/*
int my_fflush(FILE *stream)
{
	if(Is_in_Init_Bash)	{
		if(stream == stderr)	{
			Print_Summary();

			return real_fflush(stream);
		}
	}
	return real_fflush(stream);
}
*/

static long int nWriteCount=0;
ssize_t my_write(int fd, const void *buf, size_t count)
{
	if(iShell_Type == SHELL_TCSH)	{	// special handler for csh/tcsh
		nWriteCount++;
		if(nWriteCount == 1)	{	// running ooopsd at the background
			if(strncmp(buf, "[1]", 3)==0)	{
				return count;	// no real print here. Suppress output
			}
			else	{
				return real_write(fd, buf, count);
			}
		}
		else	{
			if( strncmp(buf, "[1]  + Done", 11) == 0)	{	// ooopsd is quiting. 
				if(strstr(buf, "ooopsd"))	{
					return count;
				}
				else	{
					return real_write(fd, buf, count);
				}
			}
			else	{
				return real_write(fd, buf, count);
			}
		}
	}
	else	{
		return real_write(fd, buf, count);
	}
}

static __attribute__((constructor)) void init()
{
	char szLogName[256], szEnv[256], *szPath, szCodeName[256], szBuff[256], *szDBG, szExeName[4096], *szDir, szHostName[256];
	int i=0, pid;

	gethostname(szHostName, 255);
	while(szHostName[i] != 0)	{
		if(szHostName[i] == '.')	{
			szHostName[i] = 0;	// truncate hostname[]
			break;
		}
		i++;
	}
	if( (szHostName[0] != 'c') || (szHostName[4] != '-') )	{	// NOT a compute node
		return;
	}
	
	size_pt_mutex_t = sizeof(pthread_mutex_t);
	memset(CallCount, 0, sizeof(uint64_t)*MAX_FUNC*MAX_FS_SERVER);
	
	//	freq = Get_Freq();	// ~260 us
	uid = getuid();	// ~ 10 us
	pid = getpid();

	szDBG = getenv("LIMIT_IO_DEBUG");
	if(szDBG == NULL)	{
		Limit_IO_Debug = 0;
	}
	else	{
		Limit_IO_Debug = atoi(szDBG);
	}	
	
	Get_Exe_Name_and_CmdLine(pid, szExeName);
	if(strcmp(szExeName, "hostname")==0) {
		return;
	}
	else if(strcmp(szExeName, "ooopsd")==0) {	// DO NOT SLOW DOWN the IO in ooopsd!!!
		return;
	}
        else if(strcmp(szExeName, "ssh")==0) {
                return;
        }



	szDir = getcwd(szUpdatedDir, 1024);
	if(szDir == NULL)       {
		printf("Fail to get CWD with get_current_dir_name().\nQuit\n");
		exit(1);
	}
//	else    {
//		strcpy(szUpdatedDir, szDir);
//		free(szDir);
//	}
	
	page_size = sysconf(_SC_PAGESIZE);
	filter = ~(page_size - 1);
	
	Init_Shared_Mutex();	// shared memory are ready! ~470 us.
	
//	Update_CWD();
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		nCallOpenDelayed[i] = 0;
		nCallStatDelayed[i] = 0;
	}
	
	int *p_mutex_attr;
	pthread_mutexattr_t mattr;
	p_mutex_attr = (int *)(&mattr);
	*p_mutex_attr = PTHREAD_MUTEXATTR_FLAG_PSHARED;	// PTHREAD_PROCESS_SHARED !!!!!!!!!!!!!!! Shared between processes
	pthread_mutex_init(p_futex_open, &mattr);
	pthread_mutex_init(p_futex_lxstat, &mattr);
	
	Shm_Ready = 1;
	Inited = 1;

//	init_hook();
	register_a_hook("ld", "open", (void*)my_open_ld, (long int *)(&real_open_ld));
	register_a_hook("libc", "open", (void*)my_open_libc, (long int *)(&real_open_libc));
	register_a_hook("libpthread", "open", (void*)my_open_pthread, (long int *)(&real_open_pthread));

	register_a_hook("ld", "open64", (void*)my_open_ld, (long int *)(&real_open_ld));
	register_a_hook("libc", "open64", (void*)my_open_libc, (long int *)(&real_open_libc));
	register_a_hook("libpthread", "open64", (void*)my_open_pthread, (long int *)(&real_open_pthread));

	
	register_a_hook("ld", "__xstat64", (void*)my_xstat_ld, (long int *)(&real_xstat_ld));
	register_a_hook("libc", "__xstat64", (void*)my_xstat_libc, (long int *)(&real_xstat_libc));

	register_a_hook("ld", "__lxstat64", (void*)my_lxstat_ld, (long int *)(&real_lxstat_ld));
	register_a_hook("libc", "__lxstat64", (void*)my_lxstat_libc, (long int *)(&real_lxstat_libc));

	register_a_hook("libc", "chdir", (void*)my_chdir, (long int *)(&real_chdir));
	register_a_hook("libc", "fchdir", (void*)my_fchdir, (long int *)(&real_fchdir));

	register_a_hook("ld", "getcwd", (void*)my_getcwd_ld, (long int *)(&real_getcwd_ld));
	register_a_hook("libc", "getcwd", (void*)my_getcwd_libc, (long int *)(&real_getcwd_libc));

//	if(Is_in_Init_Bash)	Register_A_Hook("libc", "fflush", (void*)my_fflush, (long int *)(&real_fflush));
	if(iShell_Type == SHELL_TCSH)	register_a_hook("libc", "__write", (void*)my_write, (long int *)(&real_write));
	install_hook();

	Update_CWD();
}

static __attribute__((destructor)) void finalize()
{
	int i;
	
    if(! Inited) return;
//	fprintf(stdout, "DBG> In finalize, n_fs_server = %d Limit_IO_Debug = %d\n", n_fs_server, Limit_IO_Debug);
	if(Limit_IO_Debug)	{
		for(i=0; i<n_fs_server; i++)	{
			fprintf(stdout, "FS %-10s open_count = %7lld delayed_open_count = %7lld Accum_open_count = %7d lxstat_count = %7lld delayed_lxstat_count = %7lld Accum_lxstat_count = %7d\n",
				szFSTag[i], CallCount[i][0], nCallOpenDelayed[i], *(p_open_Count[i]), CallCount[i][1], nCallStatDelayed[i], *(p_lxstat_Count[i]));
		}
	    fflush(stdout);
	}
	
    Close_Shared_Mutex();
	uninstall_hook();
}

#define BUFF_SIZE       (160)
static float Get_Freq(void)
{
        int i;
        FILE *fIn;
        char szLine[BUFF_SIZE], *ReadLine;
        int num_read, nReadItem, uid;
        float freq;

        fIn = fopen("/proc/cpuinfo", "r");
        if(fIn == NULL)    return (-1);

        while(1)        {
                ReadLine = fgets(szLine, BUFF_SIZE-1, fIn);
                if(ReadLine == NULL)    {
                        break;
                }
                if(feof(fIn))   {
                        break;
                }
                if(strncmp(szLine, "cpu MHz", 7)==0)    {
//                        printf("%s\n", szLine);
                        num_read = sscanf(szLine+10, "%f", &freq);
                        if(num_read == 1) {
//                                printf("freq = %f MHz\n", freq);
                                fclose(fIn);
                                return (freq*1.0E6);
                        }
                }
        }

        fclose(fIn);

  printf("Error to get CPU frequency.\n");

  return 0;
}


static void Init_Shared_Mutex(void)
{
	int To_Init=0;
	uint64_t i;
	
	sprintf(mutex_name, "/my_mutex_%d", uid);
	
	nSize_Shared_Data = size_pt_mutex_t*2 + 8*MAX_FS_SERVER*4 + sizeof(struct timeval)*MAX_REC*MAX_FS_SERVER * 2;
	
	shm_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_fd < 0) {	// failed
		shm_fd = shm_open(mutex_name, O_RDWR | O_CREAT | O_EXCL, 0664);	// create 
		
		if(shm_fd == -1)	{
			if(errno == EEXIST)	{	// file exists
				shm_fd = shm_open(mutex_name, O_RDWR, 0664); // try openning file again
				if(shm_fd == -1)    {
					printf("Fail to create file with shm_open().\n");
					exit(1);
				}
			}
			else	{
				printf("DBG> Unexpected error in Init_Shared_Mutex()!\nerrno = %d\n\n", errno);
			}
			Take_a_Short_Nap(300);
		}
		else {
			if (ftruncate(shm_fd, nSize_Shared_Data) != 0) {
				perror("ftruncate");
			}
		}
	}
	else
		Take_a_Short_Nap(400);
	
	// Map mutex into the shared memory.
	p_shm = mmap(NULL, nSize_Shared_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	//  p_shm = mmap(NULL, nSize_Shared_Data, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, shm_fd, 0);
	if (p_shm == MAP_FAILED) {
		perror("mmap");
	}
	
	p_futex_open = (pthread_mutex_t *)p_shm;
	p_futex_lxstat = (pthread_mutex_t *)(p_shm+size_pt_mutex_t);
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_open_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i);
		p_open_Delayed_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8);
		p_lxstat_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8*2);
		p_lxstat_Delayed_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8*3);
	}
//	printf("DBG> p_open_Count[0] = %d\n", *(p_open_Count[0]));
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_T_list_open[i] = (struct timeval *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*MAX_FS_SERVER*4 + sizeof(struct timeval)*MAX_REC*i);
		p_T_list_lxstat[i] = (struct timeval *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*MAX_FS_SERVER*4 + sizeof(struct timeval)*MAX_REC*(i + MAX_FS_SERVER) );
	}
		
	// start share memory for dT and tid
	sprintf(mutex_name, "/my_dt_%d", uid);
	nSize_Shared_dT_Data = sizeof(int)*MAX_REC*2*MAX_FS_SERVER + sizeof(DATA_SLEEP);	// dT and tid
	
	shm_dT_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_dT_fd < 0) {	// failed
		shm_dT_fd = shm_open(mutex_name, O_RDWR | O_CREAT | O_EXCL, 0664);	// create 
		
		if(shm_dT_fd == -1)    {	// failed to create
			Take_a_Short_Nap(300);
			shm_dT_fd = shm_open(mutex_name, O_RDWR, 0664);
			if(shm_dT_fd == -1)    {
				printf("Fail to create file, shm_dT_fd, with shm_open().\n");
				exit(1);
			}
		}
		else {
			if (ftruncate(shm_dT_fd, nSize_Shared_dT_Data) != 0) {
				perror("ftruncate");
			}
		}
	}
	else
		Take_a_Short_Nap(400);
	
	p_dT_shm = mmap(NULL, nSize_Shared_dT_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_dT_fd, 0);
	if (p_dT_shm == MAP_FAILED) {
		perror("mmap");
	}
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_tid_Open_List[i] = (int *)(p_dT_shm + sizeof(int)*MAX_REC*i);
	}
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_tid_Stat_List[i] = (int *)(p_dT_shm + sizeof(int)*MAX_REC*(i+MAX_FS_SERVER));
	}
	p_Data_Sleep = (DATA_SLEEP *)(p_dT_shm + sizeof(int)*MAX_REC*2*MAX_FS_SERVER);
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_Data_Sleep->dT_Open_Avg[i] = 1.0E6;
		p_Data_Sleep->dT_Stat_Avg[i] = 1.0E6;
		p_Data_Sleep->dT_To_Sleep_in_Open[i] = 0.0;
		p_Data_Sleep->dT_To_Sleep_in_Stat[i] = 0.0;
	}
	
	sprintf(mutex_name, "/my_io_param_%d", uid);
	nSize_Shared_Param_Data = sizeof(uint64_t)*(MAX_FS_SERVER * 4 + 2 + 1) + (MAX_LEN_FS_NAME + sizeof(int))*MAX_FS_SERVER; // p_t_Updated_Param, p_Disabled, p_Param_Mem_Ready
	nSize_Shared_Param_Data += (sizeof(float) + sizeof(int)*7);	// freq (float), func_addr[4], func_len[3]
	
	shm_param_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_param_fd < 0) {	// failed
		shm_param_fd = shm_open(mutex_name, O_RDWR | O_CREAT | O_EXCL, 0664);	// create 
		
		if(shm_param_fd == -1)    {	// failed to create
			Take_a_Short_Nap(300);
			shm_param_fd = shm_open(mutex_name, O_RDWR, 0664);
			if(shm_param_fd == -1)    {
				printf("Fail to create file, shm_param_fd, with shm_open().\n");
				exit(1);
			}
		}
		else {
			if (ftruncate(shm_param_fd, nSize_Shared_Param_Data) != 0) {
				perror("ftruncate");
			}
			To_Init = 1;
		}
	}
	else
		Take_a_Short_Nap(400);
	
	
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
	
	p_param_freq = (float *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + MAX_FS_SERVER*MAX_LEN_FS_NAME + MAX_FS_SERVER*4);
//	p_param_func_addr = (int *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + MAX_FS_SERVER*MAX_LEN_FS_NAME + MAX_FS_SERVER*4 + 4);	// int[4]
//	p_param_func_len = (int *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + MAX_FS_SERVER*MAX_LEN_FS_NAME + MAX_FS_SERVER*4 + 4 + 4*4);	// int[3]
	
	if(To_Init)	{
//		Find_Func_Addr();
		Read_Config();
		Publish_Parameters();
	}
	else	{
		while( *p_Param_Mem_Ready == 0)	{	// not ready yet
			Take_a_Short_Nap(200);
		}
		Update_Parameters();
	}
}

static void Close_Shared_Mutex(void)
{
	if( (unsigned long int)p_shm > 0x800000 ) {	// special case for vim 
		if ( munmap(p_shm, nSize_Shared_Data) ) {
			perror("munmap");
		}
	}
	p_shm = NULL;
	if (close(shm_fd)) {
//		perror("close");
	}
	shm_fd = 0;	
	
	if ( munmap(p_dT_shm, nSize_Shared_dT_Data) ) {
		perror("munmap");
	}
	p_dT_shm = NULL;
	if (close(shm_dT_fd)) {
//		perror("close");
	}
	shm_dT_fd = 0;
	
	if ( munmap(p_param_shm, nSize_Shared_Param_Data) ) {
		perror("munmap");
	}
	p_param_shm = NULL;
	if (close(shm_param_fd)) {
//		perror("close");
	}
	shm_param_fd = 0;
}

static void Update_Parameters(void)	// update parameters from share memory area
{
	int i;
	
	if(*p_Disabled == 1)	{	// turned off
		Active = 0;
		return;
	}
	else	{
		Active = 1;
	}
	
	n_fs_server = 0;
	
	t_Updated_Param = *p_t_Updated_Param;
	
	freq = *p_param_freq;
//	for(i=0; i<3; i++)	{
//		func_addr[i] = p_param_func_addr[i];
//		func_len[i] = p_param_func_len[i];
//	}
//	query_open_addr = (long int)(img_libc_base + ( (long int)(func_addr[0]) & 0xFFFFFFFF) );
	
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		if(p_FS_Tag_Len[i] == 0)	{
			continue;
		}
		
		t_threshold_open_int64[i] = p_t_threshold_open_int64[i];
		t_threshold_lxstat_int64[i] = p_t_threshold_lxstat_int64[i];
		Max_open_Freq[i] = p_Max_open_Freq_Share[i];
		Max_lxstat_Freq[i] = p_Max_lxstat_Freq_Share[i];
		
		strcpy(szFSTag[i], p_FS_Tag_List[i]);
		FS_Tag_Len[i] = p_FS_Tag_Len[i];	// I have made sure p_FS_Tag_Len[] is within our limit!
		
		if(Limit_IO_Debug)	printf("Server %d, %-16s : (%8.2lf, %8.2f, %8.2lf, %8.2f)\n", 
			i, szFSTag[i], (double)(p_t_threshold_open_int64[i]*1000000.0/freq), Max_open_Freq[i], 
			(double)(p_t_threshold_lxstat_int64[i]*1000000.0/freq), Max_lxstat_Freq[i]);
		n_fs_server++;
	}
}

static void Publish_Parameters(void)	// upload parameters to shared memory area
{
	int i;
	
	*p_t_Updated_Param = rdtscp();	// time stampe
	*p_Disabled = 0;	// limit_io is ON.
	Active = 1;
	
	for(i=0; i<n_fs_server; i++)	{
		p_FS_Tag_Len[i] = 0;
	}
	
	for(i=0; i<n_fs_server; i++)	{
		p_t_threshold_open_int64[i] = t_threshold_open_int64[i] ;
		p_t_threshold_lxstat_int64[i] = t_threshold_lxstat_int64[i];
		p_Max_open_Freq_Share[i] = Max_open_Freq[i];
		p_Max_lxstat_Freq_Share[i] = Max_lxstat_Freq[i];
		
		p_FS_Tag_Len[i] = FS_Tag_Len[i];
		
		strncpy(p_FS_Tag_List[i], szFSTag[i], FS_Tag_Len[i]);
		p_FS_Tag_List[i][FS_Tag_Len[i]] = 0;	// string termination
	}
	
	*p_param_freq = freq;
//	for(i=0; i<3; i++)	{
//		p_param_func_addr[i] = func_addr[i];
//		p_param_func_len[i] = func_len[i];
//	}
//	p_param_func_addr[3] = func_addr[3];
	
	*p_Param_Mem_Ready = 1;
}

static void Take_a_Short_Nap(int nsec)
{
    tim1.tv_sec = 0;
    tim1.tv_nsec = nsec;
    nanosleep(&tim1, &tim2);
}
/*
static void Get_Exe_Name(int pid, char szName[])
{
	FILE *fIn;
	char szPath[1024], *ReadLine;
	
	sprintf(szPath, "/proc/%d/cmdline", pid);
	fIn = fopen(szPath, "r");
	if(fIn == NULL)	{
		printf("Fail to open file: %s\nQuit\n", szPath);
		szName[0] = 0;
		return;
		//		exit(0);
	}
	
	ReadLine = fgets(szName, 1024, fIn);
	fclose(fIn);
	
	if(ReadLine == NULL)	{
		printf("Fail to determine the executable file name.\nQuit\n");
		exit(0);
	}
}
*/

#define MAX_BUFF_SIZE	(1024*1024)
static void Append_Job_End_Handler_Bash(void)
{
	FILE *fIn, *fOut;
	char szFile[MAX_BUFF_SIZE], szCmdPrepend[8192];
	int nBytes, nBytes_Written;

	fIn = fopen(szBatchScriptName_Org, "r");
	if(fIn == NULL)	{
		printf("Fail to open file %s\nQuit\n", szBatchScriptName_Org);
		exit(1);
	}
	nBytes = fread(szFile, 1, MAX_BUFF_SIZE, fIn);
	fclose(fIn);

	if(nBytes < 10)	{
		printf("Something wrong in file %s\n%s\n", szBatchScriptName_Org, szFile);
		exit(1);
	}
	else if(nBytes == MAX_BUFF_SIZE)	{
		printf("File %s is large. You need to increase MAX_BUFF_SIZE\n", szBatchScriptName_Org);
		exit(1);
	}

	fOut = fopen(szBatchScriptName_New, "w");
	if(fOut == NULL)	{
		printf("Fail to open file %s\nQuit\n", szBatchScriptName_New);
		exit(1);
	}
	sprintf(szCmdPrepend, "scontrol show hostnames $SLURM_NODELIST > /dev/shm/ooops/hostfile\n");
	
	strcat(szCmdPrepend, "myhostname=`hostname -s`\n");
	strcat(szCmdPrepend, "myip=`$TACC_OOOPS_BIN/get_ip`\n");

	strcat(szCmdPrepend, "$TACC_OOOPS_BIN/ooopsd -p 8888 &\n");
	strcat(szCmdPrepend, "export ooopsd_pid=$!\n");
	strcat(szCmdPrepend, "$TACC_OOOPS_BIN/msleep 100\n");

	strcat(szCmdPrepend, "while IFS= read -r line\n");
	strcat(szCmdPrepend, "do\n");
	strcat(szCmdPrepend, "  if [ \"$line\" != \"$myhostname\" ]; then\n");
//	strcat(szCmdPrepend, "    ssh -n $line \"/work/00410/huang/project/git/socket_aggregator/client $myip &> /dev/null &\"\n");
	strcat(szCmdPrepend, "    ssh -q -o \"StrictHostKeyChecking no\" -n $line \"IO_LIMIT_CONFIG=$IO_LIMIT_CONFIG LD_PRELOAD=$LD_PRELOAD date &> /dev/null ; LD_PRELOAD=\\\"\\\" OOOPS_REPORT_T_INTERVAL=$OOOPS_REPORT_T_INTERVAL $TACC_OOOPS_BIN/ooops $myip &> /dev/null &\" &> /dev/null &\n");
	strcat(szCmdPrepend, "  fi\n");
	strcat(szCmdPrepend, "done < \"/dev/shm/ooops/hostfile\"\n");

	nBytes_Written = fwrite(szCmdPrepend, 1, strlen(szCmdPrepend), fOut);
	nBytes_Written = fwrite(szFile, 1, nBytes, fOut);
	if(nBytes_Written != nBytes)	{
		printf("Error in writing %s. nBytes_Written = %d nBytes = %d\n", szBatchScriptName_New, nBytes_Written, nBytes);
		fclose(fOut);
		exit(1);
	}

//	fprintf(fOut, "sleep $OOOPS_REPORT_T_INTERVAL\n");
	fprintf(fOut, "kill $ooopsd_pid\n");
	fprintf(fOut, "$TACC_OOOPS_BIN/msleep 200\n");

	fprintf(fOut, "if [ -f \"/dev/shm/ooops/high_io\" ]; then \n");
	fprintf(fOut, "  ml intel impi &> /dev/null \n");
	fprintf(fOut, "  mpiexec.hydra -np $SLURM_NNODES -ppn 1 -f /dev/shm/ooops/hostfile $TACC_OOOPS_BIN/mpi_aggregator\n");
	fprintf(fOut, "fi\n");

	fclose(fOut);

//	sleep(120);
}

static void Append_Job_End_Handler_Tcsh(void)
{
	FILE *fIn, *fOut;
	char szFile[MAX_BUFF_SIZE], szCmdPrepend[8192];
	int nBytes, nBytes_Written;

	fIn = fopen(szBatchScriptName_Org, "r");
	if(fIn == NULL)	{
		printf("Fail to open file %s\nQuit\n", szBatchScriptName_Org);
		exit(1);
	}
	nBytes = fread(szFile, 1, MAX_BUFF_SIZE, fIn);
	fclose(fIn);

	if(nBytes < 10)	{
		printf("Something wrong in file %s\n%s\n", szBatchScriptName_Org, szFile);
		exit(1);
	}
	else if(nBytes == MAX_BUFF_SIZE)	{
		printf("File %s is large. You need to increase MAX_BUFF_SIZE\n", szBatchScriptName_Org);
		exit(1);
	}

	fOut = fopen(szBatchScriptName_New, "w");
	if(fOut == NULL)	{
		printf("Fail to open file %s\nQuit\n", szBatchScriptName_New);
		exit(1);
	}
	sprintf(szCmdPrepend, "scontrol show hostnames \"$SLURM_NODELIST\" > /dev/shm/ooops/hostfile\n");
	
	strcat(szCmdPrepend, "set myhostname=`hostname -s`\n");
	strcat(szCmdPrepend, "set myip=`$TACC_OOOPS_BIN/get_ip`\n");

	strcat(szCmdPrepend, "$TACC_OOOPS_BIN/ooopsd -p 8888 &\n");
	strcat(szCmdPrepend, "set export ooopsd_pid=$!\n");
	strcat(szCmdPrepend, "$TACC_OOOPS_BIN/msleep 100\n");


	strcat(szCmdPrepend, "foreach line ( `cat /dev/shm/ooops/hostfile`)\n");
	strcat(szCmdPrepend, "  if ($line != \"$myhostname\") then\n");
	strcat(szCmdPrepend, "   ( ssh -q -o \"StrictHostKeyChecking no\" -n $line \"env IO_LIMIT_CONFIG=$IO_LIMIT_CONFIG LD_PRELOAD=$LD_PRELOAD date > /dev/null ; env LD_PRELOAD=\"\" OOOPS_REPORT_T_INTERVAL=$OOOPS_REPORT_T_INTERVAL $TACC_OOOPS_BIN/ooops $myip > /dev/null &\" > /dev/null & ) \n");
	strcat(szCmdPrepend, "  endif\n");
	strcat(szCmdPrepend, "end\n");

	nBytes_Written = fwrite(szCmdPrepend, 1, strlen(szCmdPrepend), fOut);
	nBytes_Written = fwrite(szFile, 1, nBytes, fOut);
	if(nBytes_Written != nBytes)	{
		printf("Error in writing %s. nBytes_Written = %d nBytes = %d\n", szBatchScriptName_New, nBytes_Written, nBytes);
		fclose(fOut);
		exit(1);
	}

	fprintf(fOut, "kill $ooopsd_pid\n");
	fprintf(fOut, "$TACC_OOOPS_BIN/msleep 200\n");
	fprintf(fOut, "if ( -f \"/dev/shm/ooops/high_io\" ) then\n");
	fprintf(fOut, "  ml intel impi > /dev/null \n");
	fprintf(fOut, "  mpiexec.hydra -np $SLURM_NNODES -ppn 1 -f /dev/shm/ooops/hostfile $TACC_OOOPS_BIN/mpi_aggregator\n");
	fprintf(fOut, "endif\n");

	fclose(fOut);
}

static void Append_Job_End_Handler_Zsh(void)
{
	FILE *fIn, *fOut;
	char szFile[MAX_BUFF_SIZE], szCmdPrepend[8192];
	int nBytes, nBytes_Written;

	fIn = fopen(szBatchScriptName_Org, "r");
	if(fIn == NULL)	{
		printf("Fail to open file %s\nQuit\n", szBatchScriptName_Org);
		exit(1);
	}
	nBytes = fread(szFile, 1, MAX_BUFF_SIZE, fIn);
	fclose(fIn);

	if(nBytes < 10)	{
		printf("Something wrong in file %s\n%s\n", szBatchScriptName_Org, szFile);
		exit(1);
	}
	else if(nBytes == MAX_BUFF_SIZE)	{
		printf("File %s is large. You need to increase MAX_BUFF_SIZE\n", szBatchScriptName_Org);
		exit(1);
	}

	fOut = fopen(szBatchScriptName_New, "w");
	if(fOut == NULL)	{
		printf("Fail to open file %s\nQuit\n", szBatchScriptName_New);
		exit(1);
	}
	sprintf(szCmdPrepend, "scontrol show hostnames $SLURM_NODELIST > /dev/shm/ooops/hostfile\n");
	
	strcat(szCmdPrepend, "myhostname=`hostname -s`\n");
	strcat(szCmdPrepend, "myip=`$TACC_OOOPS_BIN/get_ip`\n");

	strcat(szCmdPrepend, "$TACC_OOOPS_BIN/ooopsd -p 8888 &\n");
	strcat(szCmdPrepend, "export ooopsd_pid=$!\n");
	strcat(szCmdPrepend, "$TACC_OOOPS_BIN/msleep 100\n");

	strcat(szCmdPrepend, "for line in `cat /dev/shm/ooops/hostfile`\n");
	strcat(szCmdPrepend, "do\n");
	strcat(szCmdPrepend, "  if [ \"$line\" != \"$myhostname\" ]; then\n");
	strcat(szCmdPrepend, "    ssh -q -o \"StrictHostKeyChecking no\" -n $line \"IO_LIMIT_CONFIG=$IO_LIMIT_CONFIG LD_PRELOAD=$LD_PRELOAD date &> /dev/null ; LD_PRELOAD=\\\"\\\" OOOPS_REPORT_T_INTERVAL=$OOOPS_REPORT_T_INTERVAL $TACC_OOOPS_BIN/ooops $myip &> /dev/null &\" &> /dev/null &\n");
	strcat(szCmdPrepend, "  fi\n");
	strcat(szCmdPrepend, "done\n");

	nBytes_Written = fwrite(szCmdPrepend, 1, strlen(szCmdPrepend), fOut);
	nBytes_Written = fwrite(szFile, 1, nBytes, fOut);
	if(nBytes_Written != nBytes)	{
		printf("Error in writing %s. nBytes_Written = %d nBytes = %d\n", szBatchScriptName_New, nBytes_Written, nBytes);
		fclose(fOut);
		exit(1);
	}

	fprintf(fOut, "kill $ooopsd_pid\n");
	fprintf(fOut, "$TACC_OOOPS_BIN/msleep 200\n");
	fprintf(fOut, "if [ -f \"/dev/shm/ooops/high_io\" ]; then \n");
	fprintf(fOut, "  ml intel impi &> /dev/null \n");
	fprintf(fOut, "  mpiexec.hydra -np $SLURM_NNODES -ppn 1 -f /dev/shm/ooops/hostfile $TACC_OOOPS_BIN/mpi_aggregator\n");
	fprintf(fOut, "fi\n");

	fclose(fOut);
}


static void Get_Exe_Name_and_CmdLine(int pid, char szName[])
{
	FILE *fIn;
	char szPath[64], *ReadLine, szLine[512], *p_szExeName;
	int nLen, i=0, j, nLen_szExeName, iLastPos=0;
	
	szName[0] = 0;
	szBatchScriptName_Org[0] = 0;
	szBatchScriptName_New[0] = 0;

	sprintf(szPath, "/proc/%d/cmdline", pid);
	fIn = fopen(szPath, "r");
	if(fIn == NULL)	{
		printf("Fail to open file: %s\nQuit\n", szPath);
		exit(1);
	}
	
	nLen = fread(szLine, 1, 511, fIn);
	fclose(fIn);
	
	if(nLen <= 0)	{
		printf("Fail to determine the executable file name.\nQuit\n");
		exit(1);
	}
	nLen_szExeName = strlen(szLine);

	p_szExeName = szLine;
	if(szLine[0] == '/')	{	// absolute path
		for(i=nLen_szExeName-1; i>=0; i--)	{
			if(szLine[i] == '/')	{
				p_szExeName = szLine + i + 1;
				break;
			}
		}
		strcpy(szName, p_szExeName);
	}
	else	{
		strcpy(szName, szLine);
	}
	if( strcmp(p_szExeName, "bash")==0 )	{
		iShell_Type = SHELL_BASH;
	}
	else if( strcmp(p_szExeName, "csh")==0 )	{
		iShell_Type = SHELL_TCSH;
	}
	else if( strcmp(p_szExeName, "tcsh")==0 )	{
		iShell_Type = SHELL_TCSH;
	}
	else if( strcmp(p_szExeName, "zsh")==0 )	{
		iShell_Type = SHELL_ZSH;
	}
	else	{
		iShell_Type = -1;
	}

	if( iShell_Type >= SHELL_BASH )	{
		i = nLen_szExeName;
		while(i<nLen)	{
			if(szLine[i] == 0)	{
//				printf("DBG> Found %s %s.\n", p_szExeName, szLine+i+1);
				if(strncmp(szLine+i+1,"/tmp/slurmd/job", 15)==0)	{
//					Limit_IO_Debug = 1;	// force debug one. Print summary. 
					Is_in_Init_Bash = 1;
					strcpy(szBatchScriptName_Org, szLine+i+1);
					for(j=0; j<128; j++)	{
						if(szBatchScriptName_Org[j] == 0)	{
							break;
						}
						else if(szBatchScriptName_Org[j] == '/')	{
							iLastPos = j;
						}
					}
					if(szBatchScriptName_Org[iLastPos] == '/')	{
						if(mkdir("/dev/shm/ooops", S_IRWXU) == -1)	{	// error
							fprintf(stderr, "Fail to create dir /dev/shm/ooops\nQuit\n");
							exit(1);
						}
						sprintf(szBatchScriptName_New, "/dev/shm/ooops/%s", szBatchScriptName_Org+iLastPos+1);
						if(iShell_Type == SHELL_BASH)	{
							Append_Job_End_Handler_Bash();
						}
						else if(iShell_Type == SHELL_TCSH)	{
							Append_Job_End_Handler_Tcsh();
						}
						else if(iShell_Type == SHELL_ZSH)	{
							Append_Job_End_Handler_Zsh();
						}
					}
//					printf("DBG> Found %s.\n", p_szExeName);
				}
				break;
			}
			i++;
		}
	}

}

static ssize_t read_all(int fd, void *buf, size_t count)
{
	ssize_t ret, nBytes=0;

	while (count != 0 && (ret = read(fd, buf, count)) != 0) {
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			perror ("read");
			break;
		}
		nBytes += ret;
		count -= ret;
		buf += ret;
	}
	return nBytes;
}


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
--  Copyright (C) 2017-2019 Lei Huang
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
#include <elf.h>

#include <stdarg.h>

#define __USE_GNU
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>

#include <fcntl.h>

#define PTHREAD_MUTEXATTR_FLAG_PSHARED (0x80000000)	// int 
#define MAX_FS_SERVER	(8)	// max number of distinct file system server to be tracked. 

#define MAX_REC	(512)	// number of record of file IO API call time stampe
#define MAX_REC_M	( (MAX_REC) - 1)
#define max(a,b)	( ( (a) >= (b) ) ? (a) : (b) )

#define MAX_LEN_FS_NAME	(16)

#define MAX_FUNC	(4)	// We are using only two right now for open() and stat().
#define MAX_PATCH	(16)	// the max number of patches will be applied in glibc
#define CMDLINE_LEN  (2048)

static int size_pt_mutex_t=0;

static int n_fs_server=0;
static char szFSTag[MAX_FS_SERVER][256];
static int FS_Tag[MAX_FS_SERVER];
static int FS_Tag_Len[MAX_FS_SERVER];
static unsigned long int img_libc_base=0, img_libpthread_base=0;

static unsigned long page_size, filter;

static int Inited=0;
static float freq;


static int IsLibpthreadLoaded=0;
static char szPathLibc[128]="";
static char szPathLibpthread[128];
static int func_addr[4], func_len[3];
static long int query_open_addr=0;

typedef struct{
	long int base_addr, patch_addr;
	int org_value;
}PATCH_REC;

static int nPatch=0;	// the number of patches in glibc
PATCH_REC PatchList[MAX_PATCH];

static void Read_Config(void);

static void Get_Exe_Name(int pid, char szName[]);
static void Uninstall_Patches(void);
static void Take_a_Short_Nap(int nsec);

__thread char szUpdatedDir[2048];

static int Shm_Ready=0;

static int Limit_IO_Debug=0;
static struct timespec linux_rate;
__thread int Idx_fs_Server, Idx_fs_Server_CWD;
__thread uint64_t t_list[MAX_FS_SERVER][MAX_FUNC];
static uint64_t CallCount[MAX_FS_SERVER][MAX_FUNC];
static uint64_t nCallOpenDelayed[MAX_FS_SERVER], nCallStatDelayed[MAX_FS_SERVER];


struct elt {
    int next, value, key;
};

struct dict {
    int size;           /* size of the pointer table */
    int n;              /* number of elements stored */
};

typedef struct dict *Dict;

#define HASH_TABLE_SIZE	(512)
static void DictCreate(Dict d, int nSize, struct elt ** p_elt_list, int ** p_ht_table);
static void DictInsert(Dict d, int key, const int value, struct elt ** p_elt_list, int ** p_ht_table);
static int DictSearch(Dict d, int key, struct elt ** p_elt_list, int ** p_ht_table);

// sizeof(struct dict) + sizeof(int)*(d->size) + sizeof(struct elt)*(d->size)

static int hashint(int a)
{
    a = a ^ (a>>4);
    a = (a^0xdeadbeef) + (a<<5);
    a = a ^ (a>>11);
    return a;
}

void Init_Pointers(Dict d, struct elt ** p_elt_list, int ** p_ht_table)
{
    *p_ht_table = (int *)((void *)d + sizeof(struct dict));
	*p_elt_list = (struct elt *)((void *)d + sizeof(struct dict) + sizeof(int)*(d->size));
}

static void DictCreate(Dict d, int nSize, struct elt ** p_elt_list, int ** p_ht_table)
{
	int i;
	
	if(d == NULL)	{
		printf("d = NULL.\nThe memory for hash table is not allocated.\nQuit\n");
		exit(1);
	}
	
	if(nSize) {
		if(nSize) {
			d->size = nSize;
		}
		d->n = 0;
	}
	Init_Pointers(d, p_elt_list, p_ht_table);
	
	if(nSize) for(i = 0; i < d->size; i++) (*p_ht_table)[i] = -1;
}

// insert a new key-value pair into an existing dictionary 
static void DictInsert(Dict d, int key, const int value, struct elt ** p_elt_list, int ** p_ht_table)
{
    struct elt *e;
    int h;
	
	e = &( (*p_elt_list)[d->n]);
	e->key = key;
    e->value = value;
	
    h = hashint(key) % HASH_TABLE_SIZE;
	
    e->next = (*p_ht_table)[h];
    (*p_ht_table)[h] = d->n;
    d->n++;
}

static int DictSearch(Dict d, int key, struct elt ** p_elt_list, int ** p_ht_table)
{
	int idx;
	struct elt *e;
	
	if(d->n == 0) return (-1);
	
	idx = (*p_ht_table)[hashint(key) % HASH_TABLE_SIZE];
	if(idx == -1)	{
		return (-1);
	}
	
	e = &( (*p_elt_list)[idx] );
    while(1) {
        if(e->key == key) {
            return idx;
        }
		else	{
			idx = e->next;
			if(idx == -1)	{	// end
				return (-1);
			}
			e = &( (*p_elt_list)[idx] );
		}
    }
	
    return -1;
}


static void Find_Func_Addr(void);
static float Get_Freq(void);
static void Update_CWD(void);
static void Check_FS_Server(char *szName);	// return the index of FS server
static void GetTime(int idx);
static void Update_open_Count(int IdxFunc);

inline void pre_lxstat(void);
static void post_lxstat(void);

inline unsigned long int rdtscp(void);

typedef int (*org_chdir)(const char *path);
static org_chdir real_chdir=NULL;

typedef int (*org_open)(const char *pathname, int oflags,...);
static org_open real_open=NULL;

int chdir(const char *path)
{
	int ret;
	
	if(real_chdir)	{
		ret = real_chdir(path);
	}
	else	{
		real_chdir = (org_chdir)dlsym(RTLD_NEXT, "chdir");
		ret = real_chdir(path);
	}
	Update_CWD();
	
	return ret;
}

__thread int tid=-1;

typedef struct {
	double dT_Open_Avg[MAX_FS_SERVER], dT_Stat_Avg[MAX_FS_SERVER];
	double n_Open_Task[MAX_FS_SERVER], n_Stat_Task[MAX_FS_SERVER];
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
static int *p_param_func_addr=NULL;	// beginning address of four functions
static int *p_param_func_len=NULL;	// lengths of three functions


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
static int64_t *p_dT_list_open[MAX_FS_SERVER];	// the pointor to list of time stampes
static int64_t *p_dT_list_lxstat[MAX_FS_SERVER];	// the pointor to list of time stampes
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
	
	if(getcwd(szUpdatedDir, 512) == NULL) {
		printf("The size of szUpdatedDir needs to be increased.\nQuit\n");
	}
	
	Idx_fs_Server_CWD = -1;	
	for(i=0; i<n_fs_server; i++) {	// check file name path
		if( strncmp(szUpdatedDir, szFSTag[i], FS_Tag_Len[i]) == 0 ) {
			Idx_fs_Server_CWD = i;
			break;
		}
	}
	//	printf("Idx_fs_Server_CWD = %d\n", Idx_fs_Server_CWD);
}

static void Patch_Function_Call(void *func_addr, int size, int func_dist, int offset)
{
	unsigned char *pbase, *p;
	int i, p_addr_call, *p_int;
	
	pbase = (unsigned char *)( ( (unsigned long)func_addr ) & filter );	// fast mod
	
	if(mprotect(pbase, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)	{	// two pages to make sure the code works when the modified code is around page boundary
		printf("Error in executing mprotect().\n");
		exit(1);
	}
	
	p = (unsigned char *)func_addr;
	for(i=0; i<size; i++)	{
		if(p[i] == 0xE8)	{
			p_int = (int *)(p + i + 1);
			p_addr_call = ( *p_int & 0xFFFFFFFF) + i + 5;
			if(p_addr_call == func_dist)	{
				PatchList[nPatch].base_addr = (long int)pbase;
				PatchList[nPatch].patch_addr = (long int)p_int;
				PatchList[nPatch].org_value = *p_int;
				nPatch++;	// ????????????????????
				
				*p_int = *p_int + offset;
				//	printf("Modified libc at: %p\n", p+i);
			}
			
		}
	}
	
	if(mprotect(pbase, 0x2000, PROT_READ | PROT_EXEC) != 0)	{	// two pages to make sure the code works when the modified code is around page boundary
		printf("Error in executing mprotect().\n");
		exit(1);
	}
}

static void Patch_open_calls_in_libc(void)
{
	int offset;
	long int p_addr_call, addr_open, dist;
	
	addr_open = query_open_addr;
	
	if(IsLibpthreadLoaded)	{	// ~37 us
		void *module;
		module = dlopen(szPathLibpthread, RTLD_LAZY);
		if(module == NULL)	{
			printf("Fail to dlopen: %s.\n", szPathLibpthread);
			return;
		}
		real_open = (org_open)dlsym(module, "open64");	// open64 in libpthread will be called if applicable.
		dlclose(module);
	}
	
	dist = abs(addr_open - (long int)open);
	if(dist >= 0x80000000)	{
		printf("The distance between wrapper.so and libc.so is too far! Limit_IO will NOT work for C++ code.\n");
		//		exit(1);
	}
	else {
		offset = (int)((long int)open - addr_open);
		
		Patch_Function_Call( (void *)(addr_open + func_addr[1] - func_addr[0]), func_len[1], func_addr[0] - func_addr[1], offset);
		Patch_Function_Call( (void *)(addr_open + func_addr[2] - func_addr[0]), func_len[2], func_addr[0] - func_addr[2], offset);
	}
	
}

static int callback(struct dl_phdr_info *info, size_t size, void *data)
{
	if(strstr(info->dlpi_name, "/libc.so"))	{
		strcpy(szPathLibc, info->dlpi_name);
		//		printf("Host: %s, %s\n", szHostName, szPathLibc);
		img_libc_base = (info->dlpi_addr + info->dlpi_phdr[0].p_vaddr) & filter;
	}
	else if(strstr(info->dlpi_name, "/libpthread.so"))	{
		strcpy(szPathLibpthread, info->dlpi_name);
		IsLibpthreadLoaded = 1;
		img_libpthread_base = (info->dlpi_addr + info->dlpi_phdr[0].p_vaddr) & filter;
		//		printf("Host: %s, %s\n", szHostName, szPathLibpthread);
	}
	
	return 0;
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
	char szItems[60][64];	// parameters
	int nParam, Offset;
	char szKey[128]="", szLen[8];
	int NAME_TAG_LEN=16;
	long int lKey=0;
	char szHostNameShort[64], szHostName[128];
	
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
	
	nParam = sscanf(p_Use, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", 
		szItems[0], szItems[1], szItems[2], szItems[3], szItems[4], szItems[5], szItems[6], szItems[7], szItems[8], szItems[9], 
		szItems[10], szItems[11], szItems[12], szItems[13], szItems[14], szItems[15], szItems[16], szItems[17], szItems[18], szItems[19], 
		szItems[20], szItems[21], szItems[22], szItems[23], szItems[24], szItems[25], szItems[26], szItems[27], szItems[28], szItems[29], 
		szItems[30], szItems[31], szItems[32], szItems[33], szItems[34], szItems[35], szItems[36], szItems[37], szItems[38], szItems[39]);
	
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

static void Find_Func_Addr(void)
{
	int fd, i, count=0;
	struct stat file_stat;
	void *map_start;
	Elf64_Sym *symtab;
	Elf64_Ehdr *header;
	Elf64_Shdr *sections;
	int strtab_offset=0;
	void *pSymbBase=NULL;
	int nSym=0, SymRecSize=0, SymOffset, RecAddr;
	char *szSymName;
	char szFunc_List[3][24]={"open64", "_IO_file_open", "_IO_file_fopen"};
	
	stat(szPathLibc, &file_stat);
	//	printf("Size = %zu\n", file_stat.st_size);
	
	fd = open(szPathLibc, O_RDONLY);
	map_start = mmap(0, file_stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
	header = (Elf64_Ehdr *) map_start;
	
	sections = (Elf64_Shdr *)((char *)map_start + header->e_shoff);
	
	for (i = 0; i < header->e_shnum; i++)	{
		if ( (sections[i].sh_type == SHT_STRTAB) ) {
			strtab_offset = (int)(sections[i].sh_offset);
			break;
		}
	}
	
	for (i = 0; i < header->e_shnum; i++)	{
		if ( (sections[i].sh_type == SHT_DYNSYM) || (sections[i].sh_type == SHT_SYMTAB) ) {
			pSymbBase = (void*)(sections[i].sh_offset + map_start);
			SymRecSize = sections[i].sh_entsize;
			nSym = sections[i].sh_size / sections[i].sh_entsize;
			break;
		}
	}
	
	for(i=0; i<nSym; i++) {
		RecAddr = SymRecSize*i;
//		SymOffset = *( (int *)( pSymbBase + RecAddr ) ) & 0xFFFF;
		SymOffset = *( (int *)( pSymbBase + RecAddr ) ) & 0xFFFFFFFF;
		szSymName = (char *)( map_start + strtab_offset + SymOffset );
		
		if( strcmp(szSymName, szFunc_List[0])==0 )	{	// "open64"
			func_addr[0] = *((int *)(pSymbBase + RecAddr + 8));
			func_len[0] = *((int *)(pSymbBase + RecAddr + 16));
			count++;
			query_open_addr = (long int)(img_libc_base + ( (long int)(func_addr[0]) & 0xFFFFFFFF) );
		}
		else if( strcmp(szSymName, szFunc_List[1])==0 )	{	// "_IO_file_open"
			func_addr[1] = *((int *)(pSymbBase + RecAddr + 8));
			func_len[1] = *((int *)(pSymbBase + RecAddr + 16));
			count++;
		}
		else if( strcmp(szSymName, szFunc_List[2])==0 )	{	// "_IO_file_fopen"
			func_addr[2] = *((int *)(pSymbBase + RecAddr + 8));
			func_len[2] = *((int *)(pSymbBase + RecAddr + 16));
			count++;
		}
		
		if(count == 3)	{
			break;
		}
	}
	munmap(map_start, file_stat.st_size);
	close(fd);
}

#define N_SAMPLE_FOR_AVG	(255)

static double Cal_Avg_dT(uint64_t *p_dT, int Idx)
{
	double Mean = 0.0;
	uint64_t Sum = 0, Mean_Est;
	int i;
	
	for(i=Idx-N_SAMPLE_FOR_AVG; i<=Idx; i++)	{
		Sum += p_dT[i];
	}
	Mean_Est = Sum >> 8;	// same as Sum/2^8 = Sum / 256
	
	return ((double)(1.0*Mean_Est));
}

static double Cal_Avg_N_IO_TASK(int *p_tid, int Idx)
{
	char *p_Buff;
	Dict p_Hash;
	struct elt *elt_list;
	int *ht_table=NULL;
	int i, nBytes_Hash_Table, idx_rec, nTasks=0, nTaskMin;
	char szExeName[256];
	
	nBytes_Hash_Table = sizeof(struct dict) + sizeof(int)*HASH_TABLE_SIZE + sizeof(struct elt)*HASH_TABLE_SIZE;
	p_Buff = malloc(nBytes_Hash_Table);
	
	p_Hash = (struct dict *)p_Buff;
	DictCreate(p_Hash, HASH_TABLE_SIZE, &elt_list, &ht_table);	// init hash table
	
	for(i=Idx-N_SAMPLE_FOR_AVG; i<=Idx; i++)	{
		idx_rec = DictSearch(p_Hash, p_tid[i], &elt_list, &ht_table);
		
		if( idx_rec >= 0 )	{	// existed
			elt_list[idx_rec].value += 1;
		}
		else	{	// new key
			DictInsert(p_Hash, p_tid[i], 1, &elt_list, &ht_table);
		}
	}
	
	nTaskMin = (int)(0.8*(N_SAMPLE_FOR_AVG+1)/p_Hash->size);
	
    for(i = 0; i < p_Hash->n; i++) {
        if(elt_list[i].value > nTaskMin) {
            nTasks++;
        }
    }
	
	free(p_Buff);
	
	return ((double)(nTasks));
}

static void post_lxstat(void)
{
	uint64_t dt, t_New;
	int idx_cur, nCall_New;
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
    CallCount[Idx_fs_Server][1]++;
	nCall_New = *(p_lxstat_Count[Idx_fs_Server]);
	*(p_lxstat_Count[Idx_fs_Server]) = nCall_New + 1;	// update the counter
	pthread_mutex_unlock(p_futex_lxstat);
	
	t_New = rdtscp();
	dt = t_New - t_list[Idx_fs_Server][1];
	
	//	printf("In stat(), p_lxstat_Count = %lld\n", *(p_lxstat_Count[Idx_fs_Server]));
	
	idx_cur = nCall_New & MAX_REC_M;	// fast mod
	
	if(tid < 0)	{
		tid = syscall(SYS_gettid);
	}
	
	p_tid_Stat_List[Idx_fs_Server][idx_cur] = tid;
	p_dT_list_lxstat[Idx_fs_Server][idx_cur] = dt;	// fill in current time stampe. Not used any more!!!
	
	if( (idx_cur == N_SAMPLE_FOR_AVG) || (idx_cur == MAX_REC_M) )	{	// update dT_Avg, t_Sleep and n_IO_TASK
		p_Data_Sleep->dT_Stat_Avg[Idx_fs_Server] = Cal_Avg_dT(p_dT_list_lxstat[Idx_fs_Server], idx_cur);
		p_Data_Sleep->n_Stat_Task[Idx_fs_Server] = Cal_Avg_N_IO_TASK(p_tid_Stat_List[Idx_fs_Server], idx_cur);
		p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server] = (p_Data_Sleep->n_Stat_Task[Idx_fs_Server] / Max_lxstat_Freq[Idx_fs_Server]) - (p_Data_Sleep->dT_Stat_Avg[Idx_fs_Server] / freq);
		//		printf("Stat(): %d %lf %4.1lf %lf\n", *(p_lxstat_Count[Idx_fs_Server]), p_Data_Sleep->dT_Stat_Avg[Idx_fs_Server], p_Data_Sleep->n_Stat_Task[Idx_fs_Server], p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server]);
	}
	
	if( (dt > t_threshold_lxstat_int64[Idx_fs_Server]) || (p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server] > 1.0E-7) )	{
		t_ns_Sleep = (long int)(1000000000.0*p_Data_Sleep->dT_To_Sleep_in_Stat[Idx_fs_Server]);
		
		tim1.tv_sec = t_ns_Sleep/1000000000;
		tim1.tv_nsec = t_ns_Sleep % 1000000000;
		nanosleep(&tim1 , &tim2);	// please note that nanosleep can NOT sleep precisely with given time. Uncertainty is at the order of tens microseconds. 

		nCallStatDelayed[Idx_fs_Server] = nCallStatDelayed[Idx_fs_Server] + 1;
	}
	return;
}

static void Update_open_Count(int IdxFunc)
{
	uint64_t dt, t_New;
	int idx_cur, nCall_New;
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
	
	if(tid < 0)	{
		tid = syscall(SYS_gettid);
	}
	
	p_tid_Open_List[Idx_fs_Server][idx_cur] = tid;
	p_dT_list_open[Idx_fs_Server][idx_cur] = dt;	// fill in current time stampe. Not used any more!!!
	
	if( (idx_cur == N_SAMPLE_FOR_AVG) || (idx_cur == MAX_REC_M) )	{	// update dT_Avg, t_Sleep and n_IO_TASK
		p_Data_Sleep->dT_Open_Avg[Idx_fs_Server] = Cal_Avg_dT(p_dT_list_open[Idx_fs_Server], idx_cur);
		p_Data_Sleep->n_Open_Task[Idx_fs_Server] = Cal_Avg_N_IO_TASK(p_tid_Open_List[Idx_fs_Server], idx_cur);
		p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server] = (p_Data_Sleep->n_Open_Task[Idx_fs_Server] / Max_open_Freq[Idx_fs_Server]) - (p_Data_Sleep->dT_Open_Avg[Idx_fs_Server] / freq);
		//		printf("Open(): %d %lf %4.1lf %lf\n", *(p_open_Count[Idx_fs_Server]), p_Data_Sleep->dT_Open_Avg[Idx_fs_Server], p_Data_Sleep->n_Open_Task[Idx_fs_Server], p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server]);
	}
	
	if( (dt > t_threshold_open_int64[Idx_fs_Server]) || (p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server] > 1.0E-7) )	{
		t_ns_Sleep = (long int)(1000000000.0*p_Data_Sleep->dT_To_Sleep_in_Open[Idx_fs_Server]);
		
		tim1.tv_sec = t_ns_Sleep/1000000000;
		tim1.tv_nsec = t_ns_Sleep % 1000000000;
		nanosleep(&tim1 , &tim2);
		nCallOpenDelayed[Idx_fs_Server] = nCallOpenDelayed[Idx_fs_Server] + 1;
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

typedef int (*org_lxstat)(int __ver, const char *__filename, struct stat *__stat_buf);
static org_lxstat real_lxstat=NULL;

int lxstat(int __ver, const char *__filename, struct stat *__stat_buf)
{
	int ret;
	
	//        if(Limit_IO_Debug) printf("DBG: lxstat(), file name = %s\n", __filename);
	if(real_lxstat==NULL)	{
		real_lxstat = (org_lxstat)dlsym(RTLD_NEXT, "__lxstat");
	}
	
	if(Inited == 0) {       // init() not finished yet
		ret = real_lxstat(__ver, __filename, __stat_buf);
		return ret;
	}
	
	Check_FS_Server((char*)__filename);
	
	pre_lxstat();
	ret = real_lxstat(__ver, __filename, __stat_buf);
	if(Shm_Ready) post_lxstat();
	
	return ret;
}
extern int __lxstat(int __ver, const char *__filename, struct stat *__stat_buf) __attribute__ ( (alias ("lxstat")) );
extern int __lxstat64(int __ver, const char *__filename, struct stat *__stat_buf) __attribute__ ( (alias ("lxstat")) );

typedef int (*org_xstat)(int __ver, const char *__filename, struct stat *__stat_buf);
static org_xstat real_xstat=NULL;

int xstat(int __ver, const char *__filename, struct stat *__stat_buf)
{
	int ret;
	
	//        if(Limit_IO_Debug) printf("DBG: xstat(), file name = %s\n", __filename);
	if(real_xstat==NULL)  {
		real_xstat = (org_lxstat)dlsym(RTLD_NEXT, "__xstat");
	}
	
	if(Inited == 0) {       // init() not finished yet
		ret = real_xstat(__ver, __filename, __stat_buf);
		return ret;
	}
	
	
	Check_FS_Server((char*)__filename);
	pre_lxstat();
	ret = real_xstat(__ver, __filename, __stat_buf);
	if(Shm_Ready) post_lxstat();
	
	return ret;
}
extern int __xstat(int __ver, const char *__filename, struct stat *__stat_buf) __attribute__ ( (alias ("xstat")) );
extern int __xstat64(int __ver, const char *__filename, struct stat *__stat_buf) __attribute__ ( (alias ("xstat")) );

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

int open(const char *pathname, int oflags, ...)
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
	//	printf("DBG: open(), filename = %s\n", pathname);
	
	if(real_open == NULL)	{
		real_open = (org_open)dlsym(RTLD_NEXT, "open");
	}
	
	if(Inited == 0) {	// init() not finished yet
		if(two_args)    {
			ret = real_open(pathname, oflags);
		}
		else    {
			ret = real_open(pathname, oflags, mode);
		}
		return ret;
	}
	
	
	Check_FS_Server((char*)pathname);
	GetTime(0);
	
	if(two_args)	{
		ret = real_open(pathname, oflags);
	}
	else	{
		ret = real_open(pathname, oflags, mode);
	}
	if(Shm_Ready) GetTime(1);
	
	return ret;
}
extern int __open(const char *pathname, int oflags, ...) __attribute__ ( (alias ("open")) );
extern int open64(const char *pathname, int oflags, ...) __attribute__ ( (alias ("open")) );
extern int __open64(const char *pathname, int oflags, ...) __attribute__ ( (alias ("open")) );

static __attribute__((constructor)) void init()
{
	char szLogName[256], szEnv[256], *szPath, szCodeName[256], szBuff[256], *szDBG, szExeName[512];
	int i, pid;
	
	size_pt_mutex_t = sizeof(pthread_mutex_t);
	memset(CallCount, 0, sizeof(uint64_t)*MAX_FUNC*MAX_FS_SERVER);
	
	//	freq = Get_Freq();	// ~260 us
	uid = getuid();	// ~ 10 us
	pid = getpid();
	Get_Exe_Name(pid, szExeName);
	if(strcmp(szExeName, "ssh")==0) {
		return;
	}
	
	if(getcwd(szUpdatedDir, 512) == NULL) {
		printf("The size of szUpdatedDir needs to be increased.\nQuit\n");
	}
	
	szDBG = getenv("LIMIT_IO_DEBUG");
	if(szDBG == NULL)	{
		Limit_IO_Debug = 0;
	}
	else	{
		Limit_IO_Debug = atoi(szDBG);
	}	
	
	page_size = sysconf(_SC_PAGESIZE);
	filter = ~(page_size - 1);
	
	dl_iterate_phdr(callback, NULL);	// get library full path. ~44 us
	Init_Shared_Mutex();	// shared memory are ready! ~470 us.
	
	Update_CWD();
	
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
	
    Patch_open_calls_in_libc();
	
	Shm_Ready = 1;
	Inited = 1;
}

static __attribute__((destructor)) void finalize()
{
	int i;
	
    if(! Inited) return;
	
	if(Limit_IO_Debug)	{
		for(i=0; i<n_fs_server; i++)	{
			printf("FS %-10s open_count = %6lld delayed_open_count = %6lld Accum_open_count = %6d lxstat_count = %6lld delayed_lxstat_count = %6lld Accum_lxstat_count = %6d\n",
				szFSTag[i], CallCount[i][0], nCallOpenDelayed[i], *(p_open_Count[i]), CallCount[i][1], nCallStatDelayed[i], *(p_lxstat_Count[i]));
		}
	}
	
    fflush(stdout);
	
    Close_Shared_Mutex();
    Uninstall_Patches();
}

#define BUFF_SIZE       (160)
static float Get_Freq(void)
{
	int fd, i;
	char szBuff[BUFF_SIZE], *pMax, *p;
	int num_read, nReadItem;
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


static void Init_Shared_Mutex(void)
{
	int To_Init=0;
	uint64_t i;
	
	sprintf(mutex_name, "/my_mutex_%d", uid);
	
	nSize_Shared_Data = size_pt_mutex_t*2 + 8*MAX_FS_SERVER*2 + sizeof(uint64_t)*MAX_REC*MAX_FS_SERVER * 2;
	
	shm_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_fd < 0) {	// failed
		shm_fd = shm_open(mutex_name, O_RDWR | O_CREAT | O_EXCL, 0664);	// create 
		
		if(shm_fd == -1)	{
			Take_a_Short_Nap(300);
			shm_fd = shm_open(mutex_name, O_RDWR, 0664); // try openning file again
			if(shm_fd == -1)    {
				//           printf("On %s, Fail to create file with shm_open().\n", szHostName);
				printf("Fail to create file with shm_open().\n");
				exit(1);
			}
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
		p_lxstat_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8);
	}
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_dT_list_open[i] = (uint64_t *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*MAX_FS_SERVER*2 + sizeof(uint64_t)*MAX_REC*i);
		p_dT_list_lxstat[i] = (uint64_t *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*MAX_FS_SERVER*2 + sizeof(uint64_t)*MAX_REC*(i + MAX_FS_SERVER) );
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
		p_Data_Sleep->n_Open_Task[i] = 1;
		p_Data_Sleep->n_Stat_Task[i] = 1;
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
	p_param_func_addr = (int *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + MAX_FS_SERVER*MAX_LEN_FS_NAME + MAX_FS_SERVER*4 + 4);	// int[4]
	p_param_func_len = (int *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + MAX_FS_SERVER*MAX_LEN_FS_NAME + MAX_FS_SERVER*4 + 4 + 4*4);	// int[3]
	
	if(To_Init)	{
		Find_Func_Addr();
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
		perror("close");
	}
	shm_fd = 0;	
	
	if ( munmap(p_dT_shm, nSize_Shared_dT_Data) ) {
		perror("munmap");
	}
	p_dT_shm = NULL;
	if (close(shm_dT_fd)) {
		perror("close");
	}
	shm_dT_fd = 0;
	
	if ( munmap(p_param_shm, nSize_Shared_Param_Data) ) {
		perror("munmap");
	}
	p_param_shm = NULL;
	if (close(shm_param_fd)) {
		perror("close");
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
	for(i=0; i<3; i++)	{
		func_addr[i] = p_param_func_addr[i];
		func_len[i] = p_param_func_len[i];
	}
	query_open_addr = (long int)(img_libc_base + ( (long int)(func_addr[0]) & 0xFFFFFFFF) );
	
	
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
	for(i=0; i<3; i++)	{
		p_param_func_addr[i] = func_addr[i];
		p_param_func_len[i] = func_len[i];
	}
	p_param_func_addr[3] = func_addr[3];
	
	*p_Param_Mem_Ready = 1;
}

static void Uninstall_Patches(void)
{
	int i;
	
	for(i=0; i<nPatch; i++)	{
		if(mprotect((void*)PatchList[i].base_addr, 0x2000, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)	{	// two pages to make sure the code works when the modified code is around page boundary
			printf("Error in executing mprotect().\n");
			exit(1);
		}
		*( (int*)(PatchList[i].patch_addr) ) = PatchList[i].org_value;	// restore orginal data
		if(mprotect((void*)PatchList[i].base_addr, 0x2000, PROT_READ | PROT_EXEC) != 0)	{	// two pages to make sure the code works when the modified code is around page boundary
			printf("Error in executing mprotect().\n");
			exit(1);
		}
	}
}

static void Take_a_Short_Nap(int nsec)
{
    tim1.tv_sec = 0;
    tim1.tv_nsec = nsec;
    nanosleep(&tim1, &tim2);
}

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

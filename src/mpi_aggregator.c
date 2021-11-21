#include <mpi.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
//#include <time.h>
#include <sys/stat.h>

#define MAX_FS_SERVER	(8)	// max number of distinct file system server to be tracked. 
#define MAX_REC	(512)	// number of record of file IO API call time stampe
#define MAX_LEN_FS_NAME	(16)

/*
void msleep(void)
{
	int nMillSecond=100;
	struct timespec tim1, tim2;
	
	tim1.tv_sec = 0;
	tim1.tv_nsec = 200*1000000;
	nanosleep(&tim1 , &tim2);
}
*/
int main(int argc, char** argv) {
    int gsize;
    int rank;
    char mutex_name[64];
	int nSize_Shared_Data, nSize_Shared_Param_Data, size_pt_mutex_t, shm_fd, shm_param_fd, uid;
	int i;
	void *p_shm=NULL, *p_param_shm=NULL;	// ptr to shared memory
	int *p_open_Count[MAX_FS_SERVER];
	int *p_lxstat_Count[MAX_FS_SERVER];
	int *p_open_Delayed_Count[MAX_FS_SERVER];
	int *p_lxstat_Delayed_Count[MAX_FS_SERVER];
	char *p_FS_Tag_List[MAX_FS_SERVER];	// 16 bytes per record
	long int *p_Data=NULL;
	struct stat file_stat;
	
    MPI_Init(NULL, NULL);
    MPI_Comm_size(MPI_COMM_WORLD, &gsize);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	
//	msleep();	// sleep 0.15 second. 

	uid = getuid();	// ~ 10 us
	size_pt_mutex_t = sizeof(pthread_mutex_t);
	sprintf(mutex_name, "/my_mutex_%d", uid);
	
	nSize_Shared_Data = size_pt_mutex_t*2 + 8*MAX_FS_SERVER*4 + sizeof(uint64_t)*MAX_REC*MAX_FS_SERVER * 2;
	
	shm_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_fd < 0) {	// failed
		perror("mpi_aggregator. main. shm_open. ");
	}
	
	// Map mutex into the shared memory.
	p_shm = mmap(NULL, nSize_Shared_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (p_shm == MAP_FAILED) {
		perror("mmap");
	}
	

	sprintf(mutex_name, "/my_io_param_%d", uid);
	nSize_Shared_Param_Data = sizeof(uint64_t)*(MAX_FS_SERVER * 4 + 2 + 1) + (MAX_LEN_FS_NAME + sizeof(int))*MAX_FS_SERVER;
	shm_param_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_param_fd < 0) {	// failed
		perror("mpi_aggregator. main. shm_open. shm_param_fd.");
	}
	
	p_param_shm = mmap(NULL, nSize_Shared_Param_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_param_fd, 0);
	if (p_param_shm == MAP_FAILED) {
		perror("mmap");
	}
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_FS_Tag_List[i] = (char *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + i*MAX_LEN_FS_NAME);	// 16 char per record
	}
/*
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_open_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i);
		p_open_Delayed_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8);
		p_lxstat_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8*2);
		p_lxstat_Delayed_Count[i] = (int *)((uint64_t)p_shm + size_pt_mutex_t*2 + 8*i + MAX_FS_SERVER*8*3);

		if( (*(p_open_Count[i]) > 0) || (*(p_lxstat_Count[i]) > 0) )	{
			printf("Server %d: %s, Open_Count_Accum = %d Delayed_Open_Count_Accum = %d Stat_Count_Accum = %d Delayed_Stat_Count_Accum = %d\n", 
				i, p_FS_Tag_List[i], *(p_open_Count[i]), *(p_open_Delayed_Count[i]), *(p_lxstat_Count[i]), *(p_lxstat_Delayed_Count[i]));
		}
	}
*/
	p_Data = (long int *)malloc(8*MAX_FS_SERVER*4);
	memset(p_Data, 0, 8*MAX_FS_SERVER*4);
	MPI_Reduce(p_shm + size_pt_mutex_t*2, p_Data, MAX_FS_SERVER*4, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
	
	if(rank == 0)	{
		if(stat("/dev/shm/ooops/high_io", &file_stat) == 0)	{	// file exists. Print summary. 
			for(i=0; i<MAX_FS_SERVER; i++)	{
				p_open_Count[i] = (int *)((uint64_t)p_Data + 8*i);
				p_open_Delayed_Count[i] = (int *)((uint64_t)p_Data + 8*i + MAX_FS_SERVER*8);
				p_lxstat_Count[i] = (int *)((uint64_t)p_Data + 8*i + MAX_FS_SERVER*8*2);
				p_lxstat_Delayed_Count[i] = (int *)((uint64_t)p_Data + 8*i + MAX_FS_SERVER*8*3);
				
				if( (*(p_open_Count[i]) > 0) || (*(p_lxstat_Count[i]) > 0) )	{
	//				printf("Summary Server %d: %-8s, Open_Count_Accum = %d Delayed_Open_Count_Accum = %d Stat_Count_Accum = %d Delayed_Stat_Count_Accum = %d\n", 
	//					i, p_FS_Tag_List[i], *(p_open_Count[i]), *(p_open_Delayed_Count[i]), *(p_lxstat_Count[i]), *(p_lxstat_Delayed_Count[i]));
					printf("Summary Server %d: %-9s, %2d%% open is delayed. Open_Count_Accum = %6d Delayed_Open_Count_Accum = %6d \n                             %2d%% stat is delayed. Stat_Count_Accum = %6d Delayed_Stat_Count_Accum = %6d\n", 
						i, p_FS_Tag_List[i], 100*(*(p_open_Delayed_Count[i]))/(*(p_open_Count[i])), *(p_open_Count[i]), *(p_open_Delayed_Count[i]), 
						100*(*(p_lxstat_Delayed_Count[i]))/(*(p_lxstat_Count[i])), *(p_lxstat_Count[i]), *(p_lxstat_Delayed_Count[i]) );
				}
			}
		}

	}
	free(p_Data);

	munmap(p_shm, nSize_Shared_Data);
	munmap(p_param_shm, nSize_Shared_Param_Data);
	close(shm_fd);
	close(shm_param_fd);

    MPI_Finalize();
}

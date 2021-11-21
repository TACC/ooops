#include <arpa/inet.h>
#include <netinet/in.h> 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <sys/socket.h> 
#include <sys/types.h> 
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>

#define PORT 8888
#define MAXLINE 1024
#define T_INTERVAL_REPORT	(5)

#define MAX_FS_SERVER	(8)	// max number of distinct file system server to be tracked. 
#define MAX_REC	(512)	// number of record of file IO API call time stampe
#define MAX_LEN_FS_NAME	(16)
#define DATA_TAG	(0x41544144)

static int T_Interal_Report=T_INTERVAL_REPORT;

static char szTag_NodeName[]="From: ";

void Get_BaseName(char szHostName[]);

void Get_BaseName(char szHostName[])
{
	int i=0;
	while(szHostName[i] != 0)	{
		if(szHostName[i] == '.')	{
			szHostName[i] = 0;	// truncate hostname[]
			return;
		}
		i++;
	}
}

// Data packet: 
// 1) char "DATA"
// 2) rank ""
// 3) data

typedef struct	{
	int Tag;
	int rank;
	long int Counter[MAX_FS_SERVER*4];	// data buff
}OOOPSDATA, *POOOPSDATA;

int main(int argc, char *argv[]) 
{ 
	int sockfd, nBytes_Written, nBytes_Read, rank;
	char szHostName[255], szFirstMsg[128], *szEnvT_Interval_Report;
	struct sockaddr_in servaddr;
	OOOPSDATA data;

    char mutex_name[64];
	int nSize_Shared_Data, size_pt_mutex_t, shm_fd, uid;
	void *p_shm=NULL;	// ptr to shared memory
	void *p_Data;

	szEnvT_Interval_Report = getenv("OOOPS_REPORT_T_INTERVAL");
	if(szEnvT_Interval_Report != NULL)	{
		T_Interal_Report = atoi(szEnvT_Interval_Report);
	}

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
		printf("socket creation failed"); 
		exit(0); 
	} 

	gethostname(szHostName, 255);
	Get_BaseName(szHostName);

	memset(&servaddr, 0, sizeof(servaddr)); 

	servaddr.sin_family = AF_INET; 
	servaddr.sin_port = htons(PORT); 
	servaddr.sin_addr.s_addr = inet_addr(argv[1]); 

	if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) { 
		printf("\n Error : Connect Failed \n"); 
	} 

	sprintf(szFirstMsg, "%s%s", szTag_NodeName, szHostName);
	nBytes_Written = write(sockfd, szFirstMsg, strlen(szFirstMsg));
	nBytes_Read = read(sockfd, &rank, sizeof(int));
	if(nBytes_Read == sizeof(int))	{
//		printf("Rank = %d\n", rank);
	}
	else	{
		printf("Unexpected: %d\n", rank);
	}

	
	uid = getuid();	// ~ 10 us
	size_pt_mutex_t = sizeof(pthread_mutex_t);
	sprintf(mutex_name, "/my_mutex_%d", uid);
	nSize_Shared_Data = size_pt_mutex_t*2 + 8*MAX_FS_SERVER*4 + sizeof(uint64_t)*MAX_REC*MAX_FS_SERVER * 2;
	shm_fd = shm_open(mutex_name, O_RDWR, 0664);
	
	if(shm_fd < 0) {	// failed
		perror("mpi_aggregator. main. shm_open. ");
	}
	p_shm = mmap(NULL, nSize_Shared_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (p_shm == MAP_FAILED) {
		perror("mmap");
	}
	p_Data = p_shm + size_pt_mutex_t*2;

	data.Tag = DATA_TAG;
	data.rank = rank;

	while(1)	{
		memcpy(data.Counter, p_Data, sizeof(long int)*MAX_FS_SERVER*4);
		nBytes_Written = write(sockfd, &data, sizeof(OOOPSDATA));
		sleep(T_Interal_Report);
	}

	sleep(1);
	close(sockfd); 
} 

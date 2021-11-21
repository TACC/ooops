#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>

#include "utarray.h"
#include "dict.h"

#define MAX_CLIENTS	(8192)
#define MAX_NODE_NAME_LEN	(16)
#define T_INTERVAL_REPORT	(5)

#define MAX_FS_SERVER	(8)	// max number of distinct file system server to be tracked. 
#define MAX_REC	(512)	// number of record of file IO API call time stampe
#define MAX_LEN_FS_NAME	(16)
#define DATA_TAG	(0x41544144)

#define OUT_OF_SPACE	(-111)

static ssize_t read_all(int fd, void *buf, size_t count);
static ssize_t write_all(int fd, const void *buf, size_t count);


typedef struct	{
	int Tag;
	int rank;
	long int Counter[MAX_FS_SERVER*4];	// data buff
}OOOPSDATA, *POOOPSDATA;

static long int t_now = 0;	// in seconds
static long int CounterSum[MAX_FS_SERVER*4], CounterSum_Save[MAX_FS_SERVER*4], delta_CounterSum[MAX_FS_SERVER*4];

static char szSubmitDir[512]="~";
static char szJobID[32]="";
static char szNodeListFile[]="/dev/shm/ooops/hostfile";
static char *szNodeNameList=NULL;
static int gsize=0, gsize_M1=0;	// group size
static int nDataCount=0;
static char szTag_NodeName[]="From: ";
static long int *pOoops_Data_List;
static char *p_FS_Tag_List[MAX_FS_SERVER];	// 16 bytes per record
//static char *szFSNameTrimmed[MAX_FS_SERVER];
static char szFSNameTrimmed[MAX_FS_SERVER][64];
static char szReportName[MAX_FS_SERVER][128];
static char szHrefLine[512]="";

static int nCall_Threshold=80000;	// threshold to tell whether current job has intensive io
static double nCall_per_Second_Threshold=50.0;	// threshold to tell whether current job has intensive io
static int T_Interal_Report=T_INTERVAL_REPORT;

static void Create_Trimemd_Name(void);
static int Create_Chart_Reports(void);

static int nSize_Shared_Data, nSize_Shared_Param_Data, shm_fd, shm_param_fd;
static void *p_shm=NULL, *p_param_shm=NULL;	// ptr to shared memory
static void *p_Local_Data=NULL;
static int n_fs_server=0;

//static FILE *fLog[MAX_FS_SERVER];
static int fd_Log[MAX_FS_SERVER];

Dict p_Hash;
struct elt *elt_list;
int *ht_table=NULL;

static void Read_Host_List(void);
static void Print_Summary(void);
static void Read_Default_Param(void);

static void Read_Host_List(void)
{
	FILE *fIn;
	int i, MaxSize, nBytesRead, NodeIdx;
	char *pBuff, *pNewLine;
	
	fIn = fopen(szNodeListFile, "r");
	if(fIn == NULL) {
		printf("Fail to open file: %s\nQuit\n", szNodeListFile);
	}
	
	MaxSize = MAX_CLIENTS * MAX_NODE_NAME_LEN;
	pBuff = malloc(MaxSize);
	if(pBuff == NULL)       {
		perror("Read_Host_List . malloc . pBuff.");
	}
	
	nBytesRead = fread(pBuff, 1, MaxSize, fIn);
	fclose(fIn);
	
	gsize = 0;
	for(i=0; i<nBytesRead; i++)     {
		if(pBuff[i] == '\n')    {
			pBuff[i] = 0;
			gsize ++;
		}
	}
	gsize_M1 = gsize - 1;
	pOoops_Data_List = malloc( (sizeof(OOOPSDATA)-sizeof(int)*2)*gsize);
	
	szNodeNameList = malloc(MAX_NODE_NAME_LEN * gsize);
	if(szNodeNameList == NULL)      {
		perror("Read_Host_List . malloc . szNodeNameList.");
	}
	
	NodeIdx = 0;
	pNewLine = pBuff;

	p_Hash = (struct dict *)malloc(sizeof(struct dict) + sizeof(int)*gsize*2 + sizeof(struct elt)*gsize*2);
	if(p_Hash == NULL)      {
		perror("Read_Host_List . malloc . p_Hash.");
	}
	DictCreate(p_Hash, gsize*2, &elt_list, &ht_table);	// init hash table

	while(NodeIdx < gsize)  {
		strcpy(szNodeNameList+NodeIdx*MAX_NODE_NAME_LEN, pNewLine);
		DictInsert(p_Hash, pNewLine, NodeIdx, &elt_list, &ht_table);
		pNewLine += (strlen(pNewLine) + 1);
		NodeIdx++;
	}
	
	//for(i=0; i<gsize; i++)  {
	//	printf("node %d, %s\n", i, szNodeNameList+i*MAX_NODE_NAME_LEN);
	//}
	free(pBuff);
}

/*****************************************************************************
* This program demonstrates epoll-based event notification. It monitors for
* new client connections, input on existing connections or their closure, as
* well as signals. The signals are also accepted via a file descriptor using
* signalfd. This program uses blocking I/O in all cases. The epoll mechanism
* tell us which exactly which descriptor is ready, when it is ready, so there
* is no need for speculative/non-blocking reads. This program is more
* efficient than sigio-server.c.
*
* Troy D. Hanson
****************************************************************************/

struct {
	in_addr_t addr;    /* local IP or INADDR_ANY   */
	int port;          /* local port to listen on  */
	int fd;            /* listener descriptor      */
	UT_array *fds;     /* array of client descriptors */
	int signal_fd;     /* used to receive signals  */
	int epoll_fd;      /* used for all notification*/
	int verbose;
	int ticks;         /* uptime in seconds        */
	int pid;           /* our own pid              */
	char *prog;
} cfg = {
	.addr = INADDR_ANY, /* by default, listen on all local IP's   */
		.fd = -1,
		.signal_fd = -1,
		.epoll_fd = -1,
};

static void usage() {
	fprintf(stderr,"usage: %s [-v] [-a <ip>] -p <port\n", cfg.prog);
	exit(-1);
}

/* do periodic work here */
static void periodic(void)
{
	int j;
	double t_inv;
	long int nOpen_Count, nlxstat_Count, nOpen_Delayed_Count, nlxstat_Delayed_Count;
	long int dnOpen_Count, dnlxstat_Count, dnOpen_Delayed_Count, dnlxstat_Delayed_Count;
	char szMsg[512];

	if(gsize == 1)	{	// No other clients
		memcpy(pOoops_Data_List, p_Local_Data, sizeof(long int)*MAX_FS_SERVER*4);
		Print_Summary();
	}

	t_now += T_Interal_Report;

	t_inv = 1.0/T_Interal_Report;

	if(t_now == 0)	{	// dn is calculated differently
		for(j=0; j<n_fs_server; j++)	{
			nOpen_Count = CounterSum[0*MAX_FS_SERVER + j];
			nOpen_Delayed_Count = CounterSum[1*MAX_FS_SERVER + j];
			nlxstat_Count = CounterSum[2*MAX_FS_SERVER + j];
			nlxstat_Delayed_Count = CounterSum[3*MAX_FS_SERVER + j];

			dnOpen_Count = delta_CounterSum[0*MAX_FS_SERVER + j];
			dnOpen_Delayed_Count = delta_CounterSum[1*MAX_FS_SERVER + j];
			dnlxstat_Count = delta_CounterSum[2*MAX_FS_SERVER + j];
			dnlxstat_Delayed_Count = delta_CounterSum[3*MAX_FS_SERVER + j];

			sprintf(szMsg, "%6d %6ld %6ld %6ld %6ld %6ld %6ld %6ld %6ld\n", 
				t_now, nOpen_Count, nOpen_Delayed_Count, nlxstat_Count, nlxstat_Delayed_Count, nOpen_Count, nOpen_Delayed_Count, nlxstat_Count, nlxstat_Delayed_Count);
			write_all(fd_Log[j], szMsg, strlen(szMsg));
		}
	}
	else	{
		for(j=0; j<n_fs_server; j++)	{
			nOpen_Count = CounterSum[0*MAX_FS_SERVER + j];
			nOpen_Delayed_Count = CounterSum[1*MAX_FS_SERVER + j];
			nlxstat_Count = CounterSum[2*MAX_FS_SERVER + j];
			nlxstat_Delayed_Count = CounterSum[3*MAX_FS_SERVER + j];

			dnOpen_Count = delta_CounterSum[0*MAX_FS_SERVER + j];
			dnOpen_Delayed_Count = delta_CounterSum[1*MAX_FS_SERVER + j];
			dnlxstat_Count = delta_CounterSum[2*MAX_FS_SERVER + j];
			dnlxstat_Delayed_Count = delta_CounterSum[3*MAX_FS_SERVER + j];

			sprintf(szMsg, "%6d %6ld %6ld %6ld %6ld %7.1lf %7.1lf %7.1lf %7.1lf\n", 
				t_now, nOpen_Count, nOpen_Delayed_Count, nlxstat_Count, nlxstat_Delayed_Count, 
				dnOpen_Count*t_inv, dnOpen_Delayed_Count*t_inv, dnlxstat_Count*t_inv, dnlxstat_Delayed_Count*t_inv);
			write_all(fd_Log[j], szMsg, strlen(szMsg));
		}
	}
}

static int add_epoll(int events, int fd) {
	int rc;
	struct epoll_event ev;
	memset(&ev,0,sizeof(ev)); // placate valgrind
	ev.events = events;
	ev.data.fd= fd;
	if (cfg.verbose) fprintf(stderr,"adding fd %d to epoll\n", fd);
	rc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	if (rc == -1) {
		fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
	}
	return rc;
}

static int del_epoll(int fd) {
	int rc;
	struct epoll_event ev;
	rc = epoll_ctl(cfg.epoll_fd, EPOLL_CTL_DEL, fd, &ev);
	if (rc == -1) {
		fprintf(stderr,"epoll_ctl: %s\n", strerror(errno));
	}
	return rc;
}

/* signals that we'll accept synchronously via signalfd */
int sigs[] = {SIGIO,SIGHUP,SIGTERM,SIGINT,SIGQUIT,SIGALRM};

static int setup_listener() {
	int rc = -1, one=1;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		fprintf(stderr,"socket: %s\n", strerror(errno));
		goto done;
	}
	
	/**********************************************************
	* internet socket address structure: our address and port
	*********************************************************/
	struct sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cfg.addr;
	sin.sin_port = htons(cfg.port);
	
	/**********************************************************
	* bind socket to address and port 
	*********************************************************/
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr*)&sin, sizeof(sin)) == -1) {
		fprintf(stderr,"bind: %s\n", strerror(errno));
		goto done;
	}
	
	/**********************************************************
	* put socket into listening state
	*********************************************************/
	if (listen(fd,1) == -1) {
		fprintf(stderr,"listen: %s\n", strerror(errno));
		goto done;
	}
	
	cfg.fd = fd;
	rc=0;
	
done:
	if ((rc < 0) && (fd != -1)) close(fd);
	return rc;
}

/* accept a new client connection to the listening socket */
static int accept_client() {
	int fd, nBytes, rank;
	struct sockaddr_in in;
	socklen_t sz = sizeof(in);
	char szBuff[128], szNodeName[128]="";
	unsigned long long fn_hash;
	
	fd = accept(cfg.fd,(struct sockaddr*)&in, &sz);
	if (fd == -1) {
		fprintf(stderr,"accept: %s\n", strerror(errno)); 
		goto done;
	}
	
	if (cfg.verbose && (sizeof(in)==sz)) {
		fprintf(stderr,"connection fd %d from %s:%d\n", fd,
			inet_ntoa(in.sin_addr), (int)ntohs(in.sin_port));
	}

	nBytes = read(fd, szBuff, 128);
	szBuff[nBytes] = 0;
	if(strncmp(szTag_NodeName, szBuff, 6) == 0)	{
		sscanf(szBuff + 6, "%s", szNodeName);
		rank = DictSearch(p_Hash, szNodeName, &elt_list, &ht_table, &fn_hash);
	}
	else	{
		printf("Unexpected: %s\n", szBuff);
		rank = -1;
	}
//	printf("Client %s, rank = %d\n", szBuff + 6, rank);
	write(fd, &rank, sizeof(int));
	
	if (add_epoll(EPOLLIN, fd) == -1) { close(fd); fd = -1; }
	
done:
	if (fd != -1) utarray_push_back(cfg.fds,&fd);
	return fd;
}

static void drain_client(int fd) {
	int rc, pos, *fp;
	char buf[1024];
	OOOPSDATA *pData;
	
	pData = (OOOPSDATA*)buf;
	pData->Tag = 0;
	pData->rank = -1;
	rc = read(fd, buf, 1024);
	if( (rc == sizeof(OOOPSDATA)) && (pData->Tag == DATA_TAG) )	{
		if(pData->rank)	{
			memcpy( pOoops_Data_List + (MAX_FS_SERVER*4*pData->rank), pData->Counter, sizeof(long int)*MAX_FS_SERVER*4);
			nDataCount++;
			if(nDataCount % gsize_M1 == 0)	{	// time to submit the rank 0 (local) data
				memcpy(pOoops_Data_List, p_Local_Data, sizeof(long int)*MAX_FS_SERVER*4);
				Print_Summary();
			}
		}
		else	{
			printf("Unexpected data: Rank = %d\n", pData->rank);
		}
	}

	//  switch(rc) { 
	//    default: fprintf(stderr,"received %d bytes\n", rc);         break;
	//    case  0: fprintf(stderr,"fd %d closed\n", fd);              break;
	//    case -1: fprintf(stderr, "recv: %s\n", strerror(errno));    break;
	//  }
	
	if (rc != 0) return;
	
	/* client closed. log it, tell epoll to forget it, close it */
	if (cfg.verbose) fprintf(stderr,"client %d has closed\n", fd);
	del_epoll(fd);
	close(fd);
	
	/* delete record of fd. linear scan. want hash if lots of fds */
	fp=NULL;
	while ( (fp=(int*)utarray_next(cfg.fds,fp))) { 
		if (*fp != fd) continue;
		pos = utarray_eltidx(cfg.fds,fp);
		utarray_erase(cfg.fds,pos,1);
		break;
	}	
}

int main(int argc, char *argv[])
{
    char mutex_name[64], szLogName[128], szMsg[256], *szEnvJobID, szEmptyID[]="0", *szEnv_nCall_Threshold, *szEnv_nCall_per_Second_Threshold, *szEnvSubmitDir, *szEnvT_Interval_Report;
	int i, j, uid;

	cfg.prog = argv[0];
	cfg.prog=argv[0];
	cfg.pid = getpid();
	int n, opt, *fd;
	struct epoll_event ev;
	struct signalfd_siginfo info;
	
	utarray_new(cfg.fds,&ut_int_icd);
	
	while ( (opt=getopt(argc,argv,"vp:a:h")) != -1) {
		switch(opt) {
		case 'v': cfg.verbose++; break;
		case 'p': cfg.port=atoi(optarg); break; 
		case 'a': cfg.addr=inet_addr(optarg); break; 
		case 'h': default: usage(); break;
		}
	}
	if (cfg.addr == INADDR_NONE) usage();
	if (cfg.port==0) usage();
	
	Read_Host_List();


	uid = getuid();	// ~ 10 us
	sprintf(mutex_name, "/my_mutex_%d", uid);
	nSize_Shared_Data = sizeof(pthread_mutex_t)*2 + 8*MAX_FS_SERVER*4 + sizeof(uint64_t)*MAX_REC*MAX_FS_SERVER * 2;
	shm_fd = shm_open(mutex_name, O_RDWR, 0664);
	if(shm_fd < 0) {	// failed
		perror("mpi_aggregator. main. shm_open. ");
	}
	p_shm = mmap(NULL, nSize_Shared_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (p_shm == MAP_FAILED) {
		perror("mmap");
	}
	p_Local_Data = p_shm + sizeof(pthread_mutex_t)*2;

	sprintf(mutex_name, "/my_io_param_%d", uid);
	nSize_Shared_Param_Data = sizeof(uint64_t)*(MAX_FS_SERVER * 4 + 2 + 1) + (MAX_LEN_FS_NAME + sizeof(int))*MAX_FS_SERVER;
	shm_param_fd = shm_open(mutex_name, O_RDWR, 0664);
	if(shm_param_fd < 0) {	// failed
		perror("main. shm_open. shm_param_fd. ");
	}
	p_param_shm = mmap(NULL, nSize_Shared_Param_Data, PROT_READ | PROT_WRITE, MAP_SHARED, shm_param_fd, 0);
	if (p_param_shm == MAP_FAILED) {
		perror("main. mmap. p_param_shm.");
	}
	
	for(i=0; i<MAX_FS_SERVER; i++)	{
		p_FS_Tag_List[i] = (char *)(p_param_shm+16+8*MAX_FS_SERVER*4 + 8 + i*MAX_LEN_FS_NAME);	// 16 char per record
	}
	Read_Default_Param();
	for(i=0; i<4; i++)	{	// initialization
		for(j=0; j<n_fs_server; j++)	{
			CounterSum[i*MAX_FS_SERVER + j] = 0;
		}
	}

	szEnvJobID=getenv("SLURM_JOB_ID");
	if(szEnvJobID == NULL)	{
		szEnvJobID = szEmptyID;
	}
	strcpy(szJobID, szEnvJobID);

	szEnv_nCall_Threshold=getenv("OOOPS_NCALL_REPORT_THRESHOLD");
	if(szEnv_nCall_Threshold != NULL)	{
		nCall_Threshold = atoi(szEnv_nCall_Threshold);
	}
	szEnv_nCall_per_Second_Threshold=getenv("OOOPS_NCALL_PS_REPORT_THRESHOLD");
	if(szEnv_nCall_per_Second_Threshold != NULL)	{
		nCall_per_Second_Threshold = atof(szEnv_nCall_per_Second_Threshold);
	}
	szEnvSubmitDir = getenv("SLURM_SUBMIT_DIR");
	if(szEnvSubmitDir != NULL)	{
		strcpy(szSubmitDir, szEnvSubmitDir);
	}

	szEnvT_Interval_Report = getenv("OOOPS_REPORT_T_INTERVAL");
	if(szEnvT_Interval_Report != NULL)	{
		T_Interal_Report = atoi(szEnvT_Interval_Report);
	}

	memset(CounterSum, 0, sizeof(long int)*MAX_FS_SERVER*4);
	memset(CounterSum_Save, 0, sizeof(long int)*MAX_FS_SERVER*4);

	Create_Trimemd_Name();
	for(j=0; j<n_fs_server; j++)	{
		sprintf(szLogName, "/dev/shm/ooops/log_ooops_%s_%s", szFSNameTrimmed[j], szEnvJobID);
		fd_Log[j] = open(szLogName, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
		if(fd_Log[j] == -1)	{	// error!!!
			printf("Fail to open file %s\nQuit\n", szLogName);
			exit(1);
		}
		sprintf(szMsg, "time   n_open  n_open_delayed   n_stat  n_stat_delayed  dn_open dn_open_delayed  dn_stat  dn_stat_delayed\n");
		write_all(fd_Log[j], szMsg, strlen(szMsg));
	}
	
	/* block all signals. we take signals synchronously via signalfd */
	sigset_t all;
	sigfillset(&all);
	sigprocmask(SIG_SETMASK,&all,NULL);
	
	/* a few signals we'll accept via our signalfd */
	sigset_t sw;
	sigemptyset(&sw);
	for(n=0; n < sizeof(sigs)/sizeof(*sigs); n++) sigaddset(&sw, sigs[n]);
	
	if (setup_listener()) goto done;
	
	/* create the signalfd for receiving signals */
	cfg.signal_fd = signalfd(-1, &sw, 0);
	if (cfg.signal_fd == -1) {
		fprintf(stderr,"signalfd: %s\n", strerror(errno));
		goto done;
	}
	
	/* set up the epoll instance */
	cfg.epoll_fd = epoll_create(1); 
	if (cfg.epoll_fd == -1) {
		fprintf(stderr,"epoll: %s\n", strerror(errno));
		goto done;
	}
	
	/* add descriptors of interest */
	if (add_epoll(EPOLLIN, cfg.fd))        goto done; // listening socket
	if (add_epoll(EPOLLIN, cfg.signal_fd)) goto done; // signal socket
	

	// This is our main loop. epoll for input or signals.
	t_now = -T_Interal_Report;
	alarm(T_Interal_Report);
	while (epoll_wait(cfg.epoll_fd, &ev, 1, -1) > 0) {
		// if a signal was sent to us, read its signalfd_siginfo
		if (ev.data.fd == cfg.signal_fd) { 
			if (read(cfg.signal_fd, &info, sizeof(info)) != sizeof(info)) {
				fprintf(stderr,"failed to read signal fd buffer\n");
				continue;
			}
//			switch (info.ssi_signo) {
//			case SIGALRM: 
//				if ((++cfg.ticks % 10) == 0) periodic(); 
//				alarm(T_Interal_Report); 
//				continue;
//			default:  /* exit */
			if(info.ssi_signo == SIGALRM)	{
				periodic();
				alarm(T_Interal_Report);
				continue;
			}
			else if(info.ssi_signo == SIGTERM)	{
//				write(STDERR_FILENO,"Got signal %d (SIGTERM)\n", info.ssi_signo);
				for(j=0; j<n_fs_server; j++)	{
					fsync(fd_Log[j]);
					close(fd_Log[j]);
				}
				Create_Chart_Reports();
			}
			else	{
				write(STDERR_FILENO,"Got signal %d\n", info.ssi_signo);  
				for(j=0; j<n_fs_server; j++)	{
					fsync(fd_Log[j]);
					close(fd_Log[j]);
				}
			}
				// Output my summary...
				goto done;
//			}
		}
		
		/* regular POLLIN. handle the particular descriptor that's ready */
		assert(ev.events & EPOLLIN);
		if (cfg.verbose) fprintf(stderr,"handle POLLIN on fd %d\n", ev.data.fd);
		if (ev.data.fd == cfg.fd) accept_client();
		else drain_client(ev.data.fd);
		
	}
	
	fprintf(stderr, "epoll_wait: %s\n", strerror(errno));
	
done:   /* we get here if we got a signal like Ctrl-C */
	fd=NULL;
	while ( (fd=(int*)utarray_next(cfg.fds,fd))) {del_epoll(*fd); close(*fd);}
	utarray_free(cfg.fds);
	if (cfg.fd != -1) close(cfg.fd);
	if (cfg.epoll_fd != -1) close(cfg.epoll_fd);
	if (cfg.signal_fd != -1) close(cfg.signal_fd);

	return 0;
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
                        printf("%s\n", szLine);
                        num_read = sscanf(szLine+10, "%f", &freq);
                        if(num_read == 1) {
                                printf("freq = %f MHz\n", freq);
                                fclose(fIn);
                                return (freq*1.0E6);
                        }
                }
        }

        fclose(fIn);

  printf("Error to get CPU frequency.\n");

  return 0;
}


#define MAX_LEN_CONFIG	(8192)
#define MAX_NUM_CONFIG	(4)
#define N_ITEM_PER_REC	(10)

static void Read_Default_Param(void)
{
	int fd, i, Config_Use=-1;
	char szBuff[MAX_LEN_CONFIG], *p, *p_Use;
	int num_read, nReadItem;
	float freq_list[MAX_NUM_CONFIG], f, f_Use=0.0f, f_Sys, df, df_Min=10.0, freq;
	int nConfig=0, PosList[MAX_NUM_CONFIG*2];
	int Tag_Start=0x6572663C, Tag_End=0x72662F3C;	// "<fre" and "</fr"
	char szItems[60][64];	// parameters
	int nParam;
	char *szConfigName=NULL;

	n_fs_server = 0;
	freq = Get_Freq();
	f_Sys = freq * 0.000000001;	// GHz

	szConfigName=getenv("IO_LIMIT_CONFIG");
	if(szConfigName == NULL)	{
		printf("env IO_LIMIT_CONFIG is NOT set.\nQuit\n");
		exit(1);
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

	return;
}


static void Print_Summary(void)
{
	int i, j, k, idx;
//	long int *pNodeData, nOpen_Count, nlxstat_Count, nOpen_Delayed_Count, nlxstat_Delayed_Count;
	long int *pNodeData;

	for(i=0; i<4; i++)	{
		for(j=0; j<n_fs_server; j++)	{
			CounterSum[i*MAX_FS_SERVER + j] = 0;
		}
	}

	for(k=0; k<gsize; k++)	{
		pNodeData = pOoops_Data_List + MAX_FS_SERVER*4*k;
		for(i=0; i<4; i++)	{
			for(j=0; j<n_fs_server; j++)	{
				idx = i*MAX_FS_SERVER + j;
				CounterSum[idx] += ( pNodeData[idx] );
			}
		}
/*
		for(j=0; j<n_fs_server; j++)	{
			nOpen_Count = pNodeData[0*MAX_FS_SERVER + j];
			nOpen_Delayed_Count = pNodeData[1*MAX_FS_SERVER + j];
			nlxstat_Count = pNodeData[2*MAX_FS_SERVER + j];
			nlxstat_Delayed_Count = pNodeData[3*MAX_FS_SERVER + j];
			if( (nOpen_Count > 0) || (nlxstat_Count > 0) )	{
				printf("Summary node %d Server %d: %s, Open_Count_Accum = %d Delayed_Open_Count_Accum = %d Stat_Count_Accum = %d Delayed_Stat_Count_Accum = %d\n", 
					k, j, p_FS_Tag_List[j], nOpen_Count, nOpen_Delayed_Count, nlxstat_Count, nlxstat_Delayed_Count);
			}
		}
*/
	}

	for(i=0; i<4; i++)	{
		for(j=0; j<n_fs_server; j++)	{
			idx = i*MAX_FS_SERVER + j;
			delta_CounterSum[idx] = CounterSum[idx] - CounterSum_Save[idx];
			CounterSum_Save[idx] = CounterSum[idx];
		}
	}

//	printf("\n");
/*
	for(j=0; j<n_fs_server; j++)	{
		nOpen_Count = CounterSum[0*MAX_FS_SERVER + j];
		nOpen_Delayed_Count = CounterSum[1*MAX_FS_SERVER + j];
		nlxstat_Count = CounterSum[2*MAX_FS_SERVER + j];
		nlxstat_Delayed_Count = CounterSum[3*MAX_FS_SERVER + j];
		if( (nOpen_Count > 0) || (nlxstat_Count > 0) )	{
			printf("Summary Server %d: %s, Open_Count_Accum = %d Delayed_Open_Count_Accum = %d Stat_Count_Accum = %d Delayed_Stat_Count_Accum = %d\n", 
				j, p_FS_Tag_List[j], nOpen_Count, nOpen_Delayed_Count, nlxstat_Count, nlxstat_Delayed_Count);
		}
	}
*/
}

static void Create_Trimemd_Name(void)
{
	int i, j, nLen;

	for(i=0; i<n_fs_server; i++)	{
		nLen = strlen(p_FS_Tag_List[i]);

		if(p_FS_Tag_List[i][0] == '/')	{
			memcpy(szFSNameTrimmed[i], p_FS_Tag_List[i] + 1, nLen);
		}
		else	{
			memcpy(szFSNameTrimmed[i], p_FS_Tag_List[i]    , nLen);
		}

		for(j=1; j<=2; j++)	{
			if(szFSNameTrimmed[i][nLen-j] == '/')	{
				szFSNameTrimmed[i][nLen-j] = 0;
			}
		}
	}
}

static int Create_Chart_Reports(void)
{
	FILE *fIn, *fOut;
	char szLine[256], *ReadLine, szLogName[128], szRealReportName[1024], szMsg[2048], szAllHtmlFile[512], szLocal[128];
	int i, t, nItems, idx_fs_high_IO=0;
	int n[4], n0_last, n2_last;
	int nCountMax=0;
	double dn[4];
	double dnCountMax[MAX_FS_SERVER];

//	Create_Trimemd_Name();
	szHrefLine[0] = 0;
	for(i=0; i<n_fs_server; i++)	{
		sprintf(szReportName[i], "log_ooops_%s_%s.html", szFSNameTrimmed[i], szJobID);
		sprintf(szLocal, "<a class=\"mytxt\" href=\"%s\">/%s</a> ", szReportName[i], szFSNameTrimmed[i]);
		strcat(szHrefLine, szLocal);
	}

	for(i=0; i<n_fs_server; i++)	{
		dnCountMax[i] = 0.0;
		sprintf(szLogName, "/dev/shm/ooops/log_ooops_%s_%s", szFSNameTrimmed[i], szJobID);
		fIn = fopen(szLogName, "r");
		if(fIn == NULL)	{
			sprintf(szMsg, "Fail to open file %s\nQuit\n", szLogName);
			write(STDERR_FILENO, szMsg, strlen(szMsg));
			exit(1);
		}
		
		fgets(szLine, 256, fIn);
		while(1)	{
			if(feof(fIn))	{
				break;
			}
			ReadLine = fgets(szLine, 256, fIn);
			if(ReadLine == NULL)	{
				break;
			}
			nItems = sscanf(szLine, "%d%d%d%d%d%lf%lf%lf%lf", &t, &(n[0]), &(n[1]), &(n[2]), &(n[3]), &(dn[0]), &(dn[1]), &(dn[2]), &(dn[3]));
			if(nItems == 9)	{
				n0_last = n[0];
				n2_last = n[2];
			}
		}
		if(n0_last > nCountMax)	{
			nCountMax = n0_last;
			idx_fs_high_IO = i;
		}
		if(n2_last > nCountMax)	{
			nCountMax = n2_last;
			idx_fs_high_IO = i;
		}
		fseek(fIn, 0, SEEK_SET);
		fgets(szLine, 256, fIn);

		while(1)	{
			if(feof(fIn))	{
				break;
			}
			ReadLine = fgets(szLine, 256, fIn);
			if(ReadLine == NULL)	{
				break;
			}
			nItems = sscanf(szLine, "%d%d%d%d%d%lf%lf%lf%lf", &t, &(n[0]), &(n[1]), &(n[2]), &(n[3]), &(dn[0]), &(dn[1]), &(dn[2]), &(dn[3]));
			if(nItems == 9)	{
				if(dn[0] > dnCountMax[i])	{
					dnCountMax[i] = dn[0];
				}
				if(dn[2] > dnCountMax[i])	{
					dnCountMax[i] = dn[2];
				}
			}
		}

		fclose(fIn);
	}

//	printf("DBG> nCountMax = %d nCall_Threshold = %d  dnCountMax[idx_fs_high_IO] = %5.1lf nCall_per_Second_Threshold = %5.1lf\n", 
//		nCountMax, nCall_Threshold, dnCountMax[idx_fs_high_IO], nCall_per_Second_Threshold);
	if( (nCountMax >= nCall_Threshold) && (dnCountMax[idx_fs_high_IO] >= nCall_per_Second_Threshold) )	{
		fOut = fopen("/dev/shm/ooops/high_io", "w");
		if(fOut == NULL)	{
			strcpy(szMsg, "Fail to open file /dev/shm/ooops/high_io.\nQuit\n");
			write(STDERR_FILENO, szMsg, strlen(szMsg));
			exit(1);
		}
		fprintf(fOut, "/%s\n", szFSNameTrimmed[idx_fs_high_IO]);
		fprintf(fOut, "%d\n", nCountMax);
//		fprintf(fOut, "%7.1lf\n", dnCountMax[idx_fs_high_IO]);
		fclose(fOut);


		for(i=0; i<n_fs_server; i++)	{
			sprintf(szLogName, "/dev/shm/ooops/log_ooops_%s_%s", szFSNameTrimmed[i], szJobID);
			fIn = fopen(szLogName, "r");
			if(fIn == NULL)	{
				sprintf(szMsg, "Fail to open file %s\nQuit\n", szLogName);
				write(STDERR_FILENO, szMsg, strlen(szMsg));
				exit(1);
			}
			
			sprintf(szRealReportName, "%s/%s", szSubmitDir, szReportName[i]);
			fOut = fopen(szRealReportName, "w");
			if(fOut == NULL)	{
				sprintf(szMsg, "Fail to open file %s\nQuit\n", szRealReportName);
				write(STDERR_FILENO, szMsg, strlen(szMsg));
				exit(1);
			}
			
			fgets(szLine, 256, fIn);

			fprintf(fOut, "<html>\n");
			fprintf(fOut, "  <meta http-equiv=\"Content-Type\" content=\"text/html\" />\n");
			fprintf(fOut, "<head>\n");

			fprintf(fOut, "  <style>\n");
			fprintf(fOut, "  a.mytxt:link {color:#3366cc;font-size:180%;padding:80px;font-weight: bold;}\n");
			fprintf(fOut, "  a.mytxt:visited {color:#cc0066;font-size:180%;padding:80px;font-weight: bold;}\n");
			fprintf(fOut, "  a.mytxt:hover {background:#66ff66;font-size:180%;padding:80px;font-weight: bold;}\n");
			fprintf(fOut, "  </style>\n");

			fprintf(fOut, "  <script type=\"text/javascript\" src=\"https://www.gstatic.com/charts/loader.js\"></script>\n");
			fprintf(fOut, "</head>\n");

			fprintf(fOut, "<div style=\"text-align: center\">\n%s\n</div>\n", szHrefLine);

			fprintf(fOut, "<body>\n");
			fprintf(fOut, "  <div id=\"chart_div\" style=\"width: 1280px; height: 700px;\" align=\"center\"></div>\n");
			fprintf(fOut, "  <div id=\"chart2_div\" style=\"width: 1280px; height: 700px;\" align=\"center\"></div>\n");
			fprintf(fOut, " <script type=\"text/javascript\">\n");
			fprintf(fOut, "      google.charts.load('current', {'packages':['corechart']});\n");
			fprintf(fOut, "      google.charts.setOnLoadCallback(drawChart);\n");
			fprintf(fOut, "      google.charts.setOnLoadCallback(drawChart2);\n");
			fprintf(fOut, "     function drawChart() {\n");
			fprintf(fOut, "        var data = google.visualization.arrayToDataTable([\n");
			fprintf(fOut, "         ['t(seconds)', 'nOpen', 'nOpen_delayed', 'nStat', 'nStat_delayed'],\n");

			while(1)	{
				if(feof(fIn))	{
					break;
				}
				ReadLine = fgets(szLine, 256, fIn);
				if(ReadLine == NULL)	{
					break;
				}
				nItems = sscanf(szLine, "%d%d%d%d%d%lf%lf%lf%lf", &t, &(n[0]), &(n[1]), &(n[2]), &(n[3]), &(dn[0]), &(dn[1]), &(dn[2]), &(dn[3]));
				if(nItems == 9)	{
					fprintf(fOut, "[%d,%d,%d,%d,%d],\n", t, n[0], n[1], n[2], n[3]);
				}
			}
	//		fseek(fOut, -3, SEEK_END);	// Windows
			fseek(fOut, -2, SEEK_END);	// Linux
			fseek(fIn, 0, SEEK_SET);
			fgets(szLine, 256, fIn);

			fprintf(fOut, "\n]);\n");
			fprintf(fOut, "        var options = {\n");
			fprintf(fOut, "          title: 'Accumulated Lustre access on /%s',\n", szFSNameTrimmed[i]);
			fprintf(fOut, "          hAxis: {title: 'Time (seconds)',  titleTextStyle: {color: '#333'},\n");
			fprintf(fOut, "                   slantedText:true, slantedTextAngle:80},\n");
			fprintf(fOut, "          vAxis: {minValue: 0},\n");
			fprintf(fOut, "          explorer: { \n");
			fprintf(fOut, "            actions: ['dragToZoom', 'rightClickToReset'],\n");
			fprintf(fOut, "            axis: 'horizontal',\n");
			fprintf(fOut, "            keepInBounds: true,\n");
			fprintf(fOut, "            maxZoomIn: 30.0},\n");
			fprintf(fOut, "        };\n");
			fprintf(fOut, "        var chart = new google.visualization.LineChart(document.getElementById('chart_div'));\n");
			fprintf(fOut, "       chart.draw(data, options);\n");
			fprintf(fOut, "      }\n");
			fprintf(fOut, "      function drawChart2() {\n");
			fprintf(fOut, "        var data = google.visualization.arrayToDataTable([\n");
			fprintf(fOut, "         ['t(seconds)', 'dnOpen', 'dnOpen_delayed', 'dnStat', 'dnStat_delayed'],\n");

			while(1)	{
				if(feof(fIn))	{
					break;
				}
				ReadLine = fgets(szLine, 256, fIn);
				if(ReadLine == NULL)	{
					break;
				}
				nItems = sscanf(szLine, "%d%d%d%d%d%lf%lf%lf%lf", &t, &(n[0]), &(n[1]), &(n[2]), &(n[3]), &(dn[0]), &(dn[1]), &(dn[2]), &(dn[3]));
				if(nItems == 9)	{
					fprintf(fOut, "[%d,%7.1lf,%7.1lf,%7.1lf,%7.1lf],\n", t, dn[0], dn[1], dn[2], dn[3]);
				}
			}

	//		fseek(fOut, -3, SEEK_END);	// Windows
			fseek(fOut, -2, SEEK_END);	// Linux

			fprintf(fOut, "\n]);\n");
			fprintf(fOut, "        var options = {\n");
			fprintf(fOut, "          title: 'Instant Lustre access on /%s',", szFSNameTrimmed[i]);
			fprintf(fOut, "          hAxis: {title: 'Time (seconds)',  titleTextStyle: {color: '#333'},\n");
			fprintf(fOut, "                   slantedText:true, slantedTextAngle:80},\n");
			fprintf(fOut, "          vAxis: {minValue: 0},\n");
			fprintf(fOut, "          explorer: { \n");
			fprintf(fOut, "            actions: ['dragToZoom', 'rightClickToReset'],\n");
			fprintf(fOut, "            axis: 'horizontal',\n");
			fprintf(fOut, "            keepInBounds: true,\n");
			fprintf(fOut, "            maxZoomIn: 30.0},\n");
			fprintf(fOut, "        };\n");
			fprintf(fOut, "        var chart2 = new google.visualization.LineChart(document.getElementById('chart2_div'));\n");
			fprintf(fOut, "        chart2.draw(data, options);\n");
			fprintf(fOut, "      }\n");
			fprintf(fOut, "   </script>\n");
			fprintf(fOut, "</body>\n");
			fprintf(fOut, "</html>\n");

			fclose(fIn);
			fclose(fOut);
		}

		szAllHtmlFile[0] = 0;
		for(i=0; i<n_fs_server; i++)	{
			sprintf(szMsg, "%s\n", szReportName[i]);
			strcat(szAllHtmlFile, szMsg);
		}
		sprintf(szMsg, "Warning: It seems that your job %s might have intensive IO (%d open/stat calls) on /%s.\nPlease check the following ooops reports for details.\n%sunder directory %s\n", 
			szJobID, nCountMax, szFSNameTrimmed[idx_fs_high_IO], szAllHtmlFile, szSubmitDir);
		write(STDOUT_FILENO, szMsg, strlen(szMsg));
	}

	return 0;
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

static ssize_t write_all(int fd, const void *buf, size_t count)
{
	ssize_t ret, nBytes=0;
	void *p_buf;

	p_buf = (void *)buf;
	while (count != 0 && (ret = write(fd, p_buf, count)) != 0) {
		if (ret == -1) {
			if (errno == EINTR)	{
				continue;
			}
			else if (errno == ENOSPC)	{	// out of space. Quit immediately!!!
				return OUT_OF_SPACE;
			}

			perror ("write");
			break;
		}
		nBytes += ret;
		count -= ret;
		p_buf += ret;
	}
	return nBytes;
}


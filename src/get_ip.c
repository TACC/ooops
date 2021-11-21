#include <stdio.h>
#include <unistd.h>
#include <string.h> /* for strncpy */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

static char szNICName[64]="ib0";

int main(int argc, char *argv[])
{
 int fd;
 struct ifreq ifr;

 fd = socket(AF_INET, SOCK_DGRAM, 0);
 /* I want to get an IPv4 IP address */
 ifr.ifr_addr.sa_family = AF_INET;
 /* I want IP address attached to "eth0" */
 if(argc == 2) strncpy(ifr.ifr_name, argv[1], strlen(argv[1])+1);
 else strncpy(ifr.ifr_name, "ib0", 4);
 ioctl(fd, SIOCGIFADDR, &ifr);
 close(fd);

 /* display result */
 printf("%s\n", inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr));

 return 0;
}

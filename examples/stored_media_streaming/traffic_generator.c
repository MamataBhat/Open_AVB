/* 
This application transfers udp packets to the desired machine continuously
To Compile: gcc udp_traffic_generator.c -o udp_traffic_generator
To Run: ./udp_traffic_generator "IP address of Desired Machine"
Eg: ./udp_traffic_generator 10.99.26.94

* Copyright (c) <2013>, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.

*/


#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include<string.h>
#include<stdlib.h>
#define BUFLEN 65507    //This is the maximum UDP packet size. (512-65507)
#define NPACK 1000
#define PORT 5002



int main(int argc, char *argv[])
{
        if(argc != 2)
	{
  	printf("Please specify IP Address of the machine to which traffic needs to be induced: \n");
  	return 1;
	}
        struct sockaddr_in si_other;
        int s,k=-1, i, slen=sizeof(si_other);
        char buf[BUFLEN];
        if ((s=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1)
            error("socket");
        memset((char *) &si_other, 0, sizeof(si_other));
        si_other.sin_family = AF_INET;
        si_other.sin_port = htons(PORT);
        if (inet_aton(argv[1], &si_other.sin_addr)==0)
	{
          fprintf(stderr, "inet_aton() failed\n");
          exit(1);
        }

        while(k<=-1)
      {
        for (i=0; i<NPACK; i++) 
	{

        printf("Sending packet %d\n", i);
        sprintf(buf, "This is packet %d\n", i);
        if (sendto(s, buf, BUFLEN, 0,(const struct sockaddr*) &si_other, slen)==-1)
        error("sendto()");
        }
        k--;
      }
        close(s);
        return 0;
	
}
void error(char *s)
{
       perror(s);
       return ;
}

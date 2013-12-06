/*
 * Author: Symphony teleca corporation
 * Date: Nov- 10 - 2013
 *
 * File Name: transmit_video.c
 *
 * 
 * This application reads the data from audio and video pipelines,creates a igb packet,
 * adds timestamp taking from the igb wallclock and transmits the packet to a fixed
 * multicast address.
 * 
 *
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
 * 
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <netinet/in.h>
#include <net/ethernet.h>
#include <netpacket/packet.h>
#include <pci/pci.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/un.h>
#include <sys/user.h>
#include "igb.h"
#include "mrpd.h"
#include "mrp.h"
#include "msrp.h"
#include "avbtp.h"




/* global variables */

volatile int listeners = 0;
volatile int mrp_okay;
volatile int mrp_error = 0;;
volatile int domain_a_valid = 0;
volatile int domain_b_valid = 0;

device_t igb_dev;

int halt_tx = 0;
int control_socket = -1;
int domain_class_a_id;
int domain_class_a_priority;
int domain_class_a_vid;
int domain_class_b_id;
int domain_class_b_priority;
int domain_class_b_vid;

int startofinputdata = 0;
int audio_data_start = 0;
int video_data_start = 0;


int 	buff_size;
char 	pipe_ptr[1500];
int 	*syncPtr;
int 	a_priority = 0;
u_int16_t a_vid = 0;
int 	class_a_id = 0;
struct 	igb_dma_alloc a_page;
uint32_t pkt_sz;

int audio_fd[2];
int video_fd[2];
int igb_fd[2];

void * audio_data_read(void *arg);
void * video_data_read(void *arg);
void* decode_data(void *arg);
int gst_main (char* fileData);
int read_pipe(int fd,void * ptr,int length) ;
void task_clean (void) ;

char *fileData;
pthread_mutex_t enq_lock;
pthread_mutex_t freeq_lock;
pthread_t monitor_thread;
pthread_attr_t monitor_attr;

#define ETHER_TYPE_AVTP		0x22f0
#define FRAME_SIZE 1500
#define XMIT_DELAY (200000000)	/* us */
#define RENDER_DELAY (XMIT_DELAY+2000000)	/* us */
#define AUDIO_DATA 0xffff
#define VIDEO_DATA 0xfffe
#define PACKET_IPG		125000	/* (1) packet every 125 usec */

int32_t payload_len;
int8_t *interface = NULL;
unsigned char STATION_ADDR[] = { 0, 0, 0, 0, 0, 0 };
unsigned char STREAM_ID[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
unsigned char control_stream_id[] = {0, 0, 0, 0, 0, 0, 0, 0 };
/*IEEE 1722 reserved address */
unsigned char DEST_ADDR[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80 };
int talker = 0;
int count  = 0 ;

uint64_t reverse_64(uint64_t val)
{
	uint32_t low32, high32;

	low32 = val & 0xffffffff;
	high32 = (val >> 32) & 0xffffffff;
	low32 = htonl(low32);
	high32 = htonl(high32);

	val = 0;
	val = val | low32;
	val = (val << 32) | high32;

	return val;
}





static int shm_fd = -1;
static char *memory_offset_buffer = NULL;
int gptpinit(void)
{
	shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
	if (shm_fd == -1) {
		perror("shm_open()");
		return false;
	}
	memory_offset_buffer =
	    (char *)mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			 shm_fd, 0);
	if (memory_offset_buffer == (char *)-1) {
		perror("mmap()");
		memory_offset_buffer = NULL;
		shm_unlink(SHM_NAME);
		return false;
	}
	return true;
}

void gptpdeinit(void)
{
	if (memory_offset_buffer != NULL) {
		munmap(memory_offset_buffer, SHM_SIZE);
	}
	if (shm_fd != -1) {
		close(shm_fd);
	}
}

int gptpscaling(gPtpTimeData * td)
{
	pthread_mutex_lock((pthread_mutex_t *) memory_offset_buffer);
	memcpy(td, memory_offset_buffer + sizeof(pthread_mutex_t), sizeof(*td));
	pthread_mutex_unlock((pthread_mutex_t *) memory_offset_buffer);

	fprintf(stderr, "ml_phoffset = %lld, ls_phoffset = %lld\n",
		td->ml_phoffset, td->ls_phoffset);
	fprintf(stderr, "ml_freqffset = %d, ls_freqoffset = %d\n",
		td->ml_freqoffset, td->ls_freqoffset);

	return true;
}
int mrp_join_listener(uint8_t * streamid);
int send_mrp_msg(char *notify_data, int notify_len)
{
	struct sockaddr_in addr;
	socklen_t addr_len;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MRPD_PORT_DEFAULT);
	inet_aton("127.0.0.1", &addr.sin_addr);
	addr_len = sizeof(addr);
	if (control_socket != -1)
		return (sendto
			(control_socket, notify_data, notify_len, 0,
			 (struct sockaddr *)&addr, addr_len));

	else
		return (0);
}

int mrp_connect()
{
	struct sockaddr_in addr;
	int sock_fd = -1;
	sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0)
		goto out;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MRPD_PORT_DEFAULT);
	inet_aton("127.0.0.1", &addr.sin_addr);
	memset(&addr, 0, sizeof(addr));
	control_socket = sock_fd;
	return (0);
 out:	if (sock_fd != -1)
		close(sock_fd);
	sock_fd = -1;
	return (-1);
}

int mrp_disconnect()
{
	char *msgbuf;
	int rc;
	msgbuf = malloc(64);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 64);
	sprintf(msgbuf, "BYE");
	mrp_okay = 0;
	rc = send_mrp_msg(msgbuf, 1500);

	/* rc = recv_mrp_okay(); */
	free(msgbuf);
	return rc;
}

int recv_mrp_okay()
{
	while ((mrp_okay == 0) && (mrp_error == 0))
		usleep(20000);
	return 0;
}

int mrp_register_domain(int *class_id, int *priority, u_int16_t * vid)
{
	char *msgbuf;
	int rc;
	msgbuf = malloc(64);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 64);
	sprintf(msgbuf, "S+D:C=%d,P=%d,V=%04x", *class_id, *priority, *vid);
	mrp_okay = 0;
	rc = send_mrp_msg(msgbuf, 1500);

	/* rc = recv_mrp_okay(); */
	free(msgbuf);
	return rc;
}

int send_ready()
{
	char *databuf;
	int rc;
	databuf = malloc(1500);
	if (NULL == databuf)
		return -1;
	memset(databuf, 0, 1500);
	sprintf(databuf, "S+L:L=%02x%02x%02x%02x%02x%02x%02x%02x, D=2",
		     control_stream_id[0], control_stream_id[1],
		     control_stream_id[2], control_stream_id[3],
		     control_stream_id[4], control_stream_id[5],
		     control_stream_id[6], control_stream_id[7]);
	rc = send_mrp_msg(databuf, 1500);

#ifdef DEBUG
	fprintf(stdout,"Ready-Msg: %s\n", databuf);
#endif 

	free(databuf);
	return rc;
}


int mrp_get_domain(int *class_a_id, int *a_priority, u_int16_t * a_vid,
		   int *class_b_id, int *b_priority, u_int16_t * b_vid)
{
	char *msgbuf;

	/* we may not get a notification if we are joining late,
	 * so query for what is already there ...
	 */
	msgbuf = malloc(64);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 64);
	sprintf(msgbuf, "S??");
	send_mrp_msg(msgbuf, 64);
	free(msgbuf);
	while (!halt_tx && (domain_a_valid == 0) && (domain_b_valid == 0))
		usleep(20000);
	*class_a_id = 0;
	*a_priority = 0;
	*a_vid = 0;
	*class_b_id = 0;
	*b_priority = 0;
	*b_vid = 0;
	if (domain_a_valid) {
		*class_a_id = domain_class_a_id;
		*a_priority = domain_class_a_priority;
		*a_vid = domain_class_a_vid;
	}
	if (domain_b_valid) {
		*class_b_id = domain_class_b_id;
		*b_priority = domain_class_b_priority;
		*b_vid = domain_class_b_vid;
	}
	return (0);
}
unsigned char monitor_stream_id[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

int mrp_await_listener(unsigned char *streamid)
{
	char *msgbuf;
	memcpy(monitor_stream_id, streamid, sizeof(monitor_stream_id));
	msgbuf = malloc(64);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 64);
	sprintf(msgbuf, "S??");
	send_mrp_msg(msgbuf, 64);
	free(msgbuf);

	/* either already there ... or need to wait ... */
	while (!halt_tx && (listeners == 0))
		usleep(20000);
	return (0);
}

int process_mrp_msg(char *buf, int buflen)
{

	/*
	 * 1st character indicates application
	 * [MVS] - MAC, VLAN or STREAM
	 */
	unsigned int id;
	unsigned int priority;
	unsigned int vid;
	int i, j, k,m;
	unsigned int substate;
	unsigned char recovered_streamid[8];
	int l = 0;
        k = 0;
        
	if ('S' == buf[l++] && 'N' == buf[l++] && 'E' == buf[l++] && 'T' == buf[++l])
	{
		while ('S' != buf[l++]);
		l++;
		for( m = 0; m < 8 ; l+=2, m++)
		{
			sscanf(&buf[l],"%02x",&id);
			control_stream_id[m] = (unsigned char)id;
                        printf("control message is received with ID = %2x\n",control_stream_id[m]);
		}
		talker = 1;
                
	}
        printf("message is received\n");
 next_line:if (k >= buflen)
		return (0);
	switch (buf[k]) {
	case 'E':
		printf("%s from mrpd\n", buf);
		fflush(stdout);
		mrp_error = 1;
		break;
	case 'O':
		mrp_okay = 1;
		break;
	case 'M':
	case 'V':
		printf("%s unhandled from mrpd\n", buf);
		fflush(stdout);

		/* unhandled for now */
		break;
	case 'L':

		/* parse a listener attribute - see if it matches our monitor_stream_id */
		i = k;
		while (buf[i] != 'D')
			i++;
		i += 2;		/* skip the ':' */
		sscanf(&(buf[i]), "%d", &substate);
		while (buf[i] != 'S')
			i++;
		i += 2;		/* skip the ':' */
		for (j = 0; j < 8; j++) {
			sscanf(&(buf[i + 2 * j]), "%02x", &id);
			recovered_streamid[j] = (unsigned char)id;
		} printf
		    ("FOUND STREAM ID=%02x%02x%02x%02x%02x%02x%02x%02x ",
		     recovered_streamid[0], recovered_streamid[1],
		     recovered_streamid[2], recovered_streamid[3],
		     recovered_streamid[4], recovered_streamid[5],
		     recovered_streamid[6], recovered_streamid[7]);
		switch (substate) {
		case 0:
			printf("with state ignore\n");
			break;
		case 1:
			printf("with state askfailed\n");
			break;
		case 2:
			printf("with state ready\n");
			break;
		case 3:
			printf("with state readyfail\n");
			break;
		default:
			printf("with state UNKNOWN (%d)\n", substate);
			break;
		}
		if (substate > MSRP_LISTENER_ASKFAILED) {
			if (memcmp
			    (recovered_streamid, monitor_stream_id,
			     sizeof(recovered_streamid)) == 0) {
				listeners = 1;
				printf("added listener\n");
			}
		}
		fflush(stdout);

		/* try to find a newline ... */
		while ((i < buflen) && (buf[i] != '\n') && (buf[i] != '\0'))
			i++;
		if (i == buflen)
			return (0);
		if (buf[i] == '\0')
			return (0);
		i++;
		k = i;
		goto next_line;
		break;
	case 'D':
		i = k + 4;

		/* save the domain attribute */
		sscanf(&(buf[i]), "%d", &id);
		while (buf[i] != 'P')
			i++;
		i += 2;		/* skip the ':' */
		sscanf(&(buf[i]), "%d", &priority);
		while (buf[i] != 'V')
			i++;
		i += 2;		/* skip the ':' */
		sscanf(&(buf[i]), "%x", &vid);
		if (id == 6) {
			domain_class_a_id = id;
			domain_class_a_priority = priority;
			domain_class_a_vid = vid;
			domain_a_valid = 1;
		} else {
			domain_class_b_id = id;
			domain_class_b_priority = priority;
			domain_class_b_vid = vid;
			domain_b_valid = 1;
		}
		while ((i < buflen) && (buf[i] != '\n') && (buf[i] != '\0'))
			i++;
		if ((i == buflen) || (buf[i] == '\0'))
			return (0);
		i++;
		k = i;
		goto next_line;
		break;
	case 'T':

		/* as simple_talker we don't care about other talkers */
		i = k;
		while ((i < buflen) && (buf[i] != '\n') && (buf[i] != '\0'))
			i++;
		if (i == buflen)
			return (0);
		if (buf[i] == '\0')
			return (0);
		i++;
		k = i;
		goto next_line;
		break;
	case 'S':

		/* handle the leave/join events */
		switch (buf[k + 4]) {
		case 'L':
			i = k + 5;
			while (buf[i] != 'D')
				i++;
			i += 2;	/* skip the ':' */
			sscanf(&(buf[i]), "%d", &substate);
			while (buf[i] != 'S')
				i++;
			i += 2;	/* skip the ':' */
			for (j = 0; j < 8; j++) {
				sscanf(&(buf[i + 2 * j]), "%02x", &id);
				recovered_streamid[j] = (unsigned char)id;
			} printf
			    ("EVENT on STREAM ID=%02x%02x%02x%02x%02x%02x%02x%02x ",
			     recovered_streamid[0], recovered_streamid[1],
			     recovered_streamid[2], recovered_streamid[3],
			     recovered_streamid[4], recovered_streamid[5],
			     recovered_streamid[6], recovered_streamid[7]);
			switch (substate) {
			case 0:
				printf("with state ignore\n");
				break;
			case 1:
				printf("with state askfailed\n");
				break;
			case 2:
				printf("with state ready\n");
				break;
			case 3:
				printf("with state readyfail\n");
				break;
			default:
				printf("with state UNKNOWN (%d)\n", substate);
				break;
			}
			switch (buf[k + 1]) {
			case 'L':
				printf("got a leave indication\n");
				if (memcmp
				    (recovered_streamid, monitor_stream_id,
				     sizeof(recovered_streamid)) == 0) {
					listeners = 0;
					printf("listener left\n");
				}
				break;
			case 'J':
			case 'N':
				printf("got a new/join indication\n");
				if (substate > MSRP_LISTENER_ASKFAILED) {
					if (memcmp
					    (recovered_streamid,
					     monitor_stream_id,
					     sizeof(recovered_streamid)) == 0)
						listeners = 1;
				}
				break;
			}

			/* only care about listeners ... */
		default:
			return (0);
			break;
		}
		break;
	case '\0':
		break;
	}
	return (0);
}

int await_talker()
{
	while (talker != 1){
          usleep(1000);
        }	
		
	return 0;
}
void *mrp_monitor_thread(void *arg)
{
	char *msgbuf;
	struct sockaddr_in client_addr;
	struct msghdr msg;
	struct iovec iov;
	int bytes = 0;
	struct pollfd fds;
	int rc;
	if (NULL == arg)
		rc = 0;

	else
		rc = 1;
	msgbuf = (char *)malloc(MAX_MRPD_CMDSZ);
	if (NULL == msgbuf)
		return NULL;
	while (!halt_tx) {
		fds.fd = control_socket;
		fds.events = POLLIN;
		fds.revents = 0;
		rc = poll(&fds, 1, 100);
		if (rc < 0) {
			free(msgbuf);
			pthread_exit(NULL);
		}
		if (rc == 0)
			continue;
		if ((fds.revents & POLLIN) == 0) {
			free(msgbuf);
			pthread_exit(NULL);
		}
		memset(&msg, 0, sizeof(msg));
		memset(&client_addr, 0, sizeof(client_addr));
		memset(msgbuf, 0, MAX_MRPD_CMDSZ);
		iov.iov_len = MAX_MRPD_CMDSZ;
		iov.iov_base = msgbuf;
		msg.msg_name = &client_addr;
		msg.msg_namelen = sizeof(client_addr);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		bytes = recvmsg(control_socket, &msg, 0);
		if (bytes < 0)
			continue;
		process_mrp_msg(msgbuf, bytes);
	}
	free(msgbuf);
	pthread_exit(NULL);
}

int mrp_monitor()
{
	pthread_attr_init(&monitor_attr);
	pthread_create(&monitor_thread, NULL, mrp_monitor_thread, NULL);
	return (0);
}

int mrp_join_listener(uint8_t * streamid)
{
	char *msgbuf;
	int rc;
	msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "S+L:S=%02X%02X%02X%02X%02X%02X%02X%02X"
		",D=2", streamid[0], streamid[1], streamid[2], streamid[3],
		streamid[4], streamid[5], streamid[6], streamid[7]);
	mrp_okay = 0;
	rc = send_mrp_msg(msgbuf, 1500);

	/* rc = recv_mrp_okay(); */
	free(msgbuf);
	return rc;
}

int
mrp_advertise_stream(uint8_t * streamid,
		     uint8_t * destaddr,
		     u_int16_t vlan,
		     int pktsz, int interval, int priority, int latency)
{
	char *msgbuf;
	int rc;
	msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "S++:S=%02X%02X%02X%02X%02X%02X%02X%02X"
		",A=%02X%02X%02X%02X%02X%02X"
		",V=%04X"
		",Z=%d"
		",I=%d"
		",P=%d"
		",L=%d", streamid[0], streamid[1], streamid[2],
		streamid[3], streamid[4], streamid[5], streamid[6],
		streamid[7], destaddr[0], destaddr[1], destaddr[2],
		destaddr[3], destaddr[4], destaddr[5], vlan, pktsz,
		interval, priority << 5, latency);
	mrp_okay = 0;
	rc = send_mrp_msg(msgbuf, 1500);

	/* rc = recv_mrp_okay(); */
	free(msgbuf);
	return rc;
}

int
mrp_unadvertise_stream(uint8_t * streamid,
		       uint8_t * destaddr,
		       u_int16_t vlan,
		       int pktsz, int interval, int priority, int latency)
{
	char *msgbuf;
	int rc;
	msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "S--:S=%02X%02X%02X%02X%02X%02X%02X%02X"
		",A=%02X%02X%02X%02X%02X%02X"
		",V=%04X"
		",Z=%d"
		",I=%d"
		",P=%d"
		",L=%d", streamid[0], streamid[1], streamid[2],
		streamid[3], streamid[4], streamid[5], streamid[6],
		streamid[7], destaddr[0], destaddr[1], destaddr[2],
		destaddr[3], destaddr[4], destaddr[5], vlan, pktsz,
		interval, priority << 5, latency);
	mrp_okay = 0;
	rc = send_mrp_msg(msgbuf, 1500);

	/* rc = recv_mrp_okay(); */
	free(msgbuf);
	return rc;
}




void* read_start_feed(void *arg)
{
	
	struct ifreq device;
	int err;
	struct sockaddr_ll ifsock_addr;
	struct packet_mreq mreq;
	int ifindex;
	int socket_d;
	int8_t *ifname;
	uint8_t *frameData;
        if(arg == NULL) {	
		ifname = interface;
		printf("ifname = %s\n",ifname);
		buff_size = 1024;
		
		socket_d = socket(AF_PACKET, SOCK_RAW, htons(ETHER_TYPE_AVTP));
		if (socket_d == -1) {
			printf("failed to open event socket: %s \n", strerror(errno));
			goto cleanret;
		}
		memset(&device, 0, sizeof(device));
		memcpy(device.ifr_name, ifname, IFNAMSIZ);
		err = ioctl(socket_d, SIOCGIFINDEX, &device);
		if (err == -1) {
			printf("Failed to get interface index: %s\n", strerror(errno));
			goto cleanret;
		}

		ifindex = device.ifr_ifindex;
		memset(&ifsock_addr, 0, sizeof(ifsock_addr));
		ifsock_addr.sll_family = AF_PACKET;
		ifsock_addr.sll_ifindex = ifindex;
		ifsock_addr.sll_protocol = htons(ETHER_TYPE_AVTP);
		err = bind(socket_d, (struct sockaddr *) & ifsock_addr, sizeof(ifsock_addr));
		if (err == -1) {
			printf("Call to bind() failed: %s\n", strerror(errno));
			goto cleanret;
		}

		memset(&mreq, 0, sizeof(mreq));
		mreq.mr_ifindex = ifindex;
		mreq.mr_type = PACKET_MR_MULTICAST;
		mreq.mr_alen = 6;
		unsigned char DEST_ADDR[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80 };
		memcpy(mreq.mr_address, DEST_ADDR, mreq.mr_alen);
		err = setsockopt(socket_d, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
		if (err == -1) {
			printf ("Unable to add PTP multicast addresses to port id: %u\n", ifindex);
			goto cleanret;
		}

	
	
		unsigned char frame[FRAME_SIZE];
		int size;
		memset(frame, 0, sizeof(frame));

		nice(-20);
		size = sizeof(ifsock_addr);
		printf("receive from() \n");
		while (!halt_tx) {
		
			err = recvfrom(socket_d, frame, FRAME_SIZE, 0, (struct sockaddr *) &ifsock_addr, (socklen_t *)&size);
			if (err > 0) {
			  
                           frameData = (uint8_t*)((uint8_t*)frame + sizeof(eth_header) + sizeof(seventeen22_controlHeader));
	           
		           if( *frameData == 0x02 ) {
		              audio_data_start = 0;
		              printf("AUDIO data is received to Stop\n");
		           }
		           else if( *frameData == 0x03 ) {
		              audio_data_start = 1;
				printf("AUDIO data is received to Start\n");
		           }
		           else if( *frameData == 0x04 ) {
		              video_data_start = 0;
		              printf("VIDEO data is received to Stop\n");
		           }
		           else if ( *frameData == 0x05 ) {
		             video_data_start = 1;
		             printf("VIDEO data is received to Start\n");
		           }
			} 
		        else {
	 			printf("\nFailed to recieve !!!!!!!!!!!\n");
		        }
		}
	
		close(socket_d);
        }
       
	cleanret:
             pthread_exit(NULL);
	

	return 0;

}

static int32_t avbtp_get_mac_address(int8_t *interface, uint8_t *src_addr)
{
	struct ifreq if_request;
	int lsock;
	int ret;

	lsock = socket(PF_PACKET, SOCK_RAW, htons(0x800));
	if (lsock < 0)
		return -1;

	memset(&if_request, 0, sizeof(if_request));
	strncpy(if_request.ifr_name, (const char*)interface, sizeof(if_request.ifr_name));
	ret = ioctl(lsock, SIOCGIFHWADDR, &if_request);
	if (ret < 0) {
		close(lsock);
		return -1;
	}

	memcpy(src_addr, if_request.ifr_hwaddr.sa_data, MAC_ADDR_LEN);
	close(lsock);

	return 0;
}

int32_t
avbtp_initilaze_header(void *ip_pkt, uint8_t *dst_addr, int8_t *ifname)
{
	eth_header *eptr = ip_pkt;
	uint8_t src_mac_addr[MAC_ADDR_LEN];

	if (!dst_addr || !ifname)
		return -1;

	if (avbtp_get_mac_address(ifname, src_mac_addr))
		return -1;

	memcpy(eptr->h_dest, dst_addr, MAC_ADDR_LEN);
	memcpy(eptr->h_source, src_mac_addr, MAC_ADDR_LEN);
	eptr->h_protocol[0] = 0x22;
	eptr->h_protocol[1] = 0xF0;

	return 0;
}

void sigint_handler(int signum)
{
	printf("got SIGINT\n");
	halt_tx = signum;
        halt_tx = 1;
	close(igb_fd[0]);  
        task_clean(); 
        exit(0);
}

int get_mac_addr(int8_t *interface)
{
	int lsock = socket(PF_PACKET, SOCK_RAW, htons(0x800));
	struct ifreq if_request;
	int rc;

	if (lsock < 0)
		return -1;

	memset(&if_request, 0, sizeof(if_request));
	strncpy(if_request.ifr_name, (const char *)interface, sizeof(if_request.ifr_name));

	rc = ioctl(lsock, SIOCGIFHWADDR, &if_request);
	if (rc < 0) {
		close(lsock);
		return -1;
	}
	memcpy(STATION_ADDR, if_request.ifr_hwaddr.sa_data,
			sizeof(STATION_ADDR));
	close(lsock);

	return 0;
}


int main(int argc, char *argv[])
{
	
	struct igb_packet a_packet;
	struct igb_packet *tmp_packet;
	struct igb_packet *cleaned_packets;
	struct igb_packet *free_packets;
	six1883_header *h61883;
	seventeen22_header *h1722;
	
	unsigned i;
	int err;
	int rc = 0;
	
	
	
	
#ifdef DOMAIN_QUERY
	int class_b_id = 0;
	int b_priority = 0;
	u_int16_t b_vid = 0;
#endif	
	int seqnum;
	unsigned total_samples = 0;
	int32_t read_bytes;
	int64_t total_read_bytes;
	uint8_t *data_ptr;
	void *stream_packet;
	int frame_size;
	
	pthread_t tid,tid1,tid2,tid3;
#ifdef GPTP
        int time_stamp;
        gPtpTimeData td;
        uint64_t now_local, now_8021as;
	uint64_t update_8021as;
	unsigned delta_8021as, delta_local;
	long double ml_ratio; 
	u_int64_t last_time;
#endif
	if (argc < 2) {
		fprintf(stderr,"%s <if_name> <input_file>\n", argv[0]);
		return -EINVAL;
	}
        fileData = argv[2];
        //gst_main(fileData);
	interface = (int8_t *)strdup(argv[1]);
	pkt_sz = 1024;
	payload_len = 1024;
	pkt_sz += sizeof(six1883_header) + sizeof(seventeen22_header) + sizeof(eth_header);

	if (pkt_sz > 1500) {
		fprintf(stderr,"payload_len is > MAX_ETH_PACKET_LEN - not supported.\n");
		return -EINVAL;
	}
	err = mrp_connect();
	if (err) {
		printf("socket creation failed\n");
		return (errno);
	}

	err = pci_connect(&igb_dev);
	if (err) {
		printf("connect failed (%s) - are you running as root?\n", strerror(errno));
		return (errno);
	}
        pthread_mutex_init(&(enq_lock), NULL);
	pthread_mutex_init(&(freeq_lock), NULL); 
	err = igb_init(&igb_dev);
	if (err) {
		printf("init failed (%s) - is the driver really loaded?\n", strerror(errno));
		return (errno);
	}

	err = igb_dma_malloc_page(&igb_dev, &a_page);
	if (err) {
		printf("malloc failed (%s) - out of memory?\n", strerror(errno));
		return (errno);
	}

	signal(SIGINT, sigint_handler);

	rc = get_mac_addr(interface);
	if (rc) {
		printf("failed to open interface(%s)\n",interface);
	}
        /*initizes the threads required for audio and video*/
        mrp_monitor(); 
  
        err = pthread_create(&tid, NULL, decode_data, NULL);
        if (err != 0) {
	    printf("\ncan't create thread :[%s]", strerror(err));
             return(0);
        }


        err = pthread_create(&tid2, NULL, audio_data_read, NULL);
	if (err != 0) {
	    printf("\ncan't create thread :[%s]", strerror(err));
             return(0);
        }

        err = pthread_create(&tid1, NULL, video_data_read, NULL);
	if (err != 0) {
	    printf("\ncan't create thread :[%s]", strerror(err));
            return(0);
        }

	err = pipe (audio_fd);
	if (err < 0) {
	    printf("audio Pipe is not creating \n");
	    return(0);
	}
	err = pipe (video_fd);
	if (err < 0) {
	     printf("video Pipe is not creating \n");
	     return(0);
	}
        err = pipe (igb_fd);
	if (err < 0) {
	     printf("video Pipe is not creating \n");
	     return(0);
	} 

        domain_a_valid = 1;
	class_a_id = MSRP_SR_CLASS_A;
	a_priority = MSRP_SR_CLASS_A_PRIO;
	a_vid = 2;
        mrp_register_domain(&class_a_id, &a_priority, &a_vid);
	igb_set_class_bandwidth(&igb_dev, PACKET_IPG / 125000, 0, pkt_sz - 22, 0);
	memset(STREAM_ID, 0, sizeof(STREAM_ID));
	memcpy(STREAM_ID, STATION_ADDR, sizeof(STATION_ADDR));
	a_packet.dmatime = a_packet.attime = a_packet.flags = 0;
	a_packet.map.paddr = a_page.dma_paddr;
	a_packet.map.mmap_size = a_page.mmap_size;
	a_packet.offset = 0;
	a_packet.vaddr = a_page.dma_vaddr + a_packet.offset;
	a_packet.len = pkt_sz;
	free_packets = NULL;
	seqnum = 0;
	frame_size = payload_len + sizeof(six1883_header) + sizeof(seventeen22_header) + sizeof(eth_header);
	fprintf(stderr,"frame_size = %d\n",frame_size);

	stream_packet = avbtp_create_stream_packet(payload_len);
	avbtp_initilaze_header(stream_packet, DEST_ADDR, interface);
	
        h1722 = (seventeen22_header *)((uint8_t*)stream_packet + sizeof(eth_header));
        h1722->sid_valid = VALID;
        h1722->cd_indicator = 0;
	h1722->subtype = 0;
	h1722->version = 0;
	h1722->reset = 0;
	h1722->reserved0 = 0;
	h1722->gateway_valid = 0;
	h1722->reserved1 = 0;
	h1722->timestamp_uncertain = 0;
        memset(&(h1722->stream_id), 0, sizeof(h1722->stream_id));
        memcpy(&(h1722->stream_id), STATION_ADDR,
		       sizeof(STATION_ADDR));

        h61883 = (six1883_header *)((uint8_t*)stream_packet + sizeof(eth_header) +
				sizeof(seventeen22_header));	        
        h61883->format_tag = 1;
	h61883->packet_channel = 0x1F;
	h61883->packet_tcode = 0xA;
	h61883->app_control = 0x0;
	h61883->reserved0 = 0;
	h61883->source_id = 0x3F;
	h61883->data_block_size = 1;
	h61883->fraction_number = 0;
	h61883->quadlet_padding_count = 0;
	h61883->source_packet_header = 0;
	h61883->reserved1 = 0;
	h61883->eoh = 0x2;
	h61883->format_id = 0x1;
	h61883->syt = 0xFFFF;

	       
        
	/* divide the dma page into buffers for packets */
	for (i = 1; i < ((a_page.mmap_size) / pkt_sz); i++) {
		tmp_packet = malloc(sizeof(struct igb_packet));
		if (NULL == tmp_packet) {
			printf("failed to allocate igb_packet memory!\n");
			return (errno);
		}
		*tmp_packet = a_packet;
		tmp_packet->offset = (i * pkt_sz);
		tmp_packet->vaddr += tmp_packet->offset;
		tmp_packet->next = free_packets;
		memset(tmp_packet->vaddr, 0, pkt_sz);	/* MAC header at least */
		memcpy(((char *)tmp_packet->vaddr), stream_packet, frame_size);
		tmp_packet->len = frame_size;
		free_packets = tmp_packet;
	}

	rc = nice(-20);

#if 1
        mrp_advertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16,
			     PACKET_IPG / 125000, a_priority, 3900);

        fprintf(stderr, "awaiting a listener ...\n");
	mrp_await_listener(STREAM_ID);
	fprintf(stderr,"got a listener ...\n");
	halt_tx = 0;

        /*Wait for the control message talker*/
#endif
        await_talker();
	send_ready();
	printf("got a talker\n");
	//while(1);
#ifdef GPTP 
	gptpinit();
#if 0
	gptpscaling(&td);
	if( igb_get_wallclock( &igb_dev, &now_local, NULL ) != 0 ) {
	  fprintf( stderr, "Failed to get wallclock time\n" );
	  return -1;
	}
	update_8021as = td.local_time - td.ml_phoffset;
	delta_local = (unsigned)(now_local - td.local_time);
	ml_ratio = -1 * (((long double)td.ml_freqoffset) / 1000000000000) + 1;
	delta_8021as = (unsigned)(ml_ratio * delta_local);
	now_8021as = update_8021as + delta_8021as;
	last_time = now_local + XMIT_DELAY;
	time_stamp = now_8021as + RENDER_DELAY;
  #endif
	
  #endif		
	
	err = pthread_create(&tid3, NULL, read_start_feed, NULL);
	if (err != 0)
		printf("\ncan't create thread :[%s]", strerror(err));

	total_read_bytes = 0;
       
        startofinputdata = 1; 
	while (listeners && !halt_tx)
	{
	      	tmp_packet = free_packets;
		if (NULL == tmp_packet)
			goto cleanup;

		stream_packet = ((char *)tmp_packet->vaddr);
		free_packets = tmp_packet->next;

		/* unfortuntely unless this thread is at rtprio
		 * you get pre-empted between fetching the time
		 * and programming the packet and get a late packet
		 */
		h1722 = (seventeen22_header *)((uint8_t*)stream_packet + sizeof(eth_header));
		h1722->seq_number = seqnum++;
		if (seqnum % 4 == 0)
			h1722->timestamp_valid = 0;
		else
			h1722->timestamp_valid = 1;
         #ifdef GPTP    
                gptpscaling(&td);
		if( igb_get_wallclock( &igb_dev, &now_local, NULL ) != 0 ) {
		  fprintf( stderr, "Failed to get wallclock time\n" );
		  return -1;
		}
		update_8021as = td.local_time - td.ml_phoffset;
		delta_local = (unsigned)(now_local - td.local_time);
		ml_ratio = -1 * (((long double)td.ml_freqoffset) / 1000000000000) + 1;
		delta_8021as = (unsigned)(ml_ratio * delta_local);
		now_8021as = update_8021as + delta_8021as;
		last_time = now_local + XMIT_DELAY;
		time_stamp = now_8021as + RENDER_DELAY;
  
                time_stamp = htonl(time_stamp);
		h1722->timestamp = time_stamp;
	  #endif 
		data_ptr = (uint8_t *)((uint8_t*)stream_packet + sizeof(eth_header) + sizeof(seventeen22_header) 
					+ sizeof(six1883_header));
                /*Read the data from the pipe*/



                read_bytes = read_pipe(igb_fd[0],pipe_ptr,payload_len+4 );
                if((read_bytes < payload_len) || (read_bytes < 0) ) {
                 
                    printf("data read from igb pipe is failed\n");
                    continue;
                 }
                 else {
                      syncPtr = (int*)pipe_ptr;
                      memcpy(data_ptr,pipe_ptr+4,read_bytes-4);
                      if(syncPtr[0] == 0xfffe) {       
                         h1722->reserved1 = 0x7;
                      }
                      else if (syncPtr[0] == 0xffff)
                      {	
                        h1722->reserved1 = 0x8;
                
                      }  
                 }	    


		usleep(1000);
		total_read_bytes += read_bytes;
		/* Reached end of file quit the application*/
		if (read_bytes == 0) {
			goto exit_app;
		}
		total_samples += read_bytes;
		h61883 = (six1883_header *)((uint8_t*)stream_packet + sizeof(eth_header) + sizeof(seventeen22_header));
		h61883->data_block_continuity = total_samples;

		err = igb_xmit(&igb_dev, 0, tmp_packet);
		if (!err) {
			count++;		                        
			continue;
		} else {
		
		}

		if (ENOSPC == err) {
			/* put back for now */
			tmp_packet->next = free_packets;
			free_packets = tmp_packet;
		}
cleanup:
		igb_clean(&igb_dev, &cleaned_packets);
		while (cleaned_packets) {
			tmp_packet = cleaned_packets;
			cleaned_packets = cleaned_packets->next;
			tmp_packet->next = free_packets;
			free_packets = tmp_packet;
		}
	}

	rc = nice(0);
	if (halt_tx == 0)
		printf("listener left ...\n");

exit_app:
       
	halt_tx = 1;
        usleep(10000);
       	pthread_exit(NULL);
	return (0);
}

void task_clean (void) {

        igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);
        mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz- 16,
			       PACKET_IPG / 125000, a_priority, 3900);
	igb_dma_free_page(&igb_dev, &a_page);
        mrp_disconnect();
	igb_detach(&igb_dev);
}






char video_buf[4096];
void * audio_data_read(void *arg)
{
	
	int result;
	int *j ; 
        j = (int*)video_buf;
	
        while (!halt_tx) {
	   while(!audio_data_start) {
              usleep(10);
            }	
           *j= AUDIO_DATA;
           result = read_pipe(audio_fd[0],video_buf+4,buff_size);
	    if(result > 0) {
                          
              pthread_mutex_lock(&(enq_lock));
              result = write(igb_fd[1],video_buf,result+4); 
              pthread_mutex_unlock(&(enq_lock));
            }
        }
        close(igb_fd[1]);
        close(audio_fd[0]);
        pthread_exit(NULL);  
	
}
char buf[4096];
void * video_data_read(void *arg)
{
	
	int *j ;
        int result;
	j = (int*)buf;
	*j= VIDEO_DATA;
        
	
	while (!halt_tx) {
	  #if 1
            while(!video_data_start) {
              usleep(10);
            }
            result = read_pipe(video_fd[0],buf+4,buff_size);
            
	    if(result > 0) {
              pthread_mutex_lock(&(enq_lock));
              result = write(igb_fd[1],buf,result+4);
              pthread_mutex_unlock(&(enq_lock)); 
             
           }
          #endif
        
        }  
        close(igb_fd[1]);
        close(video_fd[0]);
	pthread_exit(NULL);  
}


int read_pipe(int fd,void * ptr,int length) 
{
     int count = 0;
     int to_be_read =0;
     int result = 0;
     to_be_read = length;
     char *dataPtr;

     dataPtr = (char*)ptr;
     do {
       result = read(fd,dataPtr+count,to_be_read);
       if(result < 0) {
        count = result;
        printf("error \n");
        break;
       }
       else {
         count = count + result;
         to_be_read = length - count;
       }
     
     }while(count != length);
    
     return count;
}

void* decode_data(void *arg)

{
  
     
     while (!startofinputdata )
     {  

	  usleep(1); 
	     
      }
      gst_main(fileData);
      
      while (!halt_tx) {
      
	usleep(100);
	
      }	
      pthread_exit(NULL);  
  
}  



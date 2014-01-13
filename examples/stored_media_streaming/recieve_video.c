/*
 * Author: Symphony teleca corporation
 * Date: Nov- 10 - 2013
 *
 * File Name: recieve_video.c
 *
 * 
 * This application reads the data from IGB and sends to the Gstreamer pipeline.
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
#include "mrpd.h"
#include "mrp.h"
#include "msrp.h"

#include "avbtp.h"

/* global macros */
#define ETHER_TYPE_AVTP		0x22f0
#define NUM_OF_BUFFERS		4000
#define FRAME_SIZE 1500
#define PACKET_IPG		125000	/* (1) packet every 125 usec */

int video_count = 0; 
int audio_count = 0;

struct tq_entry {
	uint32_t i;
	uint32_t data_length;
	uint8_t *payload_data;
	uint32_t max_buf_size;
	TAILQ_ENTRY(tq_entry) entries;
};

unsigned char STATION_ADDR[] = { 0, 0, 0, 0, 0, 0 };
unsigned char STREAM_ID[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
/* IEEE 1722 reserved address */
unsigned char DEST_ADDR[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80 };

/* external function */
extern int gstreamer_main(char*);
extern int audio_data(unsigned int *dataptr);
unsigned char stream_id[8];
volatile int talker = 0;
int control_socket;
int halt_tx = 0;
volatile int listeners = 0;
volatile int mrp_okay;
volatile int mrp_error = 0;;
int domain_class_a_id;
int domain_class_a_priority;
int domain_class_a_vid;
volatile int domain_b_valid = 0;
int domain_class_b_id;
int domain_class_b_priority;
int domain_class_b_vid;
volatile int domain_a_valid = 0;
int dataStart = 0;
int dataStatus = 0; 
int audio_sync_fd[2];

/* global variables*/
device_t igb_dev;
int recvd_pipe_msg[1024];
int a_priority = 0;

int class_a_id = 0;
u_int16_t a_vid = 0;
struct igb_dma_alloc a_page;
uint32_t pkt_sz;
int start_of_input_data = 0;
int g_exit_app = 0;
pthread_t tid,tid1;
pthread_t monitor_thread;
pthread_attr_t monitor_attr;
pthread_mutex_t video_enq_lock;
pthread_mutex_t video_freeq_lock;
pthread_mutex_t enq_lock;
pthread_mutex_t freeq_lock;
pthread_mutex_t pipe_data_lock;

int mrp_await_listener(unsigned char *streamid);
int
mrp_advertise_stream(uint8_t * streamid,
		     uint8_t * destaddr,
		     u_int16_t vlan,
		     int pktsz, int interval, int priority, int latency);
int mrp_unadvertise_stream(uint8_t * streamid,
		       uint8_t * destaddr,
		       u_int16_t vlan,
		       int pktsz, int interval, int priority, int latency);
int mrp_register_domain(int *class_id, int *priority, u_int16_t * vid);
int send_mrp_msg(char *notify_data, int notify_len);
int get_mac_addr(int8_t *interface);
void *mrp_monitor_thread(void *arg);
int mrp_monitor(void);
int igb_exit(void);
TAILQ_HEAD(t_video_enqueue, tq_entry) video_enqueue;
TAILQ_HEAD(t_video_dequeue, tq_entry) video_freequeue;
TAILQ_HEAD(t_enqueue, tq_entry) enqueue;
TAILQ_HEAD(t_dequeue, tq_entry) freequeue;

struct t_enqueue *h_enq;
struct t_dequeue *h_deq;
struct t_video_enqueue *h_video_enq;
struct t_video_dequeue *h_video_deq;

uint8_t *cbuf_ptr;
uint8_t *cstruct_ptr;
uint8_t *video_cbuf_ptr;
uint8_t *video_cstruct_ptr;
int8_t *ifname;
char *input_type;
int sync_data = 0;

int socket_d;

int create_socket()
{
	struct sockaddr_in addr;
	control_socket = socket(AF_INET, SOCK_DGRAM, 0);
		
	/** in POSIX fd 0,1,2 are reserved */
	if (2 > control_socket)
	{
		if (-1 > control_socket)
			close(control_socket);
	return -1;
	}
	
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(0);
	
	if(0 > (bind(control_socket, (struct sockaddr*)&addr, sizeof(addr)))) 
	{
		fprintf(stderr, "Could not bind socket.\n");
		close(control_socket);
		return -1;
	}
	return 0;
}


int msg_process(char *buf, int buflen)
{
	uint32_t id;
        int j; 
	fprintf(stderr, "Msg: %s bufflength %d \n", buf,buflen);
 	int l = 0;
	if ('S' == buf[l++] && 'N' == buf[l++] && 'E' == buf[l++] && 'T' == buf[++l])
	{
		while ('S' != buf[l++]);
		l++;
		for(j= 0; j < 8 ; l+=2, j++)
		{
			sscanf(&buf[l],"%02x",&id);
			stream_id[j] = (unsigned char)id;
                  
		}
		talker = 1;
                printf("message is received\n"); 
	}
	return (0);
}

int recv_msg()
{
	char *databuf;
	int bytes = 0;

	databuf = (char *)malloc(2000);
	if (NULL == databuf)
		return -1;

	memset(databuf, 0, 2000);
	bytes = recv(control_socket, databuf, 2000, 0);
	if (bytes <= -1) 
	{
		free(databuf);
		return (-1);
	}
	return msg_process(databuf, bytes);

}

int await_talker()
{
	while (0 == talker)	
		recv_msg();
	return 0;
}

int send_msg(char *data, int data_len)
{
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(MRPD_PORT_DEFAULT);
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	inet_aton("127.0.0.1", &addr.sin_addr);
	if (-1 != control_socket)
		return (sendto(control_socket, data, data_len, 0, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)));
	else 
		return 0;
}

int mrp_disconnect()
{
	int rc;
	char *msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "BYE");
	rc = send_msg(msgbuf, 1500);

	free(msgbuf);
	return rc;
}
	
int report_domain_status()
{
	int rc;
	char* msgbuf = malloc(1500);

	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "S+D:C=6,P=3,V=0002");
	
	rc = send_msg(msgbuf, 1500);

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
		     stream_id[0], stream_id[1],
		     stream_id[2], stream_id[3],
		     stream_id[4], stream_id[5],
		     stream_id[6], stream_id[7]);
	rc = send_msg(databuf, 1500);


	printf("Ready-Msg: %s\n", databuf);
 

	free(databuf);
	return rc;
}

int send_leave()
{
	char *databuf;
	int rc;
	databuf = malloc(1500);
	if (NULL == databuf)
		return -1;
	memset(databuf, 0, 1500);
	sprintf(databuf, "S-L:L=%02x%02x%02x%02x%02x%02x%02x%02x, D=3",
		     stream_id[0], stream_id[1],
		     stream_id[2], stream_id[3],
		     stream_id[4], stream_id[5],
		     stream_id[6], stream_id[7]);
	rc = send_msg(databuf, 1500);
	free(databuf);
	return rc;
}



void flag_exit_app(int flag)
{
	g_exit_app = flag;
}

/**
 * init_freequeue_buffer() - Intializes the buffer queue
 * @max_buf_size: the payload size of frame.
 *
 * Returns -EINVAL on error and zero on success.
 */
static int init_freequeue_buffer(int32_t max_buf_size)
{
	int32_t i, ssize;
	struct tq_entry *q[NUM_OF_BUFFERS];

	cbuf_ptr = calloc((max_buf_size * NUM_OF_BUFFERS), sizeof(char));
	if (!cbuf_ptr) {
		printf("calloc failed to allocate memory\n");
		return -EINVAL;
	}

	ssize = sizeof(struct tq_entry);
	cstruct_ptr = calloc(NUM_OF_BUFFERS, ssize);
	if (!cstruct_ptr) {
		printf("calloc failed to allocate memory\n");
		return -EINVAL;
	}

	for (i = 0; i < NUM_OF_BUFFERS; i++) {
		q[i] = (struct tq_entry *) &(cstruct_ptr[i * ssize]);
		q[i]->data_length = 0;
		q[i]->payload_data = &(cbuf_ptr[max_buf_size * i]);
		q[i]->max_buf_size = max_buf_size;
		q[i]->i = i + 1;
		TAILQ_INSERT_TAIL(&freequeue, q[i], entries);
	}

	printf("num of buffers - %d and max buff size - %d\n",NUM_OF_BUFFERS, max_buf_size);

	return 0;
}

/**
 * init_freequeue_buffer() - Intializes the buffer queue
 * @max_buf_size: the payload size of frame.
 *
 * Returns -EINVAL on error and zero on success.
 */

static int init_video_freequeue_buffer(int32_t max_buf_size)
{
	int32_t i, ssize;
	struct tq_entry *q[NUM_OF_BUFFERS];

	video_cbuf_ptr = calloc((max_buf_size * (NUM_OF_BUFFERS)), sizeof(char));
	if (!video_cbuf_ptr) {
		printf("calloc failed to allocate memory\n");
		return -EINVAL;
	}

	ssize = sizeof(struct tq_entry);
	video_cstruct_ptr = calloc((NUM_OF_BUFFERS), ssize);
	if (!video_cstruct_ptr) {
		printf("calloc failed to allocate memory\n");
		return -EINVAL;
	}

	for (i = 0; i < (NUM_OF_BUFFERS); i++) {
		q[i] = (struct tq_entry *) &(video_cstruct_ptr[i * ssize]);
		q[i]->data_length = 0;
		q[i]->payload_data = &(video_cbuf_ptr[max_buf_size * i]);
		q[i]->max_buf_size = max_buf_size;
		q[i]->i = i + 1;
		TAILQ_INSERT_TAIL(&video_freequeue, q[i], entries);
	}

	printf("num of buffers - %d and max buff size - %d\n",(NUM_OF_BUFFERS), max_buf_size);

	return 0;
}


/**
 * free_bufferqueue() - free's the buffer queue created.
 *
 */
static void free_bufferqueue()
{
	printf("Free the buffer queue\n");
	if (cbuf_ptr)
		free(cbuf_ptr);
	if (cstruct_ptr)
		free(cstruct_ptr);
        if (video_cbuf_ptr)
		free(video_cbuf_ptr);
	if (video_cstruct_ptr)
		free(video_cstruct_ptr);

	return;
}


int read_data_from_queue( void* ptr)
{
	struct tq_entry *qptr = NULL;
	int i = 0;
       	nice(-20);
        if( ptr != NULL) {
		if(enqueue.tqh_first) {
		       
		        pthread_mutex_lock(&(enq_lock));
			qptr = enqueue.tqh_first;
			TAILQ_REMOVE(&enqueue, qptr, entries);
			pthread_mutex_unlock(&(enq_lock));
			memcpy(ptr, qptr->payload_data, qptr->data_length);
	
			i = qptr->data_length;

			pthread_mutex_lock(&(freeq_lock));
			qptr->data_length = 0;
			TAILQ_INSERT_TAIL(&freequeue, qptr, entries);
			pthread_mutex_unlock(&(freeq_lock));
            }
	}
	nice(0);
	return i; 
}


int read_data_from_video_queue( void* ptr)
{
	struct tq_entry *qptr = NULL;
	int i = 0;

	nice(-20);
       
        if( ptr != NULL) {
	        if(video_enqueue.tqh_first)  {		
		               
			pthread_mutex_lock(&(video_enq_lock));
			qptr = video_enqueue.tqh_first;
			TAILQ_REMOVE(&video_enqueue, qptr, entries);
			pthread_mutex_unlock(&(video_enq_lock));
			memcpy(ptr, qptr->payload_data, qptr->data_length);
	
			i = qptr->data_length;

			pthread_mutex_lock(&(video_freeq_lock));
			qptr->data_length = 0;
			TAILQ_INSERT_TAIL(&video_freequeue, qptr, entries);
			pthread_mutex_unlock(&(video_freeq_lock));
	        }
         }
	nice(0);
	return i; 
}  
  

void * gstreamer_main_loop(void *arg)
{
       
       while (!start_of_input_data ) {
	     usleep(1);
	}
	/*start the Gstreamer pipeline.*/
	printf("Creating Gstreamer pipeline\n");
	gstreamer_main(input_type);
	while (!halt_tx) {
		usleep(20000);
	}
        pthread_exit(NULL);
	
}

void sigint_handler(int signum)
{
	printf("got SIGINT\n");
	halt_tx = signum;
        halt_tx = 1;
        g_exit_app = 1; 
        send_leave();
        close(socket_d);
        close(audio_sync_fd[0]);
	free_bufferqueue();
        close(control_socket);
        igb_exit();
        exit(0);
                
}
void * transmitter_main_loop(void *arg)
{
        
	struct igb_packet a_packet;
	struct igb_packet *tmp_packet;
	struct igb_packet *cleaned_packets;
	struct igb_packet *free_packets;
	six1883_header *h61883;
	seventeen22_controlHeader *h1722;
	unsigned i,seqnum,frame_size;
	int err,result;
        uint32_t payload_len;
	int rc = 0;
	void *stream_packet;
        uint8_t *data_ptr;
        unsigned total_samples = 0;
	if(arg == NULL) {
        }
        err = igb_dma_malloc_page(&igb_dev, &a_page);
	if (err) {
		printf("malloc failed (%s) - out of memory?\n", strerror(errno));
		return (errno);
	}

	
	rc = get_mac_addr(ifname);
       
        mrp_monitor();
	if (rc) {
		printf("failed to open interface(%s)\n",ifname);
	}
        pkt_sz = 1024;
	payload_len = 1024;
	pkt_sz += sizeof(six1883_header) + sizeof(seventeen22_header) + sizeof(eth_header);

        //domain_a_valid = 1;
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

	frame_size = payload_len + sizeof(seventeen22_controlHeader) + sizeof(eth_header);
	fprintf(stderr,"frame_size = %d\n",frame_size);

	stream_packet = avbtp_create_stream_packet(payload_len);
	avbtp_initilaze_header(stream_packet, DEST_ADDR, ifname);
	
	h1722 = (seventeen22_controlHeader *)((uint8_t*)stream_packet + sizeof(eth_header));
        memset(&(h1722->stream_id), 0, sizeof(h1722->stream_id));
		memcpy(&(h1722->stream_id), STATION_ADDR,
		       sizeof(STATION_ADDR));
	
	
        h1722->sid_valid = VALID;
        h1722->cd_indicator = 1;
	h1722->version = 0;
	h1722->subtype = 0;
        h1722->control_frame_length = 1024;
        h1722->status = 0;
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
        mrp_advertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16,
			     PACKET_IPG / 125000, a_priority, 3900);
	fprintf(stderr, "awaiting a listener ...\n");
	mrp_await_listener(STREAM_ID);
        printf("control listener is joined\n");
        start_of_input_data = 1;  
        halt_tx = 0; 
        while(!halt_tx) {
	   tmp_packet = free_packets;
	   if (NULL == tmp_packet)
		goto cleanup;

           result = read(audio_sync_fd[0],recvd_pipe_msg,512);
           if(result <= 0) {
              printf("pipe data read is failed\n");
              continue;
           }
           printf("pipe data is received\n");
   	   stream_packet = ((char *)tmp_packet->vaddr);
	   free_packets = tmp_packet->next;
	   h1722 = (seventeen22_controlHeader *)((uint8_t*)stream_packet + sizeof(eth_header));
	   
	   data_ptr = (uint8_t *)((uint8_t*)stream_packet + sizeof(eth_header) + sizeof(seventeen22_controlHeader) 
						);
                      
           dataStart = (int)*((int*)recvd_pipe_msg);		   
	   if(dataStart == 0) {
             *data_ptr = 0x02; 
             printf("audio data stop is sent\n");
           }
           
           else if(dataStart == 1) {
             *data_ptr = 0x03;
             printf("audio data start is sent\n");
           }
           else if(dataStart == 2) {
             *data_ptr = 0x04; 
             printf("video data stop is sent\n");
           }   
           else if(dataStart == 3) {
             *data_ptr = 0x05; 
             printf("video data start is sent\n");
           }
           
           else {
              continue;
           } 
          
           err = igb_xmit(&igb_dev, 0, tmp_packet);
	   if (!err) {
             dataStatus = 0;
             
           }
           else {
             
		continue;
		
            } 
		
	    if (ENOSPC == err) {
				/* put back for now */
		tmp_packet->next = free_packets;
		free_packets = tmp_packet;
	     }
	cleanup:
	     igb_clean(&igb_dev, &cleaned_packets);
	      while (cleaned_packets)
              {
				tmp_packet = cleaned_packets;
				cleaned_packets = cleaned_packets->next;
				tmp_packet->next = free_packets;
				free_packets = tmp_packet;
		}
		

       }
        halt_tx = 1;
        printf("enetered halt tx\n");
#if 1	
        mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16, (PACKET_IPG/125000), a_priority, 3900);

	igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);	/* disable Qav */

	rc = mrp_disconnect();

	igb_dma_free_page(&igb_dev, &a_page);

	err = igb_detach(&igb_dev);
#endif
	pthread_exit(NULL);

	return (0);
		
	
}

int igb_exit(void)
{
        int err;
        mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16, (PACKET_IPG/125000), a_priority, 3900);

	igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);	/* disable Qav */

	err = mrp_disconnect();

	igb_dma_free_page(&igb_dev, &a_page);

	err = igb_detach(&igb_dev);
        return err;  


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

int main (int argc, char *argv[ ])
{
	struct ifreq device;
	int err;
	struct sockaddr_ll ifsock_addr;
	struct packet_mreq mreq;
	int ifindex;
	int *join_ptr[3];
	
	uint32_t buff_size;
	seventeen22_header *h1722;
	
	ifname = strdup(argv[1]);
	buff_size = 1024;
	sync_data = (int)atoi(argv[2]);
        input_type = strdup(argv[3]);
       
        if(argc == 0) {
          printf("usage ./recieve_video <interface> <sync> \n");
          return(-1);
        }
        printf("sync_data = %d\n",sync_data);
	if(!((sync_data == 0) || (sync_data == 1))) {
           printf("wrong sync input data. it should be 1 or 0 \n");
           return(-1);
        }	
	err = pci_connect(&igb_dev);
	if (err) {
		printf("connect failed (%s) - are you running as root?\n", strerror(errno));
		return (errno);
	}
         
	err = igb_init(&igb_dev);
	if (err) {
		printf("init failed (%s) - is the driver really loaded?\n", strerror(errno));
		return (errno);
	}

        if (create_socket())
	{
		fprintf(stderr, "Socket creation failed.\n");
		return (errno);
	}
	err = pipe (audio_sync_fd);
  	if (err < 0) {
     		printf("Pipe is not creating \n");
     		return(0);
   	}     

#if 1
	report_domain_status();

	fprintf(stdout,"Waiting for talker...\n");
	await_talker();	

	send_ready();
#endif	

        /*Send the talker related messsages for control message*/
	socket_d = socket(AF_PACKET, SOCK_RAW, htons(ETHER_TYPE_AVTP));
	if (socket_d == -1) {
		printf("failed to open event socket: %s \n", strerror(errno));
		return -1;
	}

	memset(&device, 0, sizeof(device));
	memcpy(device.ifr_name, ifname, IFNAMSIZ);
	err = ioctl(socket_d, SIOCGIFINDEX, &device);
	if (err == -1) {
		printf("Failed to get interface index: %s\n", strerror(errno));
		return -1;
	}

	ifindex = device.ifr_ifindex;
	memset(&ifsock_addr, 0, sizeof(ifsock_addr));
	ifsock_addr.sll_family = AF_PACKET;
	ifsock_addr.sll_ifindex = ifindex;
	ifsock_addr.sll_protocol = htons(ETHER_TYPE_AVTP);
	err = bind(socket_d, (struct sockaddr *) & ifsock_addr, sizeof(ifsock_addr));
	if (err == -1) {
		printf("Call to bind() failed: %s\n", strerror(errno));
		return -1;
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
		return -1;
	}

	TAILQ_INIT(&enqueue);
	TAILQ_INIT(&freequeue);
	pthread_mutex_init(&(enq_lock), NULL);
	pthread_mutex_init(&(freeq_lock), NULL);

        TAILQ_INIT(&video_enqueue);
	TAILQ_INIT(&video_freequeue);
	pthread_mutex_init(&(video_enq_lock), NULL);
	pthread_mutex_init(&(video_freeq_lock), NULL);

        pthread_mutex_init(&(pipe_data_lock), NULL);

	if (init_freequeue_buffer(buff_size) < 0)
		return -EINVAL;

	if (init_video_freequeue_buffer(buff_size) < 0)
		return -EINVAL;

     err = pthread_create(&tid, NULL, gstreamer_main_loop, NULL);
     if(err != 0) {
        printf("\ncan't create thread :[%s]", strerror(err));
        return -EINVAL;
     }
     if(sync_data == 1) {   
        err = pthread_create(&tid1, NULL, transmitter_main_loop, NULL);

     }	
     if (err != 0) {
	printf("\ncan't create thread :[%s]", strerror(err));
        return -EINVAL;
      }

	unsigned char frame[FRAME_SIZE];
	int size;
	memset(frame, 0, sizeof(frame));
        nice(-20);
	size = sizeof(ifsock_addr);
	struct tq_entry *qptr;
	printf("enetering the data received mode\n");
        signal(SIGINT, sigint_handler);
        if(sync_data != 1) {	
          start_of_input_data = 1;
        }
	while (!halt_tx) {
		if (g_exit_app)
			break;
                
		err = recvfrom(socket_d, frame, FRAME_SIZE, 0, (struct sockaddr *) &ifsock_addr, (socklen_t *)&size);
		if (err > 0) {
			
                    
                     h1722 = (seventeen22_header *)((uint8_t*)frame + sizeof(eth_header));
                     

                     if(h1722->reserved1 == 0x08 ) {  
                            
                          if((strcmp(input_type,"AUDIO") == 0) || (strcmp(input_type,"AUDIO_VIDEO") == 0)) {    
		               while (!freequeue.tqh_first) {
				                                   
                                     usleep(10);
                                     printf("audio buffer is full \n");
				}                               
		      		pthread_mutex_lock(&(freeq_lock));
				qptr = freequeue.tqh_first;
				TAILQ_REMOVE(&freequeue, qptr, entries);
			
				qptr->data_length = ntohs(h1722->length) - sizeof(six1883_header);
		                
				memcpy(qptr->payload_data, (uint8_t *)((uint8_t*)frame + sizeof(eth_header) + sizeof(seventeen22_header) 
						+ sizeof(six1883_header)), qptr->data_length);

				pthread_mutex_lock(&(enq_lock));
				TAILQ_INSERT_TAIL(&enqueue, qptr, entries);
				pthread_mutex_unlock(&(enq_lock));
				pthread_mutex_unlock(&(freeq_lock));
	                        audio_count++;
				start_of_input_data = 1;
                          }
                      }
                   
                     else {
                             if(h1722->reserved1 == 0x07 ) {
                             if((strcmp(input_type,"VIDEO") == 0) || (strcmp(input_type,"AUDIO_VIDEO") == 0)) {    
                                
                              video_count++;
                               while (!video_freequeue.tqh_first) {
                                      printf("video buffer is full \n");                                     
				      usleep(10);
				}                              
		      		pthread_mutex_lock(&(video_freeq_lock));
				qptr = video_freequeue.tqh_first;
				TAILQ_REMOVE(&video_freequeue, qptr, entries);
				qptr->data_length = ntohs(h1722->length) - sizeof(six1883_header);
		               
				memcpy(qptr->payload_data, (uint8_t *)((uint8_t*)frame + sizeof(eth_header) + sizeof(seventeen22_header) 
						+ sizeof(six1883_header)), qptr->data_length);
				pthread_mutex_lock(&(video_enq_lock));
				TAILQ_INSERT_TAIL(&video_enqueue, qptr, entries);
				pthread_mutex_unlock(&(video_enq_lock));
				pthread_mutex_unlock(&(video_freeq_lock));
				start_of_input_data = 1;
                             }
                           }
                      }
                    


		} else {
 			printf("\nFailed to recieve !!!!!!!!!!!\n");
                }
	}	
	printf("closing the buffer\n");
	close(socket_d);
	free_bufferqueue();
	pthread_join(tid1,(void**)&(join_ptr[1]));       
        exit(0);
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
	int i, j, k;
	unsigned int substate;
	unsigned char recovered_streamid[8];
	k = 0;
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
			//printf("with state ignore\n");
			break;
		case 1:
			//printf("with state askfailed\n");
			break;
		case 2:
			//printf("with state ready\n");
			break;
		case 3:
			//printf("with state readyfail\n");
			break;
		default:
			//printf("with state UNKNOWN (%d)\n", substate);
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
			} 
                        #if 0
                        printf
			    ("EVENT on STREAM ID=%02x%02x%02x%02x%02x%02x%02x%02x ",
			     recovered_streamid[0], recovered_streamid[1],
			     recovered_streamid[2], recovered_streamid[3],
			     recovered_streamid[4], recovered_streamid[5],
			     recovered_streamid[6], recovered_streamid[7]);
                        #endif
			switch (substate) {
			case 0:
				//printf("with state ignore\n");
				break;
			case 1:
				//printf("with state askfailed\n");
				break;
			case 2:
				//printf("with state ready\n");
				break;
			case 3:
				//printf("with state readyfail\n");
				break;
			default:
				//printf("with state UNKNOWN (%d)\n", substate);
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
				//printf("got a new/join indication\n");
                                // printf("listeners set to 1\n");  
				if (substate > MSRP_LISTENER_ASKFAILED) {
					if (memcmp
					    (recovered_streamid,
					     monitor_stream_id,
					     sizeof(recovered_streamid)) == 0) {
						listeners = 1;
                                           
                                          
                                       }
                                      #if 0 
                                      for (j = 0; j < 8; j++) {
						printf("recovered stream ID = %02x\n", recovered_streamid[j]);
                                                printf("monitor   stream ID = %02x\n", monitor_stream_id[j]);  
						 
					}  
                                      #endif 
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
int send_mrp_msg(char *notify_data, int notify_len)
{
	return(send_msg(notify_data,notify_len));
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


int mrp_monitor(void)
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

int mrp_unadvertise_stream(uint8_t * streamid,
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







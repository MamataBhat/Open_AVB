/*
 * Avbsink plugin: Transmits audio/video data over network using AVB.
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

//Example:
/*  gst-launch-1.0 avbsrc interface=eth3 mediaType=0 fd=0 ! queue ! qtdemux ! decodebin ! autovideosink */


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
#include "../../../../daemons/mrpd/mrpd.h"
#include "../../../../daemons/mrpd/mrp.h"
#include "../../../../daemons/mrpd/msrp.h"

#include "../../../common/avbtp.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>

#ifdef G_OS_WIN32
#include <io.h>                 /* lseek, open, close, read */
#undef lseek
#define lseek _lseeki64
#undef off_t
#define off_t guint64
#endif

#include <sys/stat.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef _MSC_VER
#undef stat
#define stat _stat
#define fstat _fstat
#define S_ISREG(m)	(((m)&S_IFREG)==S_IFREG)
#endif
#include <stdlib.h>
#include <errno.h>

#include "gstavbsrc_listener.h"

/* global macros */
#define ETHER_TYPE_AVTP		0x22f0
#define NUM_OF_BUFFERS		40000
#define FRAME_SIZE 1500
#define PACKET_IPG		125000	/* (1) packet every 125 usec */
#define DEFAULT_INTERFACE "eth0"

#define MRPD 			1

struct tq_entry {
	uint32_t i;
	uint32_t data_length;
	uint8_t *payload_data;
	uint32_t max_buf_size;
	TAILQ_ENTRY(tq_entry) entries;
};

/* IEEE 1722 reserved address */
unsigned char DEST_ADDR[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80 };

/* external function */

extern int audio_data(unsigned int *dataptr);

/* global variables*/

unsigned char STATION_ADDR[] = { 0, 0, 0, 0, 0, 0 };
unsigned char STREAM_ID[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

int video_count = 0; 
int audio_count = 0;

unsigned char stream_id[8];
volatile int talker = 0;
int control_socket;
int socket_d;
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


device_t igb_dev;
device_t *igbptr= NULL;
int recvd_pipe_msg[1024];
int a_priority = 0;
int video_fd[2];
int video1_fd[2];
int class_a_id = 0;
u_int16_t a_vid = 0;
struct igb_dma_alloc a_page;
uint32_t pkt_sz;
int err;
int start_of_input_data = 0;
int dataStopMsgSent = 0;
int dataStartMsgSent = 0;
char *video_start_buf[1024];
int g_exit_app = 0;
//pthread_t tid,tid1,tid3;
pthread_t monitor_thread;
pthread_attr_t monitor_attr;
pthread_mutex_t video_enq_lock;
pthread_mutex_t video_freeq_lock;
pthread_mutex_t enq_lock;
pthread_mutex_t freeq_lock;
pthread_mutex_t pipe_data_lock;
pthread_mutex_t data_lock;
static int mrp_await_listener(unsigned char *streamid);
static int
mrp_advertise_stream(uint8_t * streamid,
		     uint8_t * destaddr,
		     u_int16_t vlan,
		     int pktsz, int interval, int priority, int latency);
static int mrp_unadvertise_stream(uint8_t * streamid,
		       uint8_t * destaddr,
		       u_int16_t vlan,
		       int pktsz, int interval, int priority, int latency);
static int mrp_register_domain(int *class_id, int *priority, u_int16_t * vid);
static int send_mrp_msg(char *notify_data, int notify_len);
static int get_mac_addr(int8_t *interface);
static void *mrp_monitor_thread(void *arg);
static int mrp_monitor(void);
static int igb_exit(void);
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
char *interface1 ;

char *input_type;
int sync_data = 0;

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

GST_DEBUG_CATEGORY_STATIC (gst_avb_src_debug);
#define GST_CAT_DEFAULT gst_avb_src_debug

#define DEFAULT_FD              0
#define DEFAULT_TIMEOUT         0
#define DEFAULT_MEDIATYPE              0
#define DEFAULT_SYNCTYPE             0
enum
{
  PROP_0,

  PROP_FD,
  PROP_INTERFACE,
  PROP_TYPE,
  PROP_DATA_SYNC,
  PROP_TIMEOUT,

  PROP_LAST
};

static void gst_avb_src_uri_handler_init (gpointer g_iface, gpointer iface_data);

#define _do_init \
  G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, gst_avb_src_uri_handler_init); \
  GST_DEBUG_CATEGORY_INIT (gst_avb_src_debug, "avbsrc", 0, "avbsrc element");
#define gst_avb_src_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstAvbSrc, gst_avb_src, GST_TYPE_PUSH_SRC, _do_init);

static void gst_avb_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_avb_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_avb_src_start (GstBaseSrc * bsrc);
static gboolean gst_avb_src_stop (GstBaseSrc * bsrc);
static GstFlowReturn gst_avb_src_create (GstPushSrc * psrc, GstBuffer ** outbuf);


static int create_socket()
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


static int msg_process(char *buf, int buflen)
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

static int recv_msg()
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

int join_vlan()
{
	int rc;
	char *msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "V++:I=0002");
	rc = send_msg(msgbuf, 1500);

	free(msgbuf);
	return rc;
}

static int await_talker()
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

static int mrp_disconnect()
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
	
static int report_domain_status()
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

static int send_ready()
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

static int send_leave()
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



static void flag_exit_app(int flag)
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
	if (cbuf_ptr != NULL) {
		free(cbuf_ptr);
		cbuf_ptr =  NULL; 
        }		
	if (cstruct_ptr != NULL) {
		free(cstruct_ptr);
		cstruct_ptr = NULL; 
	}	
        if (video_cbuf_ptr != NULL){
		free(video_cbuf_ptr);
		video_cbuf_ptr = NULL; 
	}	
	if (video_cstruct_ptr != NULL) {
		free(video_cstruct_ptr);
		video_cstruct_ptr = NULL;
        } 
	
}


static int read_data_from_video_queue( void* ptr)
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
                        
                        pthread_mutex_lock(&(data_lock));
                        video_count--;
                        pthread_mutex_unlock(&(data_lock));
                                                
                        if(video_count <= (250*2)) {
                               if(dataStartMsgSent == 0 ) {

                                        video_start_buf[0] = 3;
                     		        pthread_mutex_lock(&(pipe_data_lock));
					write(audio_sync_fd[1],video_start_buf,512);
					pthread_mutex_unlock(&(pipe_data_lock));
					printf("start feed video\n");
                                        dataStartMsgSent=1;
                                        dataStopMsgSent=0; 
                                 }
                       }                      
	        }
         }
	nice(0);
	return i; 
}  
  

static void sigint_handler(int signum)
{
	printf("got SIGINT\n");
	
        halt_tx = 1;
        g_exit_app = 1; 
        send_leave();
        mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16, (PACKET_IPG/125000), a_priority, 3900);
        mrp_disconnect();
        close(video_fd[1]);
        close(video_fd[0]);
        if(socket_d != -1) {
           shutdown(socket_d,SHUT_RDWR);
           
        }
        if(control_socket != -1) {
            shutdown(control_socket,SHUT_RDWR);
        }
        close(audio_sync_fd[0]);
        close(audio_sync_fd[1]);
	free_bufferqueue();
        
        igb_exit();
       
                
}
char dataRead[1400];
unsigned int timeoutCount = 0;
static void *video_read(void *arg)
{
    int i = 0;
    GstAvbSrc * avbsrc;
    avbsrc = (GstAvbSrc*)arg;
    while(!start_of_input_data) {
      usleep(10);
    }
    while(!halt_tx) {
        i = read_data_from_video_queue(dataRead);
      	if(i == 0) {
          usleep(10);
          if((start_of_input_data == 1) && (dataStartMsgSent == 1)) {
             timeoutCount++;
            
             if( timeoutCount > 10000) {
                close(video_fd[1]);
		close(video_fd[0]);
        	halt_tx = 1;
             }
          }
      	}
        else {
         
         write(avbsrc->write_fd,dataRead,i);
        }
    }     

}

static void * transmitter_main_loop(void *arg)
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
	else {
	   igbptr = &igb_dev;
	}       
	
	err = igb_dma_malloc_page(&igb_dev, &a_page);
	if (err) {
		printf("malloc failed (%s) - out of memory?\n", strerror(errno));
		return (errno);
	}

	printf("interface = %s", interface1);
	rc = get_mac_addr(interface1);
      #ifdef MRPD 
        mrp_monitor();
      #endif
	if (rc) {
		printf("failed to open interface(%s)\n",interface1);
	}
        pkt_sz = 1024;
	payload_len = 1024;
	pkt_sz += sizeof(six1883_header) + sizeof(seventeen22_header) + sizeof(eth_header);

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
	avbtp_initilaze_header(stream_packet, DEST_ADDR, interface1);
	
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
#ifdef MRPD 
        mrp_advertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16,
			     PACKET_IPG / 125000, a_priority, 3900);
	fprintf(stderr, "awaiting a listener ...\n");
	mrp_await_listener(STREAM_ID);
        printf("control listener is joined\n");
#endif
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
	   
	   data_ptr = (uint8_t *)((uint8_t*)stream_packet + sizeof(eth_header) + sizeof(seventeen22_header) 
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
        
        #if 0
#ifdef MRPD 	
        mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16, (PACKET_IPG/125000), a_priority, 3900);
#endif
	igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);	/* disable Qav */

	rc = mrp_disconnect();

	igb_dma_free_page(&igb_dev, &a_page);

	err = igb_detach(&igb_dev);
 #endif
	pthread_exit(NULL);

	return (0);
		
	
}

static int igb_exit(void)
{
        int err;
        if(igbptr != NULL) {
		igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);	/* disable Qav */
	
		igb_dma_free_page(&igb_dev, &a_page);

	         err = igb_detach(&igb_dev);
	        igbptr = NULL;
	}	
        return err;  


}



static int get_mac_addr(int8_t *interface)
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

static void * main_loop(void *arg)
{
	struct ifreq device;
	int err;
	struct sockaddr_ll ifsock_addr;
	struct packet_mreq mreq;
	int ifindex;
	int *join_ptr[3];
        int length;
        GstAvbSrc * avbsrc;
	uint32_t buff_size;
	seventeen22_header *h1722;
        avbsrc = (GstAvbSrc*)arg;
        pthread_t tid,tid1,tid3;
	buff_size = 1024;

        while (!avbsrc->dataStart ) {
          usleep(10);
        }
	sync_data = avbsrc->dataSync;
        printf("main loop \n");
        printf("sync_data = %d\n",sync_data);
        printf("mediaType main = %d\n",avbsrc->mediaType); 
	if(!((sync_data == 0) || (sync_data == 1))) {
           printf("wrong sync input data. it should be 1 or 0 \n");
           return(-1);
        }
        signal(SIGINT, sigint_handler);	
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

#ifdef MRPD
	report_domain_status();

	join_vlan();

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
	printf("Main_loop interface = %s", interface1);
	memcpy(device.ifr_name, interface1, IFNAMSIZ);
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
	unsigned char DEST_ADDR[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x82 };
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
        pthread_mutex_init(&(data_lock), NULL);
        pthread_mutex_init(&(pipe_data_lock), NULL);

	if (init_freequeue_buffer(buff_size) < 0)
		return -EINVAL;

	if (init_video_freequeue_buffer(buff_size) < 0)
		return -EINVAL;
  
     if(sync_data == 1) {   
        err = pthread_create(&tid1, NULL, transmitter_main_loop, NULL);
     }
     if (err != 0) {
	printf("\ncan't create thread :[%s]", strerror(err));
        return -EINVAL;
      }    
     err = pthread_create(&tid3, NULL, video_read, avbsrc);
     
     if (err != 0) {
	printf("\ncan't create thread :[%s]", strerror(err));
        return -EINVAL;
      }

	unsigned char frame[FRAME_SIZE];
	int size,frameId;
	memset(frame, 0, sizeof(frame));
        nice(-20);
	size = sizeof(ifsock_addr);
	struct tq_entry *qptr;
	printf("enetering the data received mode\n");
        
        if(sync_data != 1) {	
          start_of_input_data = 1;
        }
        frameId = 0; 
	while (!halt_tx) {
		if (g_exit_app)
			break;
               
		err = recvfrom(socket_d, frame, FRAME_SIZE, 0, (struct sockaddr *) &ifsock_addr, (socklen_t *)&size);
		if (err > 0) {
			
                     
                     frameId++;
                     timeoutCount = 0;
                     h1722 = (seventeen22_header *)((uint8_t*)frame + sizeof(eth_header));
                              if(avbsrc->dataSync == 1 ) {                      

		                       while ((!video_freequeue.tqh_first) && (!halt_tx) ) {                                    
					      usleep(10);
					}
					if(halt_tx) {
					   break;
					}
		                        pthread_mutex_lock(&(data_lock));
		                	video_count++;  
		                	pthread_mutex_unlock(&(data_lock));
		                                                    
			      		pthread_mutex_lock(&(video_freeq_lock));
					qptr = video_freequeue.tqh_first;
					TAILQ_REMOVE(&video_freequeue, qptr, entries);
					qptr->data_length = ntohs(h1722->length) - sizeof(six1883_header);
				       
					memcpy(qptr->payload_data, (uint8_t *)((uint8_t*)frame + sizeof(eth_header) + sizeof(seventeen22_header) 
							+ sizeof(six1883_header)), qptr->data_length);
		                      
		                        if(video_count >= (1750*2)) {
		                           if(dataStopMsgSent == 0 ) {

		                                printf("inside stop msg sent \n");
		                                video_start_buf[0] = 2;
		             		        pthread_mutex_lock(&(pipe_data_lock));
						write(audio_sync_fd[1],video_start_buf,512);
						pthread_mutex_unlock(&(pipe_data_lock));
						printf("stop feed video\n");
		                                dataStopMsgSent =1;
		                                dataStartMsgSent=0; 
		                           }
		                        }
		                        pthread_mutex_lock(&(video_enq_lock));
		                  	TAILQ_INSERT_TAIL(&video_enqueue, qptr, entries);
					pthread_mutex_unlock(&(video_enq_lock));
					pthread_mutex_unlock(&(video_freeq_lock));
					#if 1
					start_of_input_data = 1;
					#endif
                                }
                                else {
                                     write(avbsrc->write_fd,(uint8_t *)((uint8_t*)frame + sizeof(eth_header) + sizeof(seventeen22_header) 
						+ sizeof(six1883_header)), length);
                                     start_of_input_data = 1; 
                                 } 

                    

		} else {
 			printf("\nFailed to recieve !!!!!!!!!!!\n");
                }
	}	
	printf("closing the buffer\n");
	igb_exit();
	free_bufferqueue();
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

static int32_t
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




static int mrp_register_domain(int *class_id, int *priority, u_int16_t * vid)
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

static int mrp_await_listener(unsigned char *streamid)
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

static int process_mrp_msg(char *buf, int buflen)
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
					#if 0
					close(video_fd[1]);
        				close(video_fd[0]);
        				halt_tx = 1;
					#endif
				}
				break;
			case 'J':
			case 'N':
				//printf("got a new/join indication\n");
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

static int send_mrp_msg(char *notify_data, int notify_len)
{
	return(send_msg(notify_data,notify_len));
}

static void *mrp_monitor_thread(void *arg)
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


static int mrp_monitor(void)
{
	pthread_attr_init(&monitor_attr);
	pthread_create(&monitor_thread, NULL, mrp_monitor_thread, NULL);
	return (0);
}

static int mrp_join_listener(uint8_t * streamid)
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

static int
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

static int mrp_unadvertise_stream(uint8_t * streamid,
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


static void
gst_avb_src_class_init (GstAvbSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpush_src_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  gstbasesrc_class = GST_BASE_SRC_CLASS (klass);
  gstpush_src_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_avb_src_set_property;
  gobject_class->get_property = gst_avb_src_get_property;

  g_object_class_install_property (gobject_class, PROP_FD,
      g_param_spec_int ("fd", "fd", "An open file descriptor to read",
          0, G_MAXINT, DEFAULT_FD, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_INTERFACE,
	g_param_spec_string ("interface", "Interface","Ethernet AVB Interface",
			     interface1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_TYPE,
	g_param_spec_int ("mediaType", "mediaType","Ethernet AVB Media Type",
			     0, G_MAXINT, DEFAULT_MEDIATYPE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_DATA_SYNC,
	g_param_spec_int ("dataSync", "dataSyncType","Ethernet AVB Data Sync",
			     0, G_MAXINT, DEFAULT_SYNCTYPE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class,
      "Filedescriptor Source",
      "Source/File",
      "Read from a file descriptor", "Erik Walthinsen <omega@cse.ogi.edu>");
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&srctemplate));

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_avb_src_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_avb_src_stop);
  gstpush_src_class->create = GST_DEBUG_FUNCPTR (gst_avb_src_create);
}
 pthread_t tid;
static void
gst_avb_src_init (GstAvbSrc * avbsrc)
{
 
  avbsrc->new_fd = DEFAULT_FD;
  avbsrc->interface = interface1;
  avbsrc->mediaType = 0;
  avbsrc->dataSync = 0;
  avbsrc->seekable_fd = FALSE;
  avbsrc->fd = -1;
  avbsrc->size = -1;
  avbsrc->timeout = DEFAULT_TIMEOUT;
  avbsrc->uri = g_strdup_printf ("fd://0");
  avbsrc->curoffset = 0;
  printf("thread creation \n");
  /* Thread Creation */
#if 1
 err = pthread_create(&tid, NULL, main_loop, avbsrc);

  if (err != 0) 
  	printf("\ncan't create thread :[%s]", strerror(err));
#endif
}

static void
gst_avb_src_update_fd (GstAvbSrc * src, guint64 size)
{
  struct stat stat_results;

  GST_DEBUG_OBJECT (src, "fdset %p, old_fd %d, new_fd %d", src->fdset, src->fd,
      src->new_fd);

  /* we need to always update the fdset since it may not have existed when
   * gst_avb_src_update_fd () was called earlier */
  if (src->fdset != NULL) {
    GstPollFD fd = GST_POLL_FD_INIT;

    if (src->fd >= 0) {
      fd.fd = src->fd;
      /* this will log a harmless warning, if it was never added */
      gst_poll_remove_fd (src->fdset, &fd);
    }

    fd.fd = src->new_fd;
    gst_poll_add_fd (src->fdset, &fd);
    gst_poll_fd_ctl_read (src->fdset, &fd, TRUE);
  }


  if (src->fd != src->new_fd) {
    GST_INFO_OBJECT (src, "Updating to fd %d", src->new_fd);

    src->fd = src->new_fd;

    GST_INFO_OBJECT (src, "Setting size to fd %" G_GUINT64_FORMAT, size);
    src->size = size;

    g_free (src->uri);
    src->uri = g_strdup_printf ("fd://%d", src->fd);

    if (fstat (src->fd, &stat_results) < 0)
      goto not_seekable;

    if (!S_ISREG (stat_results.st_mode))
      goto not_seekable;

    /* Try a seek of 0 bytes offset to check for seekability */
    if (lseek (src->fd, 0, SEEK_CUR) < 0)
      goto not_seekable;

    GST_INFO_OBJECT (src, "marking fd %d as seekable", src->fd);
    src->seekable_fd = TRUE;

    gst_base_src_set_dynamic_size (GST_BASE_SRC (src), TRUE);
  }
  return;

not_seekable:
  {
    GST_INFO_OBJECT (src, "marking fd %d as NOT seekable", src->fd);
    src->seekable_fd = FALSE;
    gst_base_src_set_dynamic_size (GST_BASE_SRC (src), FALSE);
  }
}

static gboolean
gst_avb_src_start (GstBaseSrc * bsrc)
{
  pthread_t tid;
  GstAvbSrc *src = GST_AVB_SRC (bsrc);
  printf("started \n");
  src->curoffset = 0;

  if ((src->fdset = gst_poll_new (TRUE)) == NULL)
    goto socket_pair;

  gst_avb_src_update_fd (src, -1);
  src->dataStart = 1;
  return TRUE;

  /* ERRORS */
socket_pair:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ_WRITE, (NULL),
        GST_ERROR_SYSTEM);
    return FALSE;
  }
}


static gboolean
gst_avb_src_stop (GstBaseSrc * bsrc)
{
  pthread_t tid;
  GstAvbSrc *src = GST_AVB_SRC (bsrc);
  printf("stop \n");
  sigint_handler(1);
  return TRUE;

  /* ERRORS */

}

int tempCnt = 0;
static void
gst_avb_src_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstAvbSrc *src = GST_AVB_SRC (object);
  int err; 
  printf("set property \n");
#if 1
  if(tempCnt == 0) {   
	err = pipe (video_fd);
		if (err < 0) {
		     printf("video Pipe is not creating \n");
		     return(0);
	   }
	   err = pipe (video1_fd);
		if (err < 0) {
		     printf("video Pipe is not creating \n");
		     return(0);
	   } 
	   printf("pipe is created\n");
        tempCnt++;
  }
#endif
  switch (prop_id) {
    case PROP_FD:
      src->new_fd = video_fd[0]; 
      src->write_fd= video_fd[1]; 
      if(src->mediaType == 1) {
          src->new_fd = video1_fd[0];
          src->write_fd= video1_fd[1]; 
       }
       
      /* If state is ready or below, update the current fd immediately
       * so it is reflected in get_properties and uri */
      GST_OBJECT_LOCK (object);
      if (GST_STATE (GST_ELEMENT (src)) <= GST_STATE_READY) {
        GST_DEBUG_OBJECT (src, "state ready or lower, updating to use new fd");
        gst_avb_src_update_fd (src, -1);
      } else {
        GST_DEBUG_OBJECT (src, "state above ready, not updating to new fd yet");
      }
      GST_OBJECT_UNLOCK (object);
      break;
    case PROP_INTERFACE:
	g_free(interface1);
	if (g_value_get_string (value) == NULL)
		interface1 = g_strdup (DEFAULT_INTERFACE);
	else
		interface1 = g_value_dup_string (value);
                
	break;
    case PROP_TYPE:
	
        src->mediaType = g_value_get_int (value);
        if(src->mediaType == 1) {
                  src->new_fd = video1_fd[0]; 
		      /* If state is ready or below, update the current fd immediately
		       * so it is reflected in get_properties and uri */
		      GST_OBJECT_LOCK (object);
		      if (GST_STATE (GST_ELEMENT (src)) <= GST_STATE_READY) {
			GST_DEBUG_OBJECT (src, "state ready or lower, updating to use new fd");
			gst_avb_src_update_fd (src, -1);
		      } else {
			GST_DEBUG_OBJECT (src, "state above ready, not updating to new fd yet");
		      }
		      GST_OBJECT_UNLOCK (object);
                }

	break;
    case PROP_DATA_SYNC:
       
       src->dataSync = g_value_get_int (value);
       printf("data sync value at property = %d\n", src->dataSync );
	break;
    case PROP_TIMEOUT:
      src->timeout = g_value_get_uint64 (value);
      GST_DEBUG_OBJECT (src, "poll timeout set to %" GST_TIME_FORMAT,
          GST_TIME_ARGS (src->timeout));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_avb_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstAvbSrc *src = GST_AVB_SRC (object);

  switch (prop_id) {
    case PROP_FD:
      g_value_set_int (value, src->fd);
      break;
    case PROP_INTERFACE:
      g_value_set_string (value, interface1);
      break; 
    case PROP_TYPE:
      g_value_set_int (value, src->mediaType);
      break; 
     case PROP_DATA_SYNC:
      g_value_set_int (value, src->dataSync);
      break;
     case PROP_TIMEOUT:
      g_value_set_uint64 (value, src->timeout);
      break;
     default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstFlowReturn
gst_avb_src_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstAvbSrc *src;
  GstBuffer *buf;
  gssize readbytes;
  guint blocksize;
  GstMapInfo info;

#ifndef HAVE_WIN32
  GstClockTime timeout;
  gboolean try_again;
  gint retval;
#endif

  src = GST_AVB_SRC (psrc);
#if 1
#ifndef HAVE_WIN32
  if (src->timeout > 0) {
    timeout = src->timeout * GST_USECOND;
  } else {
    timeout = GST_CLOCK_TIME_NONE;
  }

  do {
    try_again = FALSE;

    GST_LOG_OBJECT (src, "doing poll, timeout %" GST_TIME_FORMAT,
        GST_TIME_ARGS (src->timeout));

    retval = gst_poll_wait (src->fdset, timeout);
    GST_LOG_OBJECT (src, "poll returned %d", retval);

    if (G_UNLIKELY (retval == -1)) {
      if (errno == EINTR || errno == EAGAIN) {
        /* retry if interrupted */
        try_again = TRUE;
      } else if (errno == EBUSY) {
        goto stopped;
      } else {
        goto poll_error;
      }
    } else if (G_UNLIKELY (retval == 0)) {
      try_again = TRUE;
      /* timeout, post element message */
      gst_element_post_message (GST_ELEMENT_CAST (src),
          gst_message_new_element (GST_OBJECT_CAST (src),
              gst_structure_new ("GstAvbSrcTimeout",
                  "timeout", G_TYPE_UINT64, src->timeout, NULL)));
    }
  } while (G_UNLIKELY (try_again));     /* retry if interrupted or timeout */
#endif
#endif
  blocksize = GST_BASE_SRC (src)->blocksize;

  /* create the buffer */
  buf = gst_buffer_new_allocate (NULL, blocksize, NULL);
  if (G_UNLIKELY (buf == NULL))
    goto alloc_failed;

  gst_buffer_map (buf, &info, GST_MAP_WRITE);
 
  do {
    readbytes = read (src->fd, info.data, blocksize);
    GST_LOG_OBJECT (src, "read %" G_GSSIZE_FORMAT, readbytes);
  } while (readbytes == -1 && errno == EINTR && (!halt_tx));  /* retry if interrupted */
 
  if (readbytes < 0) {
    sigint_handler(1);
    goto read_error;
  }
  if(halt_tx) {
    sigint_handler(1);
    readbytes = 0;
  }
  gst_buffer_unmap (buf, &info);
  gst_buffer_resize (buf, 0, readbytes);
  if (readbytes == 0)
    goto eos;

  GST_BUFFER_OFFSET (buf) = src->curoffset;
  GST_BUFFER_TIMESTAMP (buf) = GST_CLOCK_TIME_NONE;
  src->curoffset += readbytes;

  GST_LOG_OBJECT (psrc, "Read buffer of size %" G_GSSIZE_FORMAT, readbytes);

  /* we're done, return the buffer */
  *outbuf = buf;

  return GST_FLOW_OK;

  /* ERRORS */
#ifndef HAVE_WIN32
poll_error:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL),
        ("poll on file descriptor: %s.", g_strerror (errno)));
    GST_DEBUG_OBJECT (psrc, "Error during poll");
    return GST_FLOW_ERROR;
  }
stopped:
  {
    GST_DEBUG_OBJECT (psrc, "Poll stopped");
    return GST_FLOW_FLUSHING;
  }
#endif
alloc_failed:
  {
    GST_ERROR_OBJECT (src, "Failed to allocate %u bytes", blocksize);
    return GST_FLOW_ERROR;
  }
eos:
  {
    GST_DEBUG_OBJECT (psrc, "Read 0 bytes. EOS.");
    gst_buffer_unref (buf);
    return GST_FLOW_EOS;
  }
read_error:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL),
        ("read on file descriptor: %s.", g_strerror (errno)));
    GST_DEBUG_OBJECT (psrc, "Error reading from fd");
    gst_buffer_unmap (buf, &info);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }
}


/*** GSTURIHANDLER INTERFACE *************************************************/

static GstURIType
gst_avb_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_avb_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { "fd", NULL };

  return protocols;
}

static gchar *
gst_avb_src_uri_get_uri (GstURIHandler * handler)
{
  GstAvbSrc *src = GST_AVB_SRC (handler);

  /* FIXME: make thread-safe */
  return g_strdup (src->uri);
}

static gboolean
gst_avb_src_uri_set_uri (GstURIHandler * handler, const gchar * uri,
    GError ** err)
{
  gchar *protocol, *q;
  GstAvbSrc *src = GST_AVB_SRC (handler);
  gint fd;
  guint64 size = (guint64) - 1;

  GST_INFO_OBJECT (src, "checking uri %s", uri);

  protocol = gst_uri_get_protocol (uri);
  if (strcmp (protocol, "fd") != 0) {
    g_free (protocol);
    return FALSE;
  }
  g_free (protocol);

  if (sscanf (uri, "fd://%d", &fd) != 1 || fd < 0)
    return FALSE;

  if ((q = g_strstr_len (uri, -1, "?"))) {
    gchar *sp;

    GST_INFO_OBJECT (src, "found ?");

    if ((sp = g_strstr_len (q, -1, "size="))) {
      if (sscanf (sp, "size=%" G_GUINT64_FORMAT, &size) != 1) {
        GST_INFO_OBJECT (src, "parsing size failed");
        size = -1;
      } else {
        GST_INFO_OBJECT (src, "found size %" G_GUINT64_FORMAT, size);
      }
    }
  }

  src->new_fd = fd;

  GST_OBJECT_LOCK (src);
  if (GST_STATE (GST_ELEMENT (src)) <= GST_STATE_READY) {
    gst_avb_src_update_fd (src, size);
  }
  GST_OBJECT_UNLOCK (src);

  return TRUE;
}

static void
gst_avb_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_avb_src_uri_get_type;
  iface->get_protocols = gst_avb_src_uri_get_protocols;
  iface->get_uri = gst_avb_src_uri_get_uri;
  iface->set_uri = gst_avb_src_uri_set_uri;
}

static gboolean plugin_init(GstPlugin * plugin)
{
	if (!gst_element_register(plugin, "avbsrc", GST_RANK_NONE,
				  gst_avb_src_get_type()))
		return FALSE;

	return TRUE;

}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	avbsrc,
	"Ethernet AVB Source Element",
	plugin_init,
	"1.2.1",
	"LGPL",
	"GStreamer",
	"https://launchpad.net/distros/ubuntu/+source/gstreamer1.0"
)


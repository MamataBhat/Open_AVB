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
/* gst-launch-1.0 filesrc location=/home/plad/Desktop/gst-plugins-good-1.0.0/input.mp4 ! queue ! avbsink interface=eth3 mediaType=0 dataSync=0 fd=0 */

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
#include "../../../../lib/igb/igb.h"
#include "../../../../daemons/mrpd/mrpd.h"
#include "../../../../daemons/mrpd/mrp.h"
#include "../../../../daemons/mrpd/msrp.h"

#include "../../../common/avbtp.h"


#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

//#include "../../gst/gst-i18n-lib.h"

#include <sys/types.h>

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
#include <errno.h>
#include <string.h>

#include "gstavbsink_talker.h"

#ifdef G_OS_WIN32
#include <io.h>                 /* lseek, open, close, read */
#undef lseek
#define lseek _lseeki64
#undef off_t
#define off_t guint64
#endif


#define ETHER_TYPE_AVTP		0x22f0
#define FRAME_SIZE 1500
#define XMIT_DELAY (200000000)	/* us */
#define RENDER_DELAY (XMIT_DELAY+2000000)	/* us */
#define AUDIO_DATA 0xffff
#define VIDEO_DATA 0xfffe
#define PACKET_IPG		125000	/* (1) packet every 125 usec */

#define DEFAULT_INTERFACE "eth0"

#define MRPD 			1

/* global variables */

static volatile int listeners = 0;
static volatile int mrp_okay;
static volatile int mrp_error = 0;;
static volatile int domain_a_valid = 0;
static volatile int domain_b_valid = 0;

device_t igb_dev;
device_t *ptrigb= NULL;
static int halt_tx = 0;
static int control_socket = -1;
static int domain_class_a_id;
static int domain_class_a_priority;
static int domain_class_a_vid;
static int domain_class_b_id;
static int domain_class_b_priority;
static int domain_class_b_vid;

int startofinputdata = 0;
int audio_data_start = 0;
int video_data_start = 0;



char 	pipe_ptr[1500];
int 	*syncPtr;

static int 	a_priority = 0;
static u_int16_t a_vid = 0;
static int 	class_a_id = 0;
struct 	igb_dma_alloc a_page;
uint32_t pkt_sz;
char 	*interface1 ;
uint32_t mediaType = 0;
uint32_t dataSync  = 0;

int err;
pthread_t tid;
int igb_fd[2];

int read_pipe(int fd,void * ptr,int length) ;
void task_clean (void) ;

pthread_mutex_t enq_lock;
pthread_mutex_t freeq_lock;
pthread_t monitor_thread;
pthread_attr_t monitor_attr;

int socket_d =  -1;
int32_t payload_len;

static unsigned char STATION_ADDR[] = { 0, 0, 0, 0, 0, 0 };
static unsigned char STREAM_ID[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
unsigned char control_stream_id[] = {0, 0, 0, 0, 0, 0, 0, 0 };
/*IEEE 1722 reserved address */
static unsigned char DEST_ADDR[] = { 0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x82 };
static int talker = 0;
int count  = 0 ;


static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

GST_DEBUG_CATEGORY_STATIC (gst_avb_sink__debug);
#define GST_CAT_DEFAULT gst_avb_sink__debug


/* AvbSink signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_FD,
  ARG_INTERFACE,
  ARG_MEDIA_TYPE,
  ARG_DATA_SYNC,
};

static void gst_avb_sink_uri_handler_init (gpointer g_iface,
    gpointer iface_data);

#define _do_init \
  G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, gst_avb_sink_uri_handler_init); \
  GST_DEBUG_CATEGORY_INIT (gst_avb_sink__debug, "avbsink", 0, "avbsink element");
#define gst_avb_sink_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstAvbSink, gst_avb_sink, GST_TYPE_BASE_SINK, _do_init);

static void gst_avb_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_avb_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstFlowReturn gst_avb_sink_render (GstBaseSink * sink,
    GstBuffer * buffer);
static gboolean gst_avb_sink_start (GstBaseSink * basesink);
static gboolean gst_avb_sink_stop (GstBaseSink * basesink);
static gboolean gst_avb_sink_do_seek (GstAvbSink * avbsink, guint64 new_offset);

/* avb code starts here */

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

int mrp_join_vlan()
{
	char *msgbuf;
	int rc;
	msgbuf = malloc(1500);
	if (NULL == msgbuf)
		return -1;
	memset(msgbuf, 0, 1500);
	sprintf(msgbuf, "V++:I=0002");
	rc = send_mrp_msg(msgbuf, 1500);

	free(msgbuf);
	return rc;
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

int send_leave()
{
	char *databuf;
	int rc;
	databuf = malloc(1500);
	if (NULL == databuf)
		return -1;
	memset(databuf, 0, 1500);
	sprintf(databuf, "S-L:L=%02x%02x%02x%02x%02x%02x%02x%02x, D=3",
		     control_stream_id[0], control_stream_id[1],
		     control_stream_id[2], control_stream_id[3],
		     control_stream_id[4], control_stream_id[5],
		     control_stream_id[6], control_stream_id[7]);
	rc = send_mrp_msg(databuf, 1500);
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
static unsigned char monitor_stream_id[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

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
			/*
                         		listeners = 0;
                                        close(igb_fd[1]); 
                                        close(igb_fd[0]);
                                        halt_tx = 1;*/
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
	int8_t *ifname;
	uint8_t *frameData;
        if(arg == NULL) {	
		ifname = interface1;
		printf("ifname = %s\n",ifname);
				
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
			  
                           frameData = (uint8_t*)((uint8_t*)frame + sizeof(eth_header) + sizeof(seventeen22_header));
	           
		           if( *frameData == 0x02 ) {
		              audio_data_start = 0;
		              printf("AUDIO data is received to STOP\n");
		           }
		           else if( *frameData == 0x03 ) {
		              audio_data_start = 1;
				printf("AUDIO data is received to STart\n");
		           }
		           else if( *frameData == 0x04 ) {
		              video_data_start = 0;
		              printf("vIDEO data is received to STOP\n");
		           }
		           else if ( *frameData == 0x05 ) {
		             video_data_start = 1;
		             printf("vIDEO data is received to START\n");
		           }
                           else {
		             
		             printf("Wrong data is received \n");
		          }
			} 
		        else {
	 			printf("\nFailed to recieve !!!!!!!!!!!\n");
		        }
		}
	
		close(socket_d);
                socket_d = -1; 
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
        if(control_socket  != -1 ){
        	send_leave();
        	mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz- 16,
				       PACKET_IPG / 125000, a_priority, 3900);
		mrp_disconnect();
        }        
         halt_tx = 1;
         video_data_start = 1;
         close(igb_fd[1]); 
         close(igb_fd[0]);
         if(control_socket  != -1 ) {
		
                shutdown(control_socket,SHUT_RDWR);
                          
                control_socket = -1;
         }
         if(socket_d != -1 ) {
           shutdown(socket_d,SHUT_RDWR);
           socket_d = -1;
         }
       // usleep(10000);
        task_clean(); 
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

void * main_loop_talker(void *arg)
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
	
	uint8_t *data_ptr;
	void 	*stream_packet;
	int 	frame_size;
	
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
	
	pkt_sz = 1024;
	payload_len = 1024;
	pkt_sz += sizeof(six1883_header) + sizeof(seventeen22_header) + sizeof(eth_header);

	if (pkt_sz > 1500) {
		fprintf(stderr,"payload_len is > MAX_ETH_PACKET_LEN - not supported.\n");
		return -EINVAL;
	}
#ifdef MRPD
	err = mrp_connect();
	if (err) {
		printf("socket creation failed\n");
		return (errno);
	}
#endif
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
        else {
          ptrigb = &igb_dev;
        }
	err = igb_dma_malloc_page(&igb_dev, &a_page);
	if (err) {
		printf("malloc failed (%s) - out of memory?\n", strerror(errno));
		return (errno);
	}

	signal(SIGINT, sigint_handler);

	rc = get_mac_addr(interface1);
	if (rc) {
		printf("failed to open interface(%s)\n",interface1);
	}
        /*initizes the threads required for audio and video*/
#ifdef MRPD
        mrp_monitor(); 
#endif  
     
        printf("transmit main\n"); 
        domain_a_valid = 1;
	class_a_id = MSRP_SR_CLASS_A;
	a_priority = MSRP_SR_CLASS_A_PRIO;
	a_vid = 2;
        mrp_register_domain(&class_a_id, &a_priority, &a_vid);
        mrp_join_vlan();
	igb_set_class_bandwidth(&igb_dev, 2, 0, pkt_sz - 22, 0);
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
	avbtp_initilaze_header(stream_packet, DEST_ADDR, interface1);
	
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

#ifdef MRPD
        mrp_advertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz - 16,
			     PACKET_IPG / 125000, a_priority, 3900);

        fprintf(stderr, "awaiting a listener ...\n");
	mrp_await_listener(STREAM_ID);
	printf("got a listener ...\n");
	halt_tx = 0;

        /*Wait for the control message talker*/

        await_talker();
	send_ready();
	printf("got a talker\n");
#endif
	
#ifdef GPTP 
	gptpinit();
	
#endif		

	err = pthread_create(&tid3, NULL, read_start_feed, NULL);
	if (err != 0)
		printf("\ncan't create thread :[%s]", strerror(err));

	
       
        startofinputdata = 1; 
#ifndef MRPD
        listeners = 1;
#endif
        video_data_start = 1;
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

                while(!video_data_start) {
                  usleep(10);
                }
                read_bytes = read_pipe(igb_fd[0],pipe_ptr,payload_len );
                if((read_bytes < payload_len) || (read_bytes <= 0) ) {
                 
                    printf("data read from igb pipe is failed\n");
                    continue;
                 }
                 else {
                      
                      syncPtr = (int*)pipe_ptr;
                      memcpy(data_ptr,pipe_ptr,read_bytes);
                        
                 }	    

                if(mediaType == 0) {
		   usleep(100);
		}
		/* Reached end of file quit the application*/
		if (read_bytes == 0) {
			goto exit_app;
		}
		total_samples += read_bytes;
		h61883 = (six1883_header *)((uint8_t*)stream_packet + sizeof(eth_header) + sizeof(seventeen22_header));
		h61883->data_block_continuity = total_samples;
                if(halt_tx == 1) {
                 printf("before transmit\n");
                } 
		err = igb_xmit(&igb_dev, 0, tmp_packet);
		if (!err) {
			count++;
                       // printf("frame ID = %d\n",count);		                        
			continue;
		} else {
		
		}

		if (ENOSPC == err) {
			/* put back for now */
			tmp_packet->next = free_packets;
			free_packets = tmp_packet;
		}
cleanup:
		if(halt_tx == 1) {
                 printf("before clean\n");
                } 
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
        if(halt_tx == 1) {
                 printf("before MRP\n");
        } 
        mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz- 16,
				       PACKET_IPG / 125000, a_priority, 3900);
	mrp_disconnect();
	halt_tx = 1;
        task_clean();
       	
	return (0);
}

void task_clean (void) {

         
        if(ptrigb != NULL) {
                
		igb_set_class_bandwidth(&igb_dev, 0, 0, 0, 0);
		igb_dma_free_page(&igb_dev, &a_page);
		igb_detach(&igb_dev);
                ptrigb = NULL;
        }
        printf("after the igb clean\n");
        #if 0
        if(control_socket  != -1 ) {
		
                close(control_socket); 
                control_socket = -1;
         }
        
         if(socket_d != -1 ) {
           close(socket_d);
           socket_d = -1;
         }
         #endif 
         //exit(3); 
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

/* avb code ends here */

static void
gst_avb_sink_class_init (GstAvbSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gstelement_class = GST_ELEMENT_CLASS (klass);
  gstbasesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_avb_sink_set_property;
  gobject_class->get_property = gst_avb_sink_get_property;

  gst_element_class_set_static_metadata (gstelement_class,
      "Filedescriptor Sink",
      "Sink/File",
      "Write data to a file descriptor", "Erik Walthinsen <omega@cse.ogi.edu>");
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sinktemplate));

  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_avb_sink_render);
  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_avb_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_avb_sink_stop);
  g_object_class_install_property (gobject_class, ARG_FD,
      g_param_spec_int ("fd", "fd", "An open file descriptor to write to",
          0, G_MAXINT, 1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, ARG_INTERFACE,
	g_param_spec_string ("interface", "Interface","Ethernet AVB Interface",
			     interface1, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, ARG_MEDIA_TYPE,
	g_param_spec_int ("mediaType", "mediaType","Ethernet AVB Media Type",
			     0, G_MAXINT, mediaType, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, ARG_DATA_SYNC,
	g_param_spec_int ("dataSync", "dataSyncType","Ethernet AVB Data Sync",
			     0, G_MAXINT, dataSync, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));			     
  
}

static void
gst_avb_sink_init (GstAvbSink * avbsink)
{
  int err;
  avbsink->fd = 1;
  avbsink->interface = interface1;
  avbsink->mediaType = mediaType;
  avbsink->dataSync = dataSync;
  avbsink->uri = g_strdup_printf ("fd://%d", avbsink->fd);
  avbsink->bytes_written = 0;
  avbsink->current_pos = 0;

  gst_base_sink_set_sync (GST_BASE_SINK (avbsink), FALSE);
  printf("avb sink init\n");

 err = pipe (igb_fd);
 if (err < 0) {
	     printf("video Pipe is not creating \n");
	     return(0);
  }
  /* Thread Creation */
  err = pthread_create(&tid, NULL, main_loop_talker, NULL);

  if (err != 0) 
  	printf("\ncan't create thread :[%s]", strerror(err));

}
static gboolean
gst_avb_sink_stop (GstBaseSink * basesink)
{
printf("stop\n");
  GstAvbSink *avbsink = GST_AVB_SINK (basesink);
  send_leave();
  printf("before unad \n");
 /* while(halt_tx == 0) {
    usleep(10);
  }*/
  mrp_unadvertise_stream(STREAM_ID, DEST_ADDR, a_vid, pkt_sz- 16,
				       PACKET_IPG / 125000, a_priority, 3900);
  printf("before disconnect \n");
  mrp_disconnect();
  printf("after disconnect \n");
  halt_tx = 1;
  video_data_start = 1;
  close(igb_fd[1]); 
  close(igb_fd[0]);
  if(control_socket  != -1 ) {
		
                shutdown(control_socket,SHUT_RDWR);
                          
                control_socket = -1;
    }
        
         if(socket_d != -1 ) {
           shutdown(socket_d,SHUT_RDWR);
           socket_d = -1;
         }
  #if 0    
  if (avbsink->fdset) {
    gst_poll_free (avbsink->fdset);
    avbsink->fdset = NULL;
  }
  #endif 
  //close(avbsink->fdset);
  task_clean();
  printf("after task clean \n");
  return TRUE;
}

static GstFlowReturn
gst_avb_sink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstAvbSink *avbsink;
  GstMapInfo info;
  guint8 *ptr;
  gsize left;
  gint written;

#ifndef HAVE_WIN32
  gint retval;
#endif

  avbsink = GST_AVB_SINK (sink);

  g_return_val_if_fail (avbsink->fd >= 0, GST_FLOW_ERROR);

  gst_buffer_map (buffer, &info, GST_MAP_READ);

  ptr = info.data;
  left = info.size;

again:
#ifndef HAVE_WIN32
  do {
    GST_DEBUG_OBJECT (avbsink, "going into select, have %" G_GSIZE_FORMAT
        " bytes to write", info.size);
    retval = gst_poll_wait (avbsink->fdset, GST_CLOCK_TIME_NONE);
  } while (retval == -1 && (errno == EINTR || errno == EAGAIN));

  if (retval == -1) {
    if (errno == EBUSY)
      goto stopped;
    else
      goto select_error;
  }
#endif

  GST_DEBUG_OBJECT (avbsink, "writing %" G_GSIZE_FORMAT " bytes to"
      " file descriptor %d", info.size, avbsink->fd);

  written = write (avbsink->fd, ptr, left);

  /* check for errors */
  if (G_UNLIKELY (written < 0)) {
    /* try to write again on non-fatal errors */
    if (errno == EAGAIN || errno == EINTR)
      goto again;

    /* else go to our error handler */
    goto write_error;
  }

  /* all is fine when we get here */
  left -= written;
  ptr += written;
  avbsink->bytes_written += written;
  avbsink->current_pos += written;

  GST_DEBUG_OBJECT (avbsink, "wrote %d bytes, %" G_GSIZE_FORMAT " left", written,
      left);

  /* short write, select and try to write the remainder */
  if (G_UNLIKELY (left > 0))
    goto again;

  gst_buffer_unmap (buffer, &info);

  return GST_FLOW_OK;

#ifndef HAVE_WIN32
select_error:
  {
    GST_ELEMENT_ERROR (avbsink, RESOURCE, READ, (NULL),
        ("select on file descriptor: %s.", g_strerror (errno)));
    GST_DEBUG_OBJECT (avbsink, "Error during select");
    gst_buffer_unmap (buffer, &info);
    return GST_FLOW_ERROR;
  }
stopped:
  {
    GST_DEBUG_OBJECT (avbsink, "Select stopped");
    gst_buffer_unmap (buffer, &info);
    return GST_FLOW_FLUSHING;
  }
#endif

write_error:
  {
    switch (errno) {
      case ENOSPC:
        GST_ELEMENT_ERROR (avbsink, RESOURCE, NO_SPACE_LEFT, (NULL), (NULL));
        break;
      default:{
        GST_ELEMENT_ERROR (avbsink, RESOURCE, WRITE, (NULL),
            ("Error while writing to file descriptor %d: %s",
                avbsink->fd, g_strerror (errno)));
      }
    }
    gst_buffer_unmap (buffer, &info);
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_avb_sink_check_fd (GstAvbSink * avbsink, int fd, GError ** error)
{
  struct stat stat_results;
  off_t result;

  /* see that it is a valid file descriptor */
  if (fstat (fd, &stat_results) < 0)
    goto invalid;

  if (!S_ISREG (stat_results.st_mode))
    goto not_seekable;

  /* see if it is a seekable stream */
  result = lseek (fd, 0, SEEK_CUR);
  if (result == -1) {
    switch (errno) {
      case EINVAL:
      case EBADF:
        goto invalid;

      case ESPIPE:
        goto not_seekable;
    }
  } else
    GST_DEBUG_OBJECT (avbsink, "File descriptor %d is seekable", fd);

  return TRUE;

invalid:
  {
    GST_ELEMENT_ERROR (avbsink, RESOURCE, WRITE, (NULL),
        ("File descriptor %d is not valid: %s", fd, g_strerror (errno)));
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_REFERENCE,
        "File descriptor %d is not valid: %s", fd, g_strerror (errno));
    return FALSE;
  }
not_seekable:
  {
    GST_DEBUG_OBJECT (avbsink, "File descriptor %d is a pipe", fd);
    return TRUE;
  }
}

static gboolean
gst_avb_sink_start (GstBaseSink * basesink)
{
printf("start\n");
  GstAvbSink *avbsink;
  GstPollFD fd = GST_POLL_FD_INIT;

  avbsink = GST_AVB_SINK (basesink);
  if (!gst_avb_sink_check_fd (avbsink, avbsink->fd, NULL))
    return FALSE;

  if ((avbsink->fdset = gst_poll_new (TRUE)) == NULL)
    goto socket_pair;

  fd.fd = avbsink->fd;
  gst_poll_add_fd (avbsink->fdset, &fd);
  gst_poll_fd_ctl_write (avbsink->fdset, &fd, TRUE);

  avbsink->bytes_written = 0;
  avbsink->current_pos = 0;

  avbsink->seekable = gst_avb_sink_do_seek (avbsink, 0);
  GST_INFO_OBJECT (avbsink, "seeking supported: %d", avbsink->seekable);

  return TRUE;

  /* ERRORS */
socket_pair:
  {
    GST_ELEMENT_ERROR (avbsink, RESOURCE, OPEN_READ_WRITE, (NULL),
        GST_ERROR_SYSTEM);
    return FALSE;
  }
}


static gboolean
gst_avb_sink_update_fd (GstAvbSink * avbsink, int new_fd, GError ** error)
{
 new_fd = igb_fd[1]; 
 if (new_fd < 0) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_REFERENCE,
        "File descriptor %d is not valid", new_fd);
    return FALSE;
  }

  if (!gst_avb_sink_check_fd (avbsink, new_fd, error))
    goto invalid;

  /* assign the fd */
  GST_OBJECT_LOCK (avbsink);
  if (avbsink->fdset) {
    GstPollFD fd = GST_POLL_FD_INIT;

    fd.fd = avbsink->fd;
    gst_poll_remove_fd (avbsink->fdset, &fd);

    fd.fd = new_fd;
    gst_poll_add_fd (avbsink->fdset, &fd);
    gst_poll_fd_ctl_write (avbsink->fdset, &fd, TRUE);
  }
  avbsink->fd = new_fd;
  g_free (avbsink->uri);
  avbsink->uri = g_strdup_printf ("fd://%d", avbsink->fd);

  GST_OBJECT_UNLOCK (avbsink);

  return TRUE;

invalid:
  {
    return FALSE;
  }
}

static void
gst_avb_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAvbSink *avbsink;

  avbsink = GST_AVB_SINK (object);
  printf("set proprty\n"); 
  switch (prop_id) {
    case ARG_FD:{
      int fd;

      fd = g_value_get_int (value);
      fd = igb_fd[1];
      gst_avb_sink_update_fd (avbsink, fd, NULL);
      break;
    }
     case ARG_INTERFACE:
	g_free(interface1);
	if (g_value_get_string (value) == NULL)
		interface1 = g_strdup (DEFAULT_INTERFACE);
	else
		interface1 = g_value_dup_string (value);
	break;
     case ARG_MEDIA_TYPE:
	mediaType = g_value_get_int (value);
	break;	
     case ARG_DATA_SYNC:
	dataSync = g_value_get_int (value);
	break;	
     default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_avb_sink_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstAvbSink *avbsink;

  avbsink = GST_AVB_SINK (object);

  switch (prop_id) {
    case ARG_FD:
      g_value_set_int (value, avbsink->fd);
      break;
    case ARG_INTERFACE:
      g_value_set_string (value, interface1);
      break;
    case ARG_MEDIA_TYPE:
      g_value_set_int (value, mediaType);
      break;  
    case ARG_DATA_SYNC:
      g_value_set_int (value, dataSync);
      break;   
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_avb_sink_do_seek (GstAvbSink * avbsink, guint64 new_offset)
{
printf("do_seek\n");
  off_t result;

  result = lseek (avbsink->fd, new_offset, SEEK_SET);

  if (result == -1)
    goto seek_failed;

  avbsink->current_pos = new_offset;

  GST_DEBUG_OBJECT (avbsink, "File descriptor %d to seek to position "
      "%" G_GUINT64_FORMAT, avbsink->fd, avbsink->current_pos);

  return TRUE;

  /* ERRORS */
seek_failed:
  {
    GST_DEBUG_OBJECT (avbsink, "File descriptor %d failed to seek to position "
        "%" G_GUINT64_FORMAT, avbsink->fd, new_offset);
    return FALSE;
  }
}

/*** GSTURIHANDLER INTERFACE *************************************************/

static GstURIType
gst_avb_sink_uri_get_type (GType type)
{
  return GST_URI_SINK;
}

static const gchar *const *
gst_avb_sink_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { "fd", NULL };

  return protocols;
}

static gchar *
gst_avb_sink_uri_get_uri (GstURIHandler * handler)
{
  GstAvbSink *sink = GST_AVB_SINK (handler);

  /* FIXME: make thread-safe */
  return g_strdup (sink->uri);
}

static gboolean
gst_avb_sink_uri_set_uri (GstURIHandler * handler, const gchar * uri,
    GError ** error)
{
  GstAvbSink *sink = GST_AVB_SINK (handler);
  gint fd;

  if (sscanf (uri, "fd://%d", &fd) != 1) {
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
        "File descriptor URI could not be parsed");
    return FALSE;
  }

  return gst_avb_sink_update_fd (sink, fd, error);
}

static void
gst_avb_sink_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_avb_sink_uri_get_type;
  iface->get_protocols = gst_avb_sink_uri_get_protocols;
  iface->get_uri = gst_avb_sink_uri_get_uri;
  iface->set_uri = gst_avb_sink_uri_set_uri;
}

 
static gboolean plugin_init(GstPlugin * plugin)
{
	if (!gst_element_register(plugin, "avbsink", GST_RANK_NONE,
				  gst_avb_sink_get_type()))
		return FALSE;

	return TRUE;

}

GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	avbsink,
	"Ethernet AVB Sink Element",
	plugin_init,
	"1.2.1",
	"LGPL",
	"GStreamer",
	"https://launchpad.net/distros/ubuntu/+source/gstreamer1.0"
)



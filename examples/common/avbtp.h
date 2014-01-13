/*
 * Author: Prabhakar Lad <prabhakar.lad@symphonyteleca.com>
 * Date: Jul - 8 - 2013
 * 
 */

#ifndef __AVBTP_H__
#define __AVBTP_H__

#include <inttypes.h>

#include "igb.h"

#define VALID		1
#define INVALID		0

#define MAC_ADDR_LEN	6

#define IGB_BIND_NAMESZ		24

typedef struct __attribute__ ((packed)) {
	uint64_t subtype:7;
	uint64_t cd_indicator:1;
	uint64_t timestamp_valid:1;
	uint64_t gateway_valid:1;
	uint64_t reserved0:1;
	uint64_t reset:1;
	uint64_t version:3;
	uint64_t sid_valid:1;
	uint64_t seq_number:8;
	uint64_t timestamp_uncertain:1;
	uint64_t reserved1:7;
	uint64_t stream_id;
	uint64_t timestamp:32;
	uint64_t gateway_info:32;
	uint64_t length:16;

} seventeen22_header;


typedef struct __attribute__ ((packed)) {
	uint64_t subtype:7;
	uint64_t cd_indicator:1;
	uint64_t controldata:4;
	uint64_t version:3;
	uint64_t sid_valid:1;
	uint64_t control_frame_length:11;
	uint64_t status:5;
	uint64_t stream_id;
	
} seventeen22_controlHeader;
/* 61883 CIP with SYT Field */
typedef struct {
	uint16_t packet_channel:6;
	uint16_t format_tag:2;
	uint16_t app_control:4;
	uint16_t packet_tcode:4;
	uint16_t source_id:6;
	uint16_t reserved0:2;
	uint16_t data_block_size:8;
	uint16_t reserved1:2;
	uint16_t source_packet_header:1;
	uint16_t quadlet_padding_count:3;
	uint16_t fraction_number:2;
	uint16_t data_block_continuity:8;
	uint16_t format_id:6;
	uint16_t eoh:2;
	uint16_t format_dependent_field:8;
	uint16_t syt;
} six1883_header;

typedef struct {
	uint8_t label;
	uint8_t value[3];
} six1883_sample;

#define ETH_ALEN   6 /* Size of Ethernet address */
typedef struct __attribute__ ((packed)) {
	/* Destination MAC address. */
	uint8_t h_dest [ETH_ALEN];
	/* Destination MAC address. */
	uint8_t h_source [ETH_ALEN];
	/* Protocol ID. */
	uint8_t h_protocol[2];
} eth_header;

typedef struct { 
	int64_t ml_phoffset;
	int64_t ls_phoffset;
	int32_t ml_freqoffset;
	int32_t ls_freqoffset;
	int64_t local_time;
} gPtpTimeData;

typedef enum { false = 0, true = 1 } bool;

#define SHM_SIZE 4*8 + sizeof(pthread_mutex_t) /* 3 - 64 bit and 2 - 32 bits */
#define SHM_NAME  "/ptp"

#define MAX_SAMPLE_VALUE ((1U << ((sizeof(int32_t)*8)-1))-1)

#define IEEE_61883_IIDC_SUBTYPE 0x0

#define MRPD_PORT_DEFAULT 7500

int pci_connect(device_t * igb_dev);

//int gptpscaling(gPtpTimeData * td, char *memory_offset_buffer);

//void gptpdeinit(int shm_fd, char *memory_offset_buffer);

//int gptpinit(int *shm_fd, char *memory_offset_buffer);

void * avbtp_create_stream_packet(uint32_t payload_len);

static int32_t
avbtp_initilaze_header(void *ip_pkt, uint8_t *dst_addr, int8_t *ifname);

#endif		/*  __AVBTP_H__ */

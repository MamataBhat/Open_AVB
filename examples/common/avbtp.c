#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <netinet/in.h>

#include <pci/pci.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>

#include "avbtp.h"

int pci_connect(device_t * igb_dev)
{
	char devpath[IGB_BIND_NAMESZ];
	struct pci_access *pacc;
	struct pci_dev *dev;
	int err;

	memset(igb_dev, 0, sizeof(device_t));

	pacc = pci_alloc();

	pci_init(pacc);

	pci_scan_bus(pacc);

	for (dev = pacc->devices; dev; dev = dev->next) {
		pci_fill_info(dev,
				PCI_FILL_IDENT | PCI_FILL_BASES | PCI_FILL_CLASS);
		igb_dev->pci_vendor_id = dev->vendor_id;
		igb_dev->pci_device_id = dev->device_id;
		igb_dev->domain = dev->domain;
		igb_dev->bus = dev->bus;
		igb_dev->dev = dev->dev;
		igb_dev->func = dev->func;
		snprintf(devpath, IGB_BIND_NAMESZ, "%04x:%02x:%02x.%d",
				dev->domain, dev->bus, dev->dev, dev->func);
		err = igb_probe(igb_dev);
		if (err)
			continue;

		printf("attaching to %s\n", devpath);
		err = igb_attach(devpath, igb_dev);
		if (err) {
			printf("attach failed! (%s)\n", strerror(errno));
			continue;
		}
		goto out;
	}

	pci_cleanup(pacc);
	return ENXIO;

out:	pci_cleanup(pacc);

	return 0;
}
#if 0
int gptpinit(int *shm_fd, char *memory_offset_buffer)
{
	*shm_fd = shm_open(SHM_NAME, O_RDWR, 0);
	if (*shm_fd == -1) {
		perror("shm_open()");
		return false;
	}
	memory_offset_buffer =
	    (char *)mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			 *shm_fd, 0);
	if (memory_offset_buffer == (char *)-1) {
		perror("mmap()");
		memory_offset_buffer = NULL;
		shm_unlink(SHM_NAME);
		return false;
	}
	return true;
}

void gptpdeinit(int shm_fd, char *memory_offset_buffer)
{
	if (memory_offset_buffer != NULL) {
		munmap(memory_offset_buffer, SHM_SIZE);
	}
	if (shm_fd != -1) {
		close(shm_fd);
	}
}

int gptpscaling(gPtpTimeData * td, char *memory_offset_buffer)
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
#endif
void * avbtp_create_stream_packet(uint32_t payload_len)
{
	void *e_ptr = NULL;
	uint32_t mem_size;
	seventeen22_header *h1722 = NULL;
	six1883_header *h61883;

	mem_size = sizeof(six1883_header);
	mem_size += sizeof(eth_header) + sizeof(seventeen22_header) + payload_len;
	e_ptr = calloc(mem_size, sizeof(uint8_t));
	if (e_ptr == NULL)
		return NULL;

	//assign 1722 default values.
	h1722 = (seventeen22_header *) ((uint8_t*)e_ptr + sizeof(eth_header));
	h1722->cd_indicator = 0x0;
	h1722->subtype = 0x0;
	h1722->sid_valid = INVALID;
	h1722->version = 0x0;
	h1722->reset = 0;
	h1722->reserved0 = 0;
	h1722->gateway_valid = INVALID;
	h1722->timestamp_valid = INVALID;
	h1722->reserved1 = 0;
	h1722->timestamp_uncertain = INVALID;
	h1722->timestamp = 0;
	h1722->gateway_info = 0;
	h1722->length = htons(payload_len + sizeof(six1883_header));

	h61883 = (six1883_header *)((uint8_t*)h1722 + sizeof(seventeen22_header));
	h61883->format_tag = 1;
	h61883->packet_channel = 0x1f;
	h61883->packet_tcode = 0xa;
	h61883->app_control = 0;
	h61883->source_packet_header = 0;
	h61883->reserved0 = 0;

	return e_ptr;
}

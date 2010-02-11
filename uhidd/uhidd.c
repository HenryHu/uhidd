/*-
 * Copyright (c) 2009, 2010 Kai Wang
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <libusb20.h>
#include <libusb20_desc.h>
#include <libutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include "uhidd.h"

int verbose = 0;

static int detach = 1;
static struct pidfh *pfh = NULL;
static STAILQ_HEAD(, hid_parent) hplist;

static void	usage(void);
static int	find_and_attach(struct libusb20_backend *backend,
		    const char *dev);
static int	attach_dev(const char *dev, struct libusb20_device *pdev);
static void	attach_iface(const char *dev, struct libusb20_device *pdev,
		    struct libusb20_interface *iface, int i);
static void	*start_hid_parent(void *arg);
static int	hid_set_report(void *context, int report_id, char *buf,
		    int len);
static void	sighandler(int sig __unused);
static void	terminate(int eval);

int
main(int argc, char **argv)
{
	struct hid_parent *hp;
	struct libusb20_backend *backend;
	char *pid_file;
	pid_t otherpid;
	int eval, opt;

	eval = 0;

	openlog("uhidd", LOG_PID|LOG_PERROR|LOG_NDELAY, LOG_USER);

	config_init();

	while ((opt = getopt(argc, argv, "cdhkKmMosuv")) != -1) {
		switch(opt) {
		case 'c':
			config_file = optarg;
			break;
		case 'd':
			detach = 0;
			break;
		case 'h':
			clconfig.attach_hid = 1;
			break;
		case 'k':
			clconfig.attach_kbd = 1;
			break;
		case 'm':
			clconfig.attach_mouse = 1;
			break;
		case 'o':
			clconfig.attach_cc = 1;
			break;
		case 's':
			clconfig.strip_report_id = 1;
			break;
		case 'u':
			clconfig.detach_kernel_driver = 1;
			break;
		case 'v':
			detach = 0;
			verbose++;
			break;
		default:
			usage();
		}
	}

	argv += optind;
	argc -= optind;

	if (*argv == NULL)
		usage();

	/* Check that another uhidd isn't already attached to the device. */
	if (asprintf(&pid_file, "/var/run/uhidd.%s.pid", basename(*argv)) < 0) {
		syslog(LOG_ERR, "asprintf failed: %m");
		exit(1);
	}
	pfh = pidfile_open(pid_file, 0600, &otherpid);
	if (pfh == NULL) {
		if (errno == EEXIST) {
			syslog(LOG_ERR, "uhidd already running on %s, pid: %d.",
			    *argv, otherpid);
			exit(1);
		}
		syslog(LOG_WARNING, "cannot open or create pidfile");
	}
	free(pid_file);

	if (config_read_file() < 0) {
		if (verbose)
			syslog(LOG_WARNING, "proceed without configuration"
			    " file");
	}

	if (detach) {
		if (daemon(0, 0) < 0) {
			syslog(LOG_ERR, "daemon failed: %m");
			exit(1);
		}
	}

	signal(SIGTERM, sighandler);
	signal(SIGINT, sighandler);

	/* Write pid file. */
	pidfile_write(pfh);

	backend = libusb20_be_alloc_default();
	if (backend == NULL) {
		syslog(LOG_ERR, "can not alloc backend");
		eval = 1;
		goto uhidd_end;
	}

	STAILQ_INIT(&hplist);

	if (find_and_attach(backend, *argv) < 0) {
		eval = 1;
		goto uhidd_end;
	}

	libusb20_be_free(backend);

	if (STAILQ_EMPTY(&hplist))
		goto uhidd_end;

	STAILQ_FOREACH(hp, &hplist, next) {
		if (hp->hi != NULL)
			pthread_create(&hp->thread, NULL, start_hid_parent,
			    (void *)hp);
	}
	STAILQ_FOREACH(hp, &hplist, next) {
		if (hp->hi != NULL)
			pthread_join(hp->thread, NULL);
	}

uhidd_end:

	terminate(eval);
}

static void
terminate(int eval)
{

	pidfile_remove(pfh);
	syslog(LOG_NOTICE, "terminated\n");

	exit(eval);
}

/* ARGSUSED */
static void
sighandler(int sig __unused)
{

	terminate(1);
}

static int
find_and_attach(struct libusb20_backend *backend, const char *dev)
{
	struct libusb20_device *pdev;
	unsigned int bus, addr;

	if (sscanf(dev, "/dev/ugen%u.%u", &bus, &addr) < 2) {
		syslog(LOG_ERR, "%s not found", dev);
		return (-1);
	}

	pdev = NULL;
	while ((pdev = libusb20_be_device_foreach(backend, pdev)) != NULL) {
		if (bus == libusb20_dev_get_bus_number(pdev) &&
		    addr == libusb20_dev_get_address(pdev)) {
			if (attach_dev(dev, pdev) < 0)
				return (-1);
			break;
		}
	}

	if (pdev == NULL) {
		syslog(LOG_ERR, "%s not found", dev);
		return (-1);
	}

	return (0);
}

static int
attach_dev(const char *dev, struct libusb20_device *pdev)
{
	struct LIBUSB20_DEVICE_DESC_DECODED *ddesc;
	struct libusb20_config *config;
	struct libusb20_interface *iface;
	int cndx, e, i;

	e = libusb20_dev_open(pdev, 32);
	if (e != 0) {
		syslog(LOG_ERR, "libusb20_dev_open %s failed", dev);
		return (-1);
	}

	/*
	 * Use current configuration.
	 */
	cndx = libusb20_dev_get_config_index(pdev);
	config = libusb20_dev_alloc_config(pdev, cndx);
	if (config == NULL) {
		syslog(LOG_ERR, "Can not alloc config for %s", dev);
		return (-1);
	}

	ddesc = libusb20_dev_get_device_desc(pdev);

	/*
	 * Iterate each interface.
	 */
	for (i = 0; i < config->num_interface; i++) {
		iface = &config->interface[i];
		if (iface->desc.bInterfaceClass == LIBUSB20_CLASS_HID) {
			if (verbose)
				PRINT0(dev, i, "HID interface\n");
			attach_iface(dev, pdev, iface, i);
		}
	}

	free(config);

	return (0);
}

static void
attach_iface(const char *dev, struct libusb20_device *pdev,
    struct libusb20_interface *iface, int ndx)
{
	struct LIBUSB20_DEVICE_DESC_DECODED *ddesc;
	struct LIBUSB20_CONTROL_SETUP_DECODED req;
	struct hid_parent *hp;
	struct libusb20_endpoint *ep;
	unsigned char rdesc[16384];
	int desc, ds, e, j, pos, size;
	uint16_t actlen;

#if 0
	/* XXX ioctl currently unimplemented */
	if (libusb20_dev_kernel_driver_active(pdev, ndx)) {
		printf("%s: iface(%d) kernel driver is active\n", dev, ndx);
		/* TODO probably detach the kernel driver here. */
	} else
		printf("%s: iface(%d) kernel driver is not active\n", dev, ndx);
#endif

	/*
	 * Get report descriptor.
	 */

	pos = 0;
	size = iface->extra.len;
	while (size > 2) {
		if (libusb20_me_get_1(&iface->extra, pos + 1) == LIBUSB20_DT_HID)
			break;
		size -= libusb20_me_get_1(&iface->extra, pos);
		pos += libusb20_me_get_1(&iface->extra, pos);
	}
	if (size <= 2)
		return;
	desc = pos + 6;
	for (j = 0; j < libusb20_me_get_1(&iface->extra, pos + 5);
	     j++, desc += j * 3) {
		if (libusb20_me_get_1(&iface->extra, desc) ==
		    LIBUSB20_DT_REPORT)
			break;
	}
	if (j >= libusb20_me_get_1(&iface->extra, pos + 5))
		return;
	ds = libusb20_me_get_2(&iface->extra, desc + 1);
	if (verbose)
		PRINT0(dev, ndx, "Report descriptor size = %d\n", ds);
	LIBUSB20_INIT(LIBUSB20_CONTROL_SETUP, &req);
	req.bmRequestType = LIBUSB20_ENDPOINT_IN |
	    LIBUSB20_REQUEST_TYPE_STANDARD | LIBUSB20_RECIPIENT_INTERFACE;
	req.bRequest = LIBUSB20_REQUEST_GET_DESCRIPTOR;
	req.wValue = LIBUSB20_DT_REPORT << 8;
	req.wIndex = ndx;
	req.wLength = ds;
	e = libusb20_dev_request_sync(pdev, &req, rdesc, &actlen, 0, 0);
	if (e) {
		syslog(LOG_ERR, "%s[iface:%d]=> libusb20_dev_request_sync"
		    " failed", dev, ndx);
		return;
	}

	/*
	 * Allocate a hid parent device.
	 */

	hp = calloc(1, sizeof(*hp));
	if (hp == NULL) {
		syslog(LOG_ERR, "calloc failed: %m");
		exit(1);
	}
	hp->dev = dev;
	hp->pdev = pdev;
	hp->iface = iface;
	hp->ndx = ndx;
	memcpy(hp->rdesc, rdesc, actlen);
	hp->rsz = actlen;
	ddesc = libusb20_dev_get_device_desc(pdev);
	hp->vendor_id = ddesc->idVendor;
	hp->product_id = ddesc->idProduct;

	/*
	 * Find the input interrupt endpoint.
	 */

	for (j = 0; j < iface->num_endpoints; j++) {
		ep = &iface->endpoints[j];
		if ((ep->desc.bmAttributes & LIBUSB20_TRANSFER_TYPE_MASK) ==
		    LIBUSB20_TRANSFER_TYPE_INTERRUPT &&
		    ((ep->desc.bEndpointAddress & LIBUSB20_ENDPOINT_DIR_MASK) ==
		    LIBUSB20_ENDPOINT_IN)) {
			hp->ep = ep->desc.bEndpointAddress;
			hp->pkt_sz = ep->desc.wMaxPacketSize;
			if (verbose) {
				PRINT1("Find IN interrupt ep: %#x", hp->ep);
				printf(" packet_size=%#x\n", hp->pkt_sz);
			}
			break;
		}
	}
	if (hp->ep == 0) {
		PRINT1("does not have IN interrupt ep\n");
		free(hp);
		return;
	}

	hp->hi = hid_interface_alloc(hp->rdesc, hp->rsz, hp);
	hid_interface_set_write_callback(hp->hi, hid_set_report);

	STAILQ_INSERT_TAIL(&hplist, hp, next);
}

static void *
start_hid_parent(void *arg)
{
	struct hid_parent *hp;
	struct libusb20_backend *backend;
	struct libusb20_transfer *xfer;
	struct libusb20_device *pdev;
	unsigned int bus, addr;
	char buf[4096];
	uint32_t actlen;
	uint8_t x;
	int e, i;

	hp = arg;
	assert(hp != NULL);

	if (verbose)
		PRINT1("HID parent started\n");

	if (sscanf(hp->dev, "/dev/ugen%u.%u", &bus, &addr) < 2) {
		syslog(LOG_ERR, "%s not found", hp->dev);
		return (NULL);
	}

	backend = libusb20_be_alloc_default();
	pdev = NULL;
	while ((pdev = libusb20_be_device_foreach(backend, pdev)) != NULL) {
		if (bus == libusb20_dev_get_bus_number(pdev) &&
		    addr == libusb20_dev_get_address(pdev)) {
			e = libusb20_dev_open(pdev, 32);
			if (e != 0) {
				syslog(LOG_ERR,
				    "%s: libusb20_dev_open failed\n",
				    hp->dev);
				return (NULL);
			}
			break;
		}
	}
	if (pdev == NULL) {
		syslog(LOG_ERR, "%s not found", hp->dev);
		return (NULL);
	}
	hp->pdev = pdev;

	x = (hp->ep & LIBUSB20_ENDPOINT_ADDRESS_MASK) * 2;
	x |= 1;			/* IN transfer. */
	xfer = libusb20_tr_get_pointer(hp->pdev, x);
	if (xfer == NULL) {
		syslog(LOG_ERR, "%s[iface:%d] libusb20_tr_get_pointer failed\n",
		    hp->dev, hp->ndx);
		goto parent_end;
	}

	e = libusb20_tr_open(xfer, 4096, 1, hp->ep);
	if (e == LIBUSB20_ERROR_BUSY) {
		PRINT1("xfer already opened\n");
	} else if (e) {
		syslog(LOG_ERR, "%s[iface:%d] libusb20_tr_open failed\n",
		    hp->dev, hp->ndx);
		goto parent_end;
	}

	for (;;) {

		if (libusb20_tr_pending(xfer)) {
			PRINT1("tr pending\n");
			continue;
		}

		libusb20_tr_setup_intr(xfer, buf, hp->pkt_sz, 0);

		libusb20_tr_start(xfer);

		for (;;) {
			if (libusb20_dev_process(hp->pdev) != 0) {
				PRINT1(" device detached?\n");
				goto parent_end;
			}
			if (libusb20_tr_pending(xfer) == 0)
				break;
			libusb20_dev_wait_process(hp->pdev, -1);
		}

		switch (libusb20_tr_get_status(xfer)) {
		case 0:
			actlen = libusb20_tr_get_actual_length(xfer);
			if (verbose > 2) {
				PRINT1("received data(%u): ", actlen);
				for (i = 0; (uint32_t) i < actlen; i++)
					printf("%02d ", buf[i]);
				putchar('\n');
			}
			hid_interface_input_data(hp->hi, buf, actlen);
			break;
		case LIBUSB20_TRANSFER_TIMED_OUT:
			if (verbose)
				PRINT1("TIMED OUT\n");
			break;
		default:
			if (verbose)
				PRINT1("transfer error\n");
			break;
		}
	}

parent_end:

	if (verbose)
		PRINT1("HID parent exit\n");

	return (NULL);
}

#if 0
int
hid_interrupt_out(void *context, int report_id, char *buf, int len)
{
	struct hid_parent *hp;
	struct libusb20_transfer *xfer;
	uint32_t actlen;
	uint8_t x;
	int e, i, size;

	hp = context;
	assert(hp != NULL && hp->pdev != NULL);

	x = (hp->ep & LIBUSB20_ENDPOINT_ADDRESS_MASK) * 2;
	xfer = libusb20_tr_get_pointer(hp->pdev, x);
	if (xfer == NULL) {
		syslog(LOG_ERR, "%s[iface:%d] libusb20_tr_get_pointer failed\n",
		    hp->dev, hp->ndx);
		return (-1);
	}

	e = libusb20_tr_open(xfer, 4096, 1, XXX); /* FIXME */
	if (e && e != LIBUSB20_ERROR_BUSY) {
		syslog(LOG_ERR, "%s[iface:%d] libusb20_tr_open failed\n",
		    hp->dev, hp->ndx);
		return (-1);
	}
	
	if (libusb20_tr_pending(xfer)) {
		PRINT1("tr pending\n");
		return (-1);
	}

	size = len;
	while (size > 0) {

		libusb20_tr_setup_intr(xfer, buf, len, 0);

		libusb20_tr_start(xfer);

		for (;;) {
			if (libusb20_dev_process(hp->pdev) != 0) {
				PRINT1(" device detached?\n");
				return (-1);
			}
			if (libusb20_tr_pending(xfer) == 0)
				break;
			libusb20_dev_wait_process(hp->pdev, -1);
		}

		switch (libusb20_tr_get_status(xfer)) {
		case 0:
			actlen = libusb20_tr_get_actual_length(xfer);
			if (verbose > 2) {
				PRINT1("transfered data(%u): ", actlen);
				for (i = 0; (uint32_t) i < actlen; i++)
					printf("%02d ", buf[i]);
				putchar('\n');
			}
			break;

		case LIBUSB20_TRANSFER_TIMED_OUT:
			if (verbose)
				PRINT1("TIMED OUT\n");
			return (-1);
		default:
			if (verbose)
				PRINT1("transfer error\n");
			return (-1);
		}

		buf += actlen;
		size -= actlen;
	}

	return (0);
}
#endif

static int
hid_set_report(void *context, int report_id, char *buf, int len)
{
	struct LIBUSB20_CONTROL_SETUP_DECODED req;
	struct hid_parent *hp;
	uint16_t actlen;
	int e;

	hp = context;
	assert(hp != NULL && hp->pdev != NULL);

	LIBUSB20_INIT(LIBUSB20_CONTROL_SETUP, &req);
	req.bmRequestType = LIBUSB20_ENDPOINT_OUT |
	    LIBUSB20_REQUEST_TYPE_CLASS | LIBUSB20_RECIPIENT_INTERFACE;
	req.bRequest = 0x09;	/* SET_REPORT */
	req.wValue = (0x02 << 8) | (report_id & 0xff); /* FIXME report type */
	req.wIndex = hp->ndx;
	req.wLength = len;
	e = libusb20_dev_request_sync(hp->pdev, &req, buf, &actlen, len, 0);
	if (e) {
		syslog(LOG_ERR, "%s[iface:%d]=> libusb20_dev_request_sync"
		    " failed", hp->dev, hp->ndx);
		return (-1);
	}

	return (0);
}

static void
usage(void)
{

	fprintf(stderr, "usage: uhidd [-cdhkmouv] /dev/ugen%%u.%%u\n");
	exit(1);
}

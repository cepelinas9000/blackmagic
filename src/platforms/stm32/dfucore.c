/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2013 Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform.h"

#include <string.h>
#if defined(STM32F1)
#	include <libopencm3/stm32/f1/flash.h>
#elif defined(STM32F2)
#	include <libopencm3/stm32/f2/flash.h>
#elif defined(STM32F4)
#	include <libopencm3/stm32/f4/flash.h>
#endif
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/dfu.h>

#include "usbdfu.h"

usbd_device *usbdev;
/* We need a special large control buffer for this device: */
u8 usbd_control_buffer[1024];

static u32 max_address;

static enum dfu_state usbdfu_state = STATE_DFU_IDLE;

static char *get_dev_unique_id(char *serial_no);

static struct {
	u8 buf[sizeof(usbd_control_buffer)];
	u16 len;
	u32 addr;
	u16 blocknum;
} prog;

const struct usb_device_descriptor dev = {
        .bLength = USB_DT_DEVICE_SIZE,
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = 0,
        .bDeviceSubClass = 0,
        .bDeviceProtocol = 0,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x1D50,
        .idProduct = 0x6017,
        .bcdDevice = 0x0100,
        .iManufacturer = 1,
        .iProduct = 2,
        .iSerialNumber = 3,
        .bNumConfigurations = 1,
};

const struct usb_dfu_descriptor dfu_function = {
	.bLength = sizeof(struct usb_dfu_descriptor),
	.bDescriptorType = DFU_FUNCTIONAL,
	.bmAttributes = USB_DFU_CAN_DOWNLOAD | USB_DFU_WILL_DETACH,
	.wDetachTimeout = 255,
	.wTransferSize = 1024,
	.bcdDFUVersion = 0x011A,
};

const struct usb_interface_descriptor iface = {
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 0,
	.bInterfaceClass = 0xFE, /* Device Firmware Upgrade */
	.bInterfaceSubClass = 1,
	.bInterfaceProtocol = 2,

	/* The ST Microelectronics DfuSe application needs this string.
	 * The format isn't documented... */
	.iInterface = 4,

	.extra = &dfu_function,
	.extralen = sizeof(dfu_function),
};

const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.altsetting = &iface,
}};

const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 1,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0xC0,
	.bMaxPower = 0x32,

	.interface = ifaces,
};

static char serial_no[9];

static const char *usb_strings[] = {
	"Black Sphere Technologies",
	BOARD_IDENT_DFU,
	serial_no,
	/* This string is used by ST Microelectronics' DfuSe utility */
	DFU_IFACE_STRING,
};

static u32 get_le32(const void *vp)
{
	const u8 *p = vp;
	return ((u32)p[3] << 24) + ((u32)p[2] << 16) + (p[1] << 8) + p[0];
}

static u8 usbdfu_getstatus(u32 *bwPollTimeout)
{
	switch(usbdfu_state) {
	case STATE_DFU_DNLOAD_SYNC:
		usbdfu_state = STATE_DFU_DNBUSY;
		*bwPollTimeout = dfu_poll_timeout(prog.buf[0],
					get_le32(prog.buf + 1),
					prog.blocknum);
		return DFU_STATUS_OK;

	case STATE_DFU_MANIFEST_SYNC:
		/* Device will reset when read is complete */
		usbdfu_state = STATE_DFU_MANIFEST;
		return DFU_STATUS_OK;

	default:
		return DFU_STATUS_OK;
	}
}

static void
usbdfu_getstatus_complete(usbd_device *dev, struct usb_setup_data *req)
{
	(void)req;

	switch(usbdfu_state) {
	case STATE_DFU_DNBUSY:

		flash_unlock();
		if(prog.blocknum == 0) {
			u32 addr = get_le32(prog.buf + 1);
			if (addr < APP_ADDRESS ||
			    (addr >= max_address)) {
				flash_lock();
				usbd_ep_stall_set(dev, 0, 1);
				return;
			}
			switch(prog.buf[0]) {
			case CMD_ERASE:
				dfu_check_and_do_sector_erase(addr);
			case CMD_SETADDR:
				prog.addr = addr;
			}
		} else {
			u32 baseaddr = prog.addr +
				((prog.blocknum - 2) *
					dfu_function.wTransferSize);
			dfu_flash_program_buffer(baseaddr, prog.buf, prog.len);
		}
		flash_lock();

		/* We jump straight to dfuDNLOAD-IDLE,
		 * skipping dfuDNLOAD-SYNC
		 */
		usbdfu_state = STATE_DFU_DNLOAD_IDLE;
		return;

	case STATE_DFU_MANIFEST:
		dfu_detach();
		return; /* Will never return */
	default:
		return;
	}
}

static int usbdfu_control_request(usbd_device *dev,
		struct usb_setup_data *req, u8 **buf, u16 *len,
		void (**complete)(usbd_device *dev, struct usb_setup_data *req))
{
	(void)dev;

	if((req->bmRequestType & 0x7F) != 0x21)
		return 0; /* Only accept class request */

	switch(req->bRequest) {
	case DFU_DNLOAD:
		if((len == NULL) || (*len == 0)) {
			usbdfu_state = STATE_DFU_MANIFEST_SYNC;
			return 1;
		} else {
			/* Copy download data for use on GET_STATUS */
			prog.blocknum = req->wValue;
			prog.len = *len;
			memcpy(prog.buf, *buf, *len);
			usbdfu_state = STATE_DFU_DNLOAD_SYNC;
			return 1;
		}
	case DFU_CLRSTATUS:
		/* Clear error and return to dfuIDLE */
		if(usbdfu_state == STATE_DFU_ERROR)
			usbdfu_state = STATE_DFU_IDLE;
		return 1;
	case DFU_ABORT:
		/* Abort returns to dfuIDLE state */
		usbdfu_state = STATE_DFU_IDLE;
		return 1;
	case DFU_UPLOAD:
		/* Upload not supported for now */
		return 0;
	case DFU_GETSTATUS: {
		u32 bwPollTimeout = 0; /* 24-bit integer in DFU class spec */

		(*buf)[0] = usbdfu_getstatus(&bwPollTimeout);
		(*buf)[1] = bwPollTimeout & 0xFF;
		(*buf)[2] = (bwPollTimeout >> 8) & 0xFF;
		(*buf)[3] = (bwPollTimeout >> 16) & 0xFF;
		(*buf)[4] = usbdfu_state;
		(*buf)[5] = 0;	/* iString not used here */
		*len = 6;

		*complete = usbdfu_getstatus_complete;

		return 1;
		}
	case DFU_GETSTATE:
		/* Return state with no state transision */
		*buf[0] = usbdfu_state;
		*len = 1;
		return 1;
	}

	return 0;
}

void dfu_init(const usbd_driver *driver)
{
	get_dev_unique_id(serial_no);

	usbdev = usbd_init(driver, &dev, &config, usb_strings, 4);
	usbd_set_control_buffer_size(usbdev, sizeof(usbd_control_buffer));
	usbd_register_control_callback(usbdev,
				USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
				USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT,
				usbdfu_control_request);
}

void dfu_main(void)
{
	while (1)
		usbd_poll(usbdev);
}

static char *get_dev_unique_id(char *s)
{
#if defined(STM32F4) || defined(STM32F2)
#define UNIQUE_SERIAL_R 0x1FFF7A10
#define FLASH_SIZE_R    0x1fff7A22
#elif defined(STM32F3)
#define UNIQUE_SERIAL_R 0x1FFFF7AC
#define FLASH_SIZE_R    0x1fff77cc
#elif defined(STM32L1)
#define UNIQUE_SERIAL_R 0x1ff80050
#define FLASH_SIZE_R    0x1FF8004C
#else
#define UNIQUE_SERIAL_R 0x1FFFF7E8;
#define FLASH_SIZE_R    0x1ffff7e0
#endif
        volatile uint32_t *unique_id_p = (volatile uint32_t *)UNIQUE_SERIAL_R;
	uint32_t unique_id = *unique_id_p +
			*(unique_id_p + 1) +
			*(unique_id_p + 2);
        int i;

        /* Calculated the upper flash limit from the exported data
           in theparameter block*/
        max_address = (*(u32 *) FLASH_SIZE_R) <<10;
        /* Fetch serial number from chip's unique ID */
        for(i = 0; i < 8; i++) {
                s[7-i] = ((unique_id >> (4*i)) & 0xF) + '0';
        }
        for(i = 0; i < 8; i++)
                if(s[i] > '9')
                        s[i] += 'A' - '9' - 1;
	s[8] = 0;

	return s;
}

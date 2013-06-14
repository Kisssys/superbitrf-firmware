/*
 * This file is part of the superbitrf project.
 *
 * Copyright (C) 2013 Freek van Tienen <freek.v.tienen@gmail.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <libopencm3/cm3/common.h>
#include <libopencm3/stm32/f1/rcc.h>
#include <libopencm3/stm32/f1/gpio.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>

#include "cdcacm.h"

// The recieve callback
cdcacm_receive_callback _cdcacm_receive_callback = NULL;
// The usbd device
usbd_device *cdacm_usbd_dev = NULL;
// The usbd control buffer
u8 cdacm_usbd_control_buffer[128];
// When the device is connected TODO: Fix nicely
u8 is_connected = 0;

// The usb device descriptor
static const struct usb_device_descriptor dev = {
	.bLength = USB_DT_DEVICE_SIZE,
	.bDescriptorType = USB_DT_DEVICE,
	.bcdUSB = 0x0200,
	.bDeviceClass = USB_CLASS_CDC,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x0484,
	.idProduct = 0x5741,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 3,
	.bNumConfigurations = 1,
};

// The usb comm endpoint descriptor
static const struct usb_endpoint_descriptor comm_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x83,
	.bmAttributes = USB_ENDPOINT_ATTR_INTERRUPT,
	.wMaxPacketSize = 16,
	.bInterval = 255,
}};

// The usb data endpoint desciptor
static const struct usb_endpoint_descriptor data_endp[] = {{
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x01,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
}, {
	.bLength = USB_DT_ENDPOINT_SIZE,
	.bDescriptorType = USB_DT_ENDPOINT,
	.bEndpointAddress = 0x82,
	.bmAttributes = USB_ENDPOINT_ATTR_BULK,
	.wMaxPacketSize = 64,
	.bInterval = 1,
}};

// The functional descriptors
static const struct {
	struct usb_cdc_header_descriptor header;
	struct usb_cdc_call_management_descriptor call_mgmt;
	struct usb_cdc_acm_descriptor acm;
	struct usb_cdc_union_descriptor cdc_union;
} __attribute__((packed)) cdcacm_functional_descriptors = {
	.header = {
		.bFunctionLength = sizeof(struct usb_cdc_header_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_HEADER,
		.bcdCDC = 0x0110,
	},
	.call_mgmt = {
		.bFunctionLength =
			sizeof(struct usb_cdc_call_management_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_CALL_MANAGEMENT,
		.bmCapabilities = 0,
		.bDataInterface = 1,
	},
	.acm = {
		.bFunctionLength = sizeof(struct usb_cdc_acm_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_ACM,
		.bmCapabilities = 0,
	},
	.cdc_union = {
		.bFunctionLength = sizeof(struct usb_cdc_union_descriptor),
		.bDescriptorType = CS_INTERFACE,
		.bDescriptorSubtype = USB_CDC_TYPE_UNION,
		.bControlInterface = 0,
		.bSubordinateInterface0 = 1,
	 },
};

// The comm interface descriptor
static const struct usb_interface_descriptor comm_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = USB_CLASS_CDC,
	.bInterfaceSubClass = USB_CDC_SUBCLASS_ACM,
	.bInterfaceProtocol = USB_CDC_PROTOCOL_AT,
	.iInterface = 0,

	.endpoint = comm_endp,

	.extra = &cdcacm_functional_descriptors,
	.extralen = sizeof(cdcacm_functional_descriptors),
}};

// The data interface descriptor
static const struct usb_interface_descriptor data_iface[] = {{
	.bLength = USB_DT_INTERFACE_SIZE,
	.bDescriptorType = USB_DT_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = USB_CLASS_DATA,
	.bInterfaceSubClass = 0,
	.bInterfaceProtocol = 0,
	.iInterface = 0,

	.endpoint = data_endp,
}};

// The usb interfaces
static const struct usb_interface ifaces[] = {{
	.num_altsetting = 1,
	.altsetting = comm_iface,
}, {
	.num_altsetting = 1,
	.altsetting = data_iface,
}};

// The usb config descriptor
static const struct usb_config_descriptor config = {
	.bLength = USB_DT_CONFIGURATION_SIZE,
	.bDescriptorType = USB_DT_CONFIGURATION,
	.wTotalLength = 0,
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0x80,
	.bMaxPower = 0x32,

	.interface = ifaces,
};

// The usb strings
static const char *usb_strings[] = {
	"Black Sphere Technologies",
	"SuperbitRF",
	"DEMO",
};

/**
 * CDCACM control request received
 */
static int cdcacm_control_request(usbd_device *usbd_dev,
		struct usb_setup_data *req, u8 **buf, u16 *len,
		void (**complete)(usbd_device *usbd_dev, struct usb_setup_data *req)) {
	(void) complete;
	(void) buf;
	(void) usbd_dev;

	switch (req->bRequest) {
	case USB_CDC_REQ_SET_CONTROL_LINE_STATE: {
		/*
		 * This Linux cdc_acm driver requires this to be implemented
		 * even though it's optional in the CDC spec, and we don't
		 * advertise it in the ACM functional descriptor.
		 */
		return 1;
	}
	case USB_CDC_REQ_SET_LINE_CODING:
		if (*len < sizeof(struct usb_cdc_line_coding))
			return 0;
		return 1;
	}
	return 0;
}

/**
 * CDCACM recieve callback
 */
static void cdcacm_data_rx_cb(usbd_device *usbd_dev, u8 ep) {
	(void) ep;
	(void) usbd_dev;

	char buf[64];
	int len = usbd_ep_read_packet(usbd_dev, 0x01, buf, 64);
	is_connected = 1;

	if (len) {
		//sprintf(buf, "test: 0x%02X 0x%02X 0x%02X 0x%02X", dsm.cyrf_mfg_id[0], dsm.cyrf_mfg_id[1], dsm.cyrf_mfg_id[2], dsm.cyrf_mfg_id[3]);
		//while(usbd_ep_write_packet(usbd_dev, 0x82, buf, 25) == 0);
		//while(usbd_ep_write_packet(usbd_dev, 0x82, "a\r\n", 3) == 0);
		buf[len] = 0;

		if (_cdcacm_receive_callback != NULL) {
			_cdcacm_receive_callback(buf, len);
		}
	}
}

/**
 * CDCACM set config
 */
static void cdcacm_set_config_callback(usbd_device *usbd_dev, u16 wValue) {
	(void) wValue;
	(void) usbd_dev;

	usbd_ep_setup(usbd_dev, 0x01, USB_ENDPOINT_ATTR_BULK, 64,
			cdcacm_data_rx_cb);
	usbd_ep_setup(usbd_dev, 0x82, USB_ENDPOINT_ATTR_BULK, 64, NULL);
	usbd_ep_setup(usbd_dev, 0x83, USB_ENDPOINT_ATTR_INTERRUPT, 16, NULL);

	usbd_register_control_callback(usbd_dev,
			USB_REQ_TYPE_CLASS | USB_REQ_TYPE_INTERFACE,
			USB_REQ_TYPE_TYPE | USB_REQ_TYPE_RECIPIENT, cdcacm_control_request);
}

/**
 * Initialize the CDCACM
 */
void cdcacm_init(void) {
	/**
	 * Setup GPIOA Detach pin no pullup on D+ making it float, until we are
	 * ready to talk to the host.
	 */
	rcc_peripheral_enable_clock(&RCC_APB2ENR, USB_DETACH_CLK);
	gpio_clear(USB_DETACH_PORT, USB_DETACH_PIN);
	gpio_set_mode(USB_DETACH_PORT, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT,
			USB_DETACH_PIN);

	/* Setup the USB driver. */
	cdacm_usbd_dev = usbd_init(&stm32f103_usb_driver, &dev, &config,
			usb_strings, 3, cdacm_usbd_control_buffer,
			sizeof(cdacm_usbd_control_buffer));
	usbd_register_set_config_callback(cdacm_usbd_dev,
			cdcacm_set_config_callback);

	/**
	 * Setup GPIOA Detach pin to pull up the D+ high. To let the host know that we are here and ready to talk.
	 */
	gpio_set(USB_DETACH_PORT, USB_DETACH_PIN);
	gpio_set_mode(USB_DETACH_PORT, GPIO_MODE_OUTPUT_2_MHZ, GPIO_CNF_OUTPUT_PUSHPULL,
			USB_DETACH_PIN);
}

/**
 * Run the CDCACM
 */
void cdcacm_run(void) {
	usbd_poll(cdacm_usbd_dev);
}

/**
 * Register CDCACM receive callback
 * @param[in] callback The function for the receive callback
 */
void cdcacm_register_receive_callback(cdcacm_receive_callback callback) {
	_cdcacm_receive_callback = callback;
}

/**
 * Send data trough the CDCACM
 * @param[in] data The data that needs to be send
 * @param[in] size The size of the data in bytes
 */
bool cdcacm_send(const char *data, const int size) {
	int i = 0;

	// When a host is connected TODO: Fix nicely
	if(!is_connected)
		return false;

	while ((size - (i * 64)) > 64) {
		while (usbd_ep_write_packet(cdacm_usbd_dev, 0x82, (data + (i * 64)), 64) == 0);
		i++;
	}

	while (usbd_ep_write_packet(cdacm_usbd_dev, 0x82, (data + (i * 64)), size - (i * 64)) == 0);

	return true;
}

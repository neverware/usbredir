/* usbredirhost.c usb network redirection usb host code.

   Copyright 2010-2012 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "usbredirhost.h"

#define MAX_ENDPOINTS        32
#define MAX_INTERFACES       32 /* Max 32 endpoints and thus interfaces */
#define CTRL_TIMEOUT       5000 /* USB specifies a 5 second max timeout */
#define BULK_TIMEOUT       5000
#define ISO_TIMEOUT        1000
#define INTERRUPT_TIMEOUT     0 /* No timeout for interrupt transfers */

#define MAX_ISO_TRANSFER_COUNT       16
#define MAX_ISO_PACKETS_PER_TRANSFER 32
/* Special iso_packet_idx value indicating a submitted transfer */
#define ISO_SUBMITTED_IDX            -1

/* Macros to go from an endpoint address to an index for our ep array */
#define EP2I(ep_address) (((ep_address & 0x80) >> 3) | (ep_address & 0x0f))
#define I2EP(i) (((i & 0x10) << 3) | (i & 0x0f))

/* Locking convenience macros */
#define LOCK(host) \
    do { \
        if ((host)->lock) \
            (host)->parser->lock_func((host)->lock); \
    } while (0)

#define UNLOCK(host) \
    do { \
        if ((host)->lock) \
            (host)->parser->unlock_func((host)->lock); \
    } while (0)

#define FLUSH(host) \
    do { \
        if ((host)->flush_writes_func) \
            (host)->flush_writes_func((host)->func_priv); \
    } while (0)

struct usbredirtransfer {
    struct usbredirhost *host;        /* Back pointer to the the redirhost */
    struct libusb_transfer *transfer; /* Back pointer to the libusb transfer */
    uint32_t id;
    uint8_t cancelled;
    int iso_packet_idx;
    union {
        struct usb_redir_control_packet_header control_packet;
        struct usb_redir_bulk_packet_header bulk_packet;
        struct usb_redir_iso_packet_header iso_packet;
        struct usb_redir_interrupt_packet_header interrupt_packet;
    };
    struct usbredirtransfer *next;
    struct usbredirtransfer *prev;
};

struct usbredirhost_ep {
    uint8_t type;
    uint8_t interval;
    uint8_t interface;
    uint8_t iso_started;
    uint8_t iso_pkts_per_transfer;
    uint8_t iso_transfer_count;
    int iso_out_idx;
    int iso_drop_packets;
    int max_packetsize;
    struct usbredirtransfer *iso_transfer[MAX_ISO_TRANSFER_COUNT];
    struct usbredirtransfer *interrupt_in_transfer;
};

struct usbredirhost {
    struct usbredirparser *parser;

    void *lock;
    void *disconnect_lock;

    usbredirparser_log log_func;
    usbredirparser_read read_func;
    usbredirparser_write write_func;
    usbredirhost_flush_writes flush_writes_func;
    void *func_priv;
    int verbose;
    libusb_context *ctx;
    libusb_device *dev;
    libusb_device_handle *handle;
    struct libusb_device_descriptor desc;
    struct libusb_config_descriptor *config;
    int active_config;
    int claimed;
    int disconnected;
    int read_status;
    int cancels_pending;
    int wait_disconnect;
    int connect_pending;
    struct usbredirhost_ep endpoint[MAX_ENDPOINTS];
    uint8_t alt_setting[MAX_INTERFACES];
    struct usbredirtransfer transfers_head;
    struct usbredirfilter_rule *filter_rules;
    int filter_rules_count;
};

static void va_log(struct usbredirhost *host, int level,
    const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (level > host->verbose) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    
    host->log_func(host->func_priv, level, buf);
}

#define ERROR(...)   va_log(host, usbredirparser_error, \
                            "usbredirhost error: " __VA_ARGS__)
#define WARNING(...) va_log(host, usbredirparser_warning, \
                            "usbredirhost warning: " __VA_ARGS__)
#define INFO(...)    va_log(host, usbredirparser_info, \
                            "usbredirhost: " __VA_ARGS__)

#define DEBUG(...)   va_log(host, usbredirparser_debug, \
                            "usbredirhost: " __VA_ARGS__)

static void usbredirhost_hello(void *priv, struct usb_redir_hello_header *h);
static void usbredirhost_reset(void *priv);
static void usbredirhost_set_configuration(void *priv, uint32_t id,
    struct usb_redir_set_configuration_header *set_configuration);
static void usbredirhost_get_configuration(void *priv, uint32_t id);
static void usbredirhost_set_alt_setting(void *priv, uint32_t id,
    struct usb_redir_set_alt_setting_header *set_alt_setting);
static void usbredirhost_get_alt_setting(void *priv, uint32_t id,
    struct usb_redir_get_alt_setting_header *get_alt_setting);
static void usbredirhost_start_iso_stream(void *priv, uint32_t id,
    struct usb_redir_start_iso_stream_header *start_iso_stream);
static void usbredirhost_stop_iso_stream(void *priv, uint32_t id,
    struct usb_redir_stop_iso_stream_header *stop_iso_stream);
static void usbredirhost_start_interrupt_receiving(void *priv, uint32_t id,
    struct usb_redir_start_interrupt_receiving_header *start_interrupt_receiving);
static void usbredirhost_stop_interrupt_receiving(void *priv, uint32_t id,
    struct usb_redir_stop_interrupt_receiving_header *stop_interrupt_receiving);
static void usbredirhost_alloc_bulk_streams(void *priv, uint32_t id,
    struct usb_redir_alloc_bulk_streams_header *alloc_bulk_streams);
static void usbredirhost_free_bulk_streams(void *priv, uint32_t id,
    struct usb_redir_free_bulk_streams_header *free_bulk_streams);
static void usbredirhost_cancel_data_packet(void *priv, uint32_t id);
static void usbredirhost_filter_reject(void *priv);
static void usbredirhost_filter_filter(void *priv,
    struct usbredirfilter_rule *rules, int rules_count);
static void usbredirhost_device_disconnect_ack(void *priv);
static void usbredirhost_control_packet(void *priv, uint32_t id,
    struct usb_redir_control_packet_header *control_packet,
    uint8_t *data, int data_len);
static void usbredirhost_bulk_packet(void *priv, uint32_t id,
    struct usb_redir_bulk_packet_header *bulk_packet,
    uint8_t *data, int data_len);
static void usbredirhost_iso_packet(void *priv, uint32_t id,
    struct usb_redir_iso_packet_header *iso_packet,
    uint8_t *data, int data_len);
static void usbredirhost_interrupt_packet(void *priv, uint32_t id,
    struct usb_redir_interrupt_packet_header *interrupt_packet,
    uint8_t *data, int data_len);

static int usbredirhost_alloc_iso_stream(struct usbredirhost *host,
    uint8_t ep, uint8_t pkts_per_transfer, uint8_t transfer_count);
static void usbredirhost_cancel_iso_stream(struct usbredirhost *host,
    uint8_t ep);
static void usbredirhost_cancel_iso_stream_unlocked(struct usbredirhost *host,
    uint8_t ep);
static void usbredirhost_cancel_interrupt_in_transfer_unlocked(
    struct usbredirhost *host, uint8_t ep);
static int usbredirhost_cancel_pending_urbs(struct usbredirhost *host);
static void usbredirhost_free_transfer(struct usbredirtransfer *transfer);
static void usbredirhost_clear_device(struct usbredirhost *host);

static void usbredirhost_log(void *priv, int level, const char *msg)
{
    struct usbredirhost *host = priv;

    host->log_func(host->func_priv, level, msg);
}

static int usbredirhost_read(void *priv, uint8_t *data, int count)
{
    struct usbredirhost *host = priv;

    if (host->read_status) {
        int ret = host->read_status;
        host->read_status = 0;
        return ret;
    }

    return host->read_func(host->func_priv, data, count);
}

static int usbredirhost_write(void *priv, uint8_t *data, int count)
{
    struct usbredirhost *host = priv;

    return host->write_func(host->func_priv, data, count);
}

/* Can be called both from parser read callbacks as well as from libusb
   packet completion callbacks */
static void usbredirhost_handle_disconnect(struct usbredirhost *host)
{
    /* Disconnect uses its own lock to avoid needing nesting capable locks */
    if (host->disconnect_lock) {
        host->parser->lock_func(host->disconnect_lock);
    }
    if (!host->disconnected) {
        INFO("device disconnected");
        usbredirparser_send_device_disconnect(host->parser);
        if (usbredirparser_peer_has_cap(host->parser,
                                        usb_redir_cap_device_disconnect_ack))
            host->wait_disconnect = 1;
        host->disconnected = 1;
    }
    if (host->disconnect_lock) {
        host->parser->unlock_func(host->disconnect_lock);
    }
}

/* One function to convert either a transfer status code, or a libusb error
   code to a usb_redir status. We handle both in one conversion function so
   that we can pass error codes as status codes to the completion handler
   in case of submission error (the codes don't overlap), using the completion
   handler to report back the status and cleanup as it would on completion of
   a successfully submitted transfer. */
static int libusb_status_or_error_to_redir_status(struct usbredirhost *host,
                                                  int status)
{
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return usb_redir_success;
        case LIBUSB_TRANSFER_ERROR:
            return usb_redir_ioerror;
        case LIBUSB_TRANSFER_TIMED_OUT:
            return usb_redir_timeout;
        case LIBUSB_TRANSFER_CANCELLED:
            return usb_redir_cancelled;
        case LIBUSB_TRANSFER_STALL:
            return usb_redir_stall;
        case LIBUSB_TRANSFER_NO_DEVICE:
            usbredirhost_handle_disconnect(host);
            return usb_redir_ioerror;
        case LIBUSB_TRANSFER_OVERFLOW:
            return usb_redir_ioerror;

        case LIBUSB_ERROR_INVALID_PARAM: 
            return usb_redir_inval;
        case LIBUSB_ERROR_NO_DEVICE:
            usbredirhost_handle_disconnect(host);
            return usb_redir_ioerror;
        case LIBUSB_ERROR_TIMEOUT:
            return usb_redir_timeout;
        default:
            return usb_redir_ioerror;
    }
}

static int usbredirhost_get_max_packetsize(uint16_t wMaxPacketSize)
{
    int size, packets_per_microframe;

    size = wMaxPacketSize & 0x7ff;
    switch ((wMaxPacketSize >> 11) & 3) {
    case 1:  packets_per_microframe = 2; break;
    case 2:  packets_per_microframe = 3; break;
    default: packets_per_microframe = 1; break;
    }
    return size * packets_per_microframe;
}

/* Called from open/close and parser read callbacks */
static void usbredirhost_send_interface_n_ep_info(struct usbredirhost *host)
{
    int i;
    const struct libusb_interface_descriptor *intf_desc;
    struct usb_redir_ep_info_header ep_info;
    struct usb_redir_interface_info_header interface_info;

    interface_info.interface_count = host->config->bNumInterfaces;
    for (i = 0; i < host->config->bNumInterfaces; i++) {
        intf_desc =
            &host->config->interface[i].altsetting[host->alt_setting[i]];

        interface_info.interface[i] = intf_desc->bInterfaceNumber;
        interface_info.interface_class[i] = intf_desc->bInterfaceClass;
        interface_info.interface_subclass[i] = intf_desc->bInterfaceSubClass;
        interface_info.interface_protocol[i] = intf_desc->bInterfaceProtocol;
    }
    usbredirparser_send_interface_info(host->parser, &interface_info);

    for (i = 0; i < MAX_ENDPOINTS; i++) {
        ep_info.type[i] = host->endpoint[i].type;
        ep_info.interval[i] = host->endpoint[i].interval;
        ep_info.interface[i] = host->endpoint[i].interface;
    }
    usbredirparser_send_ep_info(host->parser, &ep_info);
}

/* Called from open/close and parser read callbacks */
static void usbredirhost_send_device_connect(struct usbredirhost *host)
{
    struct usb_redir_device_connect_header device_connect;
    enum libusb_speed speed;

    if (!host->disconnected) {
        ERROR("internal error sending device_connect but already connected");
        return;
    }

    if (!usbredirparser_have_peer_caps(host->parser) ||
            host->wait_disconnect) {
        host->connect_pending = 1;
        return;
    }

    speed = libusb_get_device_speed(host->dev);
    switch (speed) {
    case LIBUSB_SPEED_LOW:
        device_connect.speed = usb_redir_speed_low; break;
    case LIBUSB_SPEED_FULL:
        device_connect.speed = usb_redir_speed_full; break;
    case LIBUSB_SPEED_HIGH:
        device_connect.speed = usb_redir_speed_high; break;
    case LIBUSB_SPEED_SUPER:
        device_connect.speed = usb_redir_speed_super; break;
    default:
        device_connect.speed = usb_redir_speed_unknown;
    }
    device_connect.device_class = host->desc.bDeviceClass;
    device_connect.device_subclass = host->desc.bDeviceSubClass;
    device_connect.device_protocol = host->desc.bDeviceProtocol;
    device_connect.vendor_id = host->desc.idVendor;
    device_connect.product_id = host->desc.idProduct;
    device_connect.device_version_bcd = host->desc.bcdDevice;

    usbredirhost_send_interface_n_ep_info(host);
    usbredirparser_send_device_connect(host->parser, &device_connect);
    host->connect_pending = 0;
    host->disconnected = 0; /* The guest know may use the device */

    FLUSH(host);
}

/* Called from open/close and parser read callbacks */
static void usbredirhost_parse_interface(struct usbredirhost *host, int i)
{
    int j;
    const struct libusb_interface_descriptor *intf_desc;
    uint8_t ep_address;

    intf_desc =
        &host->config->interface[i].altsetting[host->alt_setting[i]];

    for (j = 0; j < intf_desc->bNumEndpoints; j++) {
        ep_address = intf_desc->endpoint[j].bEndpointAddress;
        /* FIXlibusb libusb_get_max_iso_packet_size always returns 0
           independent of alt setting?? */
        host->endpoint[EP2I(ep_address)].max_packetsize =
            usbredirhost_get_max_packetsize(
                intf_desc->endpoint[j].wMaxPacketSize);
            /* libusb_get_max_iso_packet_size(host->dev, ep_address); */
        host->endpoint[EP2I(ep_address)].type =
            intf_desc->endpoint[j].bmAttributes &
                LIBUSB_TRANSFER_TYPE_MASK;
        host->endpoint[EP2I(ep_address)].interval =
            intf_desc->endpoint[j].bInterval;
        host->endpoint[EP2I(ep_address)].interface =
            intf_desc->bInterfaceNumber;
    }
}

static void usbredirhost_parse_config(struct usbredirhost *host)
{
    int i;

    for (i = 0; i < MAX_ENDPOINTS; i++) {
        if ((i & 0x0f) == 0) {
            host->endpoint[i].type = usb_redir_type_control;
        } else {
            host->endpoint[i].type = usb_redir_type_invalid;
        }
        host->endpoint[i].interval = 0;
        host->endpoint[i].interface = 0;
    }

    for (i = 0; i < host->config->bNumInterfaces; i++) {
        usbredirhost_parse_interface(host, i);
    }
}

/* Called from open/close and parser read callbacks */
static int usbredirhost_claim(struct usbredirhost *host)
{
    int i, n, r, ret = usb_redir_success;

    if (host->config) {
        libusb_free_config_descriptor(host->config);
        host->config = NULL;
    }

    r = libusb_get_config_descriptor_by_value(host->dev, host->active_config,
                                              &host->config);
    if (r < 0) {
        ERROR("could not get descriptors for configuration %d: %d",
              host->active_config, r);
        return libusb_status_or_error_to_redir_status(host, r);
    }
    if (host->config->bNumInterfaces > MAX_INTERFACES) {
        ERROR("usb decriptor has too much intefaces (%d > %d)",
              (int)host->config->bNumInterfaces, MAX_INTERFACES);
        return usb_redir_ioerror;
    }

    /* All interfaces begin alt setting 0 when (re)claimed */
    memset(host->alt_setting, 0, MAX_INTERFACES);

    for (i = 0; i < host->config->bNumInterfaces; i++) {
        n = host->config->interface[i].altsetting[0].bInterfaceNumber;

        r = libusb_detach_kernel_driver(host->handle, n);
        if (r < 0 && r != LIBUSB_ERROR_NOT_FOUND) {
            ERROR("could not detach driver from interface %d (configuration %d): %d",
                  n, host->active_config, r);
            ret = libusb_status_or_error_to_redir_status(host, r);
            goto error;
        }

        r = libusb_claim_interface(host->handle, n);
        if (r < 0) {
            ERROR("could not claim interface %d (configuration %d): %d",
                  n, host->active_config, r);
            ret = libusb_status_or_error_to_redir_status(host, r);
            goto error;
        }
    }

    usbredirhost_parse_config(host);
    host->claimed = 1;
    return ret;

error:
    for (; i >= 0; i--) {
        n = host->config->interface[i].altsetting[0].bInterfaceNumber;

        /* This is a nop on non claimed interfaces */
        libusb_release_interface(host->handle, n);
        libusb_attach_kernel_driver(host->handle, n);
    }
    return ret;
}

/* Called from open/close and parser read callbacks */
static int usbredirhost_release(struct usbredirhost *host, int attach_drivers)
{
    int i, n, r, ret = usb_redir_success;

    if (!host->claimed) {
        return usb_redir_success;
    }

    for (i = 0; i < host->config->bNumInterfaces; i++) {
        n = host->config->interface[i].altsetting[0].bInterfaceNumber;

        r = libusb_release_interface(host->handle, n);
        if (r < 0 && r != LIBUSB_ERROR_NOT_FOUND
                  && r != LIBUSB_ERROR_NO_DEVICE) {
            ERROR("could not release interface %d (configuration %d): %d",
                  n, host->active_config, r);
            ret = usb_redir_ioerror;
        }
    }

    if (!attach_drivers) {
        goto exit;
    }

    for (i = 0; i < host->config->bNumInterfaces; i++) {
        n = host->config->interface[i].altsetting[0].bInterfaceNumber;
        r = libusb_attach_kernel_driver(host->handle, n);
        if (r < 0 && r != LIBUSB_ERROR_NOT_FOUND /* No driver */
                  && r != LIBUSB_ERROR_NO_DEVICE /* Device unplugged */
                  && r != LIBUSB_ERROR_BUSY /* driver rebound already */) {
            ERROR("could not re-attach driver to interface %d (configuration %d): %d",
                  n, host->active_config, r);
            ret = usb_redir_ioerror;
        }
    }

    host->claimed = 0;
exit:
    return ret;
}

struct usbredirhost *usbredirhost_open(
    libusb_context *usb_ctx,
    libusb_device_handle *usb_dev_handle,
    usbredirparser_log log_func,
    usbredirparser_read  read_guest_data_func,
    usbredirparser_write write_guest_data_func,
    void *func_priv, const char *version, int verbose, int flags)
{
    return usbredirhost_open_full(usb_ctx, usb_dev_handle, log_func,
                                  read_guest_data_func, write_guest_data_func,
                                  NULL, NULL, NULL, NULL, NULL,
                                  func_priv, version, verbose, flags);
}

struct usbredirhost *usbredirhost_open_full(
    libusb_context *usb_ctx,
    libusb_device_handle *usb_dev_handle,
    usbredirparser_log log_func,
    usbredirparser_read  read_guest_data_func,
    usbredirparser_write write_guest_data_func,
    usbredirhost_flush_writes flush_writes_func,
    usbredirparser_alloc_lock alloc_lock_func,
    usbredirparser_lock lock_func,
    usbredirparser_unlock unlock_func,
    usbredirparser_free_lock free_lock_func,
    void *func_priv, const char *version, int verbose, int flags)
{
    struct usbredirhost *host;
    int parser_flags = usbredirparser_fl_usb_host;
    uint32_t caps[USB_REDIR_CAPS_SIZE] = { 0, };

    host = calloc(1, sizeof(*host));
    if (!host) {
        log_func(func_priv, usbredirparser_error,
            "usbredirhost error: Out of memory allocating usbredirhost");
        libusb_close(usb_dev_handle);
        return NULL;
    }

    host->ctx = usb_ctx;
    host->log_func = log_func;
    host->read_func = read_guest_data_func;
    host->write_func = write_guest_data_func;
    host->flush_writes_func = flush_writes_func;
    host->func_priv = func_priv;
    host->verbose = verbose;
    host->disconnected = 1; /* No device is connected initially */
    host->parser = usbredirparser_create();
    if (!host->parser) {
        log_func(func_priv, usbredirparser_error,
            "usbredirhost error: Out of memory allocating usbredirparser");
        usbredirhost_close(host);
        return NULL;
    }
    host->parser->priv = host;
    host->parser->log_func = usbredirhost_log;
    host->parser->read_func = usbredirhost_read;
    host->parser->write_func = usbredirhost_write;
    host->parser->hello_func = usbredirhost_hello;
    host->parser->reset_func = usbredirhost_reset;
    host->parser->set_configuration_func = usbredirhost_set_configuration;
    host->parser->get_configuration_func = usbredirhost_get_configuration;
    host->parser->set_alt_setting_func = usbredirhost_set_alt_setting;
    host->parser->get_alt_setting_func = usbredirhost_get_alt_setting;
    host->parser->start_iso_stream_func = usbredirhost_start_iso_stream;
    host->parser->stop_iso_stream_func = usbredirhost_stop_iso_stream;
    host->parser->start_interrupt_receiving_func =
        usbredirhost_start_interrupt_receiving;
    host->parser->stop_interrupt_receiving_func =
        usbredirhost_stop_interrupt_receiving;
    host->parser->alloc_bulk_streams_func = usbredirhost_alloc_bulk_streams;
    host->parser->free_bulk_streams_func = usbredirhost_free_bulk_streams;
    host->parser->cancel_data_packet_func = usbredirhost_cancel_data_packet;
    host->parser->filter_reject_func = usbredirhost_filter_reject;
    host->parser->filter_filter_func = usbredirhost_filter_filter;
    host->parser->device_disconnect_ack_func =
        usbredirhost_device_disconnect_ack;
    host->parser->control_packet_func = usbredirhost_control_packet;
    host->parser->bulk_packet_func = usbredirhost_bulk_packet;
    host->parser->iso_packet_func = usbredirhost_iso_packet;
    host->parser->interrupt_packet_func = usbredirhost_interrupt_packet;
    host->parser->alloc_lock_func = alloc_lock_func;
    host->parser->lock_func = lock_func;
    host->parser->unlock_func = unlock_func;
    host->parser->free_lock_func = free_lock_func;

    if (host->parser->alloc_lock_func) {
        host->lock = host->parser->alloc_lock_func();
        host->disconnect_lock = host->parser->alloc_lock_func();
    }

    if (flags & usbredirhost_fl_write_cb_owns_buffer) {
        parser_flags |= usbredirparser_fl_write_cb_owns_buffer;
    }

    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_filter);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_device_disconnect_ack);

    usbredirparser_init(host->parser, version, caps, USB_REDIR_CAPS_SIZE,
                        parser_flags);

    libusb_set_debug(host->ctx, host->verbose);

    if (usbredirhost_set_device(host, usb_dev_handle) != usb_redir_success) {
        usbredirhost_close(host);
        return NULL;
    }

    FLUSH(host);

    return host;
}

void usbredirhost_close(struct usbredirhost *host)
{
    usbredirhost_clear_device(host);

    if (host->lock) {
        host->parser->free_lock_func(host->lock);
    }
    if (host->disconnect_lock) {
        host->parser->free_lock_func(host->disconnect_lock);
    }
    if (host->parser) {
        usbredirparser_destroy(host->parser);
    }
    free(host->filter_rules);
    free(host);
}

int usbredirhost_set_device(struct usbredirhost *host,
                             libusb_device_handle *usb_dev_handle)
{
    int r, status;

    usbredirhost_clear_device(host);

    if (!usb_dev_handle)
        return usb_redir_success;

    host->dev = libusb_get_device(usb_dev_handle);
    host->handle = usb_dev_handle;

    r = libusb_get_configuration(host->handle, &host->active_config);
    if (r < 0) {
        ERROR("could not get active configuration: %d", r);
        usbredirhost_clear_device(host);
        return libusb_status_or_error_to_redir_status(host, r);
    }

    r = libusb_get_device_descriptor(host->dev, &host->desc);
    if (r < 0) {
        ERROR("could not get device descriptor: %d", r);
        usbredirhost_clear_device(host);
        return libusb_status_or_error_to_redir_status(host, r);
    }

    status = usbredirhost_claim(host);
    if (status != usb_redir_success) {
        usbredirhost_clear_device(host);
        return status;
    }

    usbredirhost_send_device_connect(host);

    return usb_redir_success;
}

static void usbredirhost_clear_device(struct usbredirhost *host)
{
    int wait;
    struct timeval tv;

    if (!host->dev)
        return;

    wait = usbredirhost_cancel_pending_urbs(host);
    while (wait) {
        memset(&tv, 0, sizeof(tv));
        tv.tv_usec = 2500;
        libusb_handle_events_timeout(host->ctx, &tv);
        LOCK(host);
        wait = host->cancels_pending || host->transfers_head.next;
        UNLOCK(host);
    }

    usbredirhost_release(host, 1);

    if (host->config) {
        libusb_free_config_descriptor(host->config);
        host->config = NULL;
    }
    if (host->handle) {
        libusb_close(host->handle);
        host->handle = NULL;
    }

    host->connect_pending = 0;
    host->dev = NULL;

    usbredirhost_handle_disconnect(host);
    FLUSH(host);
}

int usbredirhost_read_guest_data(struct usbredirhost *host)
{
    return usbredirparser_do_read(host->parser);
}

int usbredirhost_has_data_to_write(struct usbredirhost *host)
{
    return usbredirparser_has_data_to_write(host->parser);
}

int usbredirhost_write_guest_data(struct usbredirhost *host)
{
    return usbredirparser_do_write(host->parser);
}

void usbredirhost_free_write_buffer(struct usbredirhost *host, uint8_t *data)
{
    usbredirparser_free_write_buffer(host->parser, data);
}

/**************************************************************************/

static struct usbredirtransfer *usbredirhost_alloc_transfer(
    struct usbredirhost *host, int iso_packets)
{
    struct usbredirtransfer *redir_transfer;
    struct libusb_transfer *libusb_transfer;

    redir_transfer  = calloc(1, sizeof(*redir_transfer));
    libusb_transfer = libusb_alloc_transfer(iso_packets);
    if (!redir_transfer || !libusb_transfer) {
        ERROR("out of memory allocating usb transfer, dropping packet");
        free(redir_transfer);
        libusb_free_transfer(libusb_transfer);
        return NULL;
    }
    redir_transfer->host       = host;
    redir_transfer->transfer   = libusb_transfer;
    libusb_transfer->user_data = redir_transfer;

    return redir_transfer;
}

static void usbredirhost_free_transfer(struct usbredirtransfer *transfer)
{
    if (!transfer)
        return;

    /* In certain cases this should really be a usbredirparser_free_packet_data
       but since we use the same malloc impl. as usbredirparser this is ok. */
    free(transfer->transfer->buffer);
    libusb_free_transfer(transfer->transfer);
    free(transfer);
}

static void usbredirhost_add_transfer(struct usbredirhost *host,
    struct usbredirtransfer *new_transfer)
{
    struct usbredirtransfer *transfer = &host->transfers_head;

    LOCK(host);
    while (transfer->next) {
        transfer = transfer->next;
    }

    new_transfer->prev = transfer;
    transfer->next = new_transfer;
    UNLOCK(host);
}

static void usbredirhost_remove_and_free_transfer(
    struct usbredirtransfer *transfer)
{
    struct usbredirhost *host = transfer->host;

    LOCK(host);
    if (transfer->next)
        transfer->next->prev = transfer->prev;
    if (transfer->prev)
        transfer->prev->next = transfer->next;
    UNLOCK(host);
    usbredirhost_free_transfer(transfer);
}

/* Called from close and parser read callbacks */
static int usbredirhost_cancel_pending_urbs(struct usbredirhost *host)
{
    struct usbredirtransfer *t;
    int i, wait;

    LOCK(host);
    for (i = 0; i < MAX_ENDPOINTS; i++) {
        switch (host->endpoint[i].type) {
        case usb_redir_type_iso:
            usbredirhost_cancel_iso_stream_unlocked(host, I2EP(i));
            break;
        case usb_redir_type_interrupt:
            if (i & 0x10)
                usbredirhost_cancel_interrupt_in_transfer_unlocked(host,
                                                                   I2EP(i));
            break;
        }
    }

    wait = host->cancels_pending;
    for (t = host->transfers_head.next; t; t = t->next) {
        libusb_cancel_transfer(t->transfer);
        wait = 1;
    }
    UNLOCK(host);

    return wait;
}

/* Called from close and parser read callbacks */
static void usbredirhost_cancel_pending_urbs_on_interface(
    struct usbredirhost *host, int i)
{
    struct usbredirtransfer *t;
    const struct libusb_interface_descriptor *intf_desc;

    LOCK(host);

    intf_desc = &host->config->interface[i].altsetting[host->alt_setting[i]];
    for (i = 0; i < intf_desc->bNumEndpoints; i++) {
        uint8_t ep = intf_desc->endpoint[i].bEndpointAddress;

        switch (host->endpoint[EP2I(ep)].type) {
        case usb_redir_type_iso:
            usbredirhost_cancel_iso_stream_unlocked(host, ep);
            break;
        case usb_redir_type_interrupt:
            if (ep & LIBUSB_ENDPOINT_IN)
                usbredirhost_cancel_interrupt_in_transfer_unlocked(host, ep);
            break;
        }

        for (t = host->transfers_head.next; t; t = t->next) {
            if (t->transfer->endpoint == ep)
                libusb_cancel_transfer(t->transfer);
        }
    }

    UNLOCK(host);
}

/* Only called from read callbacks */
static int usbredirhost_bInterfaceNumber_to_index(
    struct usbredirhost *host, uint8_t bInterfaceNumber)
{
    int i, n;

    for (i = 0; i < host->config->bNumInterfaces; i++) {
        n = host->config->interface[i].altsetting[0].bInterfaceNumber;
        if (n == bInterfaceNumber) {
            return i;
        }
    }

    ERROR("invalid bNumInterface: %d\n", (int)bInterfaceNumber);
    return -1;
}

static void usbredirhost_log_data(struct usbredirhost *host, const char *desc,
    const uint8_t *data, int len)
{
    if (usbredirparser_debug_data <= host->verbose) {
        int i, j, n;

        for (i = 0; i < len; i += j) {
            char buf[128];

            n = sprintf(buf, "%s", desc);
            for (j = 0; j < 8 && i + j < len; j++){
                 n += sprintf(buf + n, " %02X", data[i + j]);
            }
            va_log(host, usbredirparser_debug_data, buf);
        }
    }
}

/**************************************************************************/

static void usbredirhost_send_iso_status(struct usbredirhost *host,
    uint32_t id, uint8_t endpoint, uint8_t status)
{
    struct usb_redir_iso_stream_status_header iso_stream_status;

    iso_stream_status.endpoint = endpoint;
    iso_stream_status.status = status;
    usbredirparser_send_iso_stream_status(host->parser, id, &iso_stream_status);
}

/* Called from both parser read and packet complete callbacks */
static int usbredirhost_submit_iso_transfer_unlocked(struct usbredirhost *host,
    struct usbredirtransfer *transfer)
{
    int r;

    r = libusb_submit_transfer(transfer->transfer);
    if (r < 0) {
        uint8_t ep = transfer->transfer->endpoint;
        ERROR("submitting iso transfer on ep %02X: %d, stopping stream",
              (unsigned int)ep, r);
        usbredirhost_cancel_iso_stream_unlocked(host, ep);
        return libusb_status_or_error_to_redir_status(host, r);
    }

    transfer->iso_packet_idx = ISO_SUBMITTED_IDX;
    return usb_redir_success;
}

/* Return value:
    0 All ok
    1 Packet borked, continue with next packet / urb
    2 Stream borked, full stop, no resubmit, etc.
   Note in the case of a return value of 2 this function takes care of
   sending an iso status message to the usb-guest. */
static int usbredirhost_handle_iso_status(struct usbredirhost *host,
    uint32_t id, uint8_t ep, int r)
{
    int i, status;

    switch (r) {
    case LIBUSB_TRANSFER_COMPLETED:
    case -EXDEV: /* FIXlibusb: Passing regular error codes, bad libusb, bad! */
        return 0;
    case LIBUSB_TRANSFER_CANCELLED:
        /* Stream was intentionally stopped */
        return 2;
    case LIBUSB_TRANSFER_STALL: {
        uint8_t pkts_per_transfer, transfer_count;

        /* Uhoh, Cancel the stream, clear stall, on success re-alloc
           and in case of an input stream resubmit the transfers */
        WARNING("iso stream on endpoint %02X stalled, clearing stall",
                (unsigned int)ep);
        pkts_per_transfer = host->endpoint[EP2I(ep)].iso_pkts_per_transfer;
        transfer_count = host->endpoint[EP2I(ep)].iso_transfer_count;
        usbredirhost_cancel_iso_stream_unlocked(host, ep);
        r = libusb_clear_halt(host->handle, ep);
        if (r < 0) {
            usbredirhost_send_iso_status(host, id, ep, usb_redir_stall);
            return 2;
        }
        status = usbredirhost_alloc_iso_stream(host, ep, pkts_per_transfer,
                                               transfer_count);
        if (status != usb_redir_success) {
            usbredirhost_send_iso_status(host, id, ep, usb_redir_stall);
            return 2;
        }
        if (ep & LIBUSB_ENDPOINT_IN) {
            for (i = 0; i < host->endpoint[EP2I(ep)].iso_transfer_count; i++) {
                host->endpoint[EP2I(ep)].iso_transfer[i]->id =
                    i * host->endpoint[EP2I(ep)].iso_pkts_per_transfer;
                status = usbredirhost_submit_iso_transfer_unlocked(host,
                                    host->endpoint[EP2I(ep)].iso_transfer[i]);
                if (status != usb_redir_success) {
                    usbredirhost_send_iso_status(host, id, ep,
                                                 usb_redir_stall);
                    return 2;
                }
            }
            host->endpoint[EP2I(ep)].iso_started = 1;
        }
        /* No iso status message, stall successfully cleared */
        return 2;
    }
    case LIBUSB_TRANSFER_NO_DEVICE:
        usbredirhost_handle_disconnect(host);
        return 2;
    case LIBUSB_TRANSFER_OVERFLOW:
    case LIBUSB_TRANSFER_ERROR:
    case LIBUSB_TRANSFER_TIMED_OUT:
    default:
        ERROR("iso stream error on endpoint %02X: %d", (unsigned int)ep, r);
        return 1;
    }
}

static void usbredirhost_iso_packet_complete(
    struct libusb_transfer *libusb_transfer)
{
    struct usbredirtransfer *transfer = libusb_transfer->user_data;
    uint8_t ep = libusb_transfer->endpoint;
    struct usbredirhost *host = transfer->host;
    int i, r, len, status;

    LOCK(host);
    if (transfer->cancelled) {
        host->cancels_pending--;
        usbredirhost_free_transfer(transfer);
        goto unlock;
    }

    /* Mark transfer completed (iow not submitted) */
    transfer->iso_packet_idx = 0;

    /* Check overal transfer status */
    r = libusb_transfer->status;
    switch (usbredirhost_handle_iso_status(host, transfer->id, ep, r)) {
    case 0:
        break;
    case 1:
        status = libusb_status_or_error_to_redir_status(host, r);
        if (ep & LIBUSB_ENDPOINT_IN) {
            struct usb_redir_iso_packet_header iso_packet = {
                .endpoint = ep,
                .status   = status,
                .length   = 0
            };
            usbredirparser_send_iso_packet(host->parser, transfer->id,
                           &iso_packet, NULL, 0);
            transfer->id += libusb_transfer->num_iso_packets;
            goto resubmit;
        } else {
            usbredirhost_send_iso_status(host, transfer->id, ep, status);
            goto unlock;
        }
        break;
    case 2:
        goto unlock;
    }

    /* Check per packet status and send ok input packets to usb-guest */
    for (i = 0; i < libusb_transfer->num_iso_packets; i++) {
        r   = libusb_transfer->iso_packet_desc[i].status;
        len = libusb_transfer->iso_packet_desc[i].actual_length;
        status = libusb_status_or_error_to_redir_status(host, r);
        switch (usbredirhost_handle_iso_status(host, transfer->id, ep, r)) {
        case 0:
            break;
        case 1:
            if (ep & LIBUSB_ENDPOINT_IN) {
                len = 0;
            } else {
                usbredirhost_send_iso_status(host, transfer->id, ep, status);
                goto unlock; /* We send max one iso status message per urb */
            }
            break;
        case 2:
            goto unlock;
        }
        if (ep & LIBUSB_ENDPOINT_IN) {
            struct usb_redir_iso_packet_header iso_packet = {
                .endpoint = ep,
                .status   = status,
                .length   = len
            };
            DEBUG("iso-out ep %02X status %d len %d", ep, status, len);
            usbredirparser_send_iso_packet(host->parser, transfer->id,
                           &iso_packet,
                           libusb_get_iso_packet_buffer(libusb_transfer, i),
                           len);
            transfer->id++;
        } else {
            DEBUG("iso-in complete ep %02X pkt %d len %d id %d",
                  ep, i, len, transfer->id);
        }
    }

    /* And for input transfers resubmit the transfer (output transfers
       get resubmitted when they have all their packets filled with data) */
    if (ep & LIBUSB_ENDPOINT_IN) {
resubmit:
        transfer->id += (host->endpoint[EP2I(ep)].iso_transfer_count - 1) *
                        libusb_transfer->num_iso_packets;
        status = usbredirhost_submit_iso_transfer_unlocked(host, transfer);
        if (status != usb_redir_success) {
            usbredirhost_send_iso_status(host, transfer->id, ep,
                                         usb_redir_stall);
        }
    } else {
        for (i = 0; i < host->endpoint[EP2I(ep)].iso_transfer_count; i++) {
            transfer = host->endpoint[EP2I(ep)].iso_transfer[i];
            if (transfer->iso_packet_idx == ISO_SUBMITTED_IDX)
                break;
        }
        if (i == host->endpoint[EP2I(ep)].iso_transfer_count) {
            DEBUG("underflow of iso out queue on ep: %02X", ep);
            /* Re-fill buffers before submitting urbs again */
            for (i = 0; i < host->endpoint[EP2I(ep)].iso_transfer_count; i++)
                host->endpoint[EP2I(ep)].iso_transfer[i]->iso_packet_idx = 0;
            host->endpoint[EP2I(ep)].iso_out_idx = 0;
            host->endpoint[EP2I(ep)].iso_started = 0;
            host->endpoint[EP2I(ep)].iso_drop_packets = 0;
        }
    }
unlock:
    UNLOCK(host);
    FLUSH(host);
}

static int usbredirhost_alloc_iso_stream(struct usbredirhost *host,
    uint8_t ep, uint8_t pkts_per_transfer, uint8_t transfer_count)
{
    int i, buf_size;
    unsigned char *buffer;

    if (host->endpoint[EP2I(ep)].type != usb_redir_type_iso) {
        ERROR("start iso stream on non iso endpoint");
        return usb_redir_inval;
    }

    if (   pkts_per_transfer < 1 ||
           pkts_per_transfer > MAX_ISO_PACKETS_PER_TRANSFER ||
           transfer_count < 1 ||
           transfer_count > MAX_ISO_TRANSFER_COUNT) {
        ERROR("start iso stream pkts_per_urb or no_urbs invalid");
        return usb_redir_inval;
    }

    if (host->endpoint[EP2I(ep)].iso_transfer_count) {
        ERROR("received iso start for already started iso stream");
        return usb_redir_inval;
    }

    DEBUG("allocating iso stream ep %02X packet-size %d pkts %d urbs %d",
          ep, host->endpoint[EP2I(ep)].max_packetsize, pkts_per_transfer, transfer_count);
    for (i = 0; i < transfer_count; i++) {
        host->endpoint[EP2I(ep)].iso_transfer[i] =
            usbredirhost_alloc_transfer(host, pkts_per_transfer);
        if (!host->endpoint[EP2I(ep)].iso_transfer[i]) {
            goto alloc_error;
        }

        buf_size = host->endpoint[EP2I(ep)].max_packetsize * pkts_per_transfer;
        buffer = malloc(buf_size);
        if (!buffer) {
            goto alloc_error;
        }
        libusb_fill_iso_transfer(
            host->endpoint[EP2I(ep)].iso_transfer[i]->transfer, host->handle,
            ep, buffer, buf_size, pkts_per_transfer,
            usbredirhost_iso_packet_complete,
            host->endpoint[EP2I(ep)].iso_transfer[i], ISO_TIMEOUT);
        libusb_set_iso_packet_lengths(
            host->endpoint[EP2I(ep)].iso_transfer[i]->transfer,
            host->endpoint[EP2I(ep)].max_packetsize);
    }
    host->endpoint[EP2I(ep)].iso_out_idx = 0;
    host->endpoint[EP2I(ep)].iso_drop_packets = 0;
    host->endpoint[EP2I(ep)].iso_pkts_per_transfer = pkts_per_transfer;
    host->endpoint[EP2I(ep)].iso_transfer_count = transfer_count;

    return usb_redir_success;

alloc_error:
    ERROR("out of memory allocating iso stream buffers");
    do {
        usbredirhost_free_transfer(host->endpoint[EP2I(ep)].iso_transfer[i]);
        host->endpoint[EP2I(ep)].iso_transfer[i] = NULL;
        i--;
    } while (i >= 0);
    return usb_redir_ioerror;
}

static void usbredirhost_cancel_iso_stream_unlocked(struct usbredirhost *host,
    uint8_t ep)
{
    int i;
    struct usbredirtransfer *transfer;

    for (i = 0; i < host->endpoint[EP2I(ep)].iso_transfer_count; i++) {
        transfer = host->endpoint[EP2I(ep)].iso_transfer[i];
        if (transfer->iso_packet_idx == ISO_SUBMITTED_IDX) {
            libusb_cancel_transfer(transfer->transfer);
            transfer->cancelled = 1;
            host->cancels_pending++;
        } else {
            usbredirhost_free_transfer(transfer);
        }
        host->endpoint[EP2I(ep)].iso_transfer[i] = NULL;
    }
    host->endpoint[EP2I(ep)].iso_out_idx = 0;
    host->endpoint[EP2I(ep)].iso_started = 0;
    host->endpoint[EP2I(ep)].iso_drop_packets = 0;
    host->endpoint[EP2I(ep)].iso_pkts_per_transfer = 0;
    host->endpoint[EP2I(ep)].iso_transfer_count = 0;
}

static void usbredirhost_cancel_iso_stream(struct usbredirhost *host,
    uint8_t ep)
{
    LOCK(host);
    usbredirhost_cancel_iso_stream_unlocked(host, ep);
    UNLOCK(host);
}

/**************************************************************************/

static void usbredirhost_send_interrupt_recv_status(struct usbredirhost *host,
    uint32_t id, uint8_t endpoint, uint8_t status)
{
    struct usb_redir_interrupt_receiving_status_header interrupt_status;

    interrupt_status.endpoint = endpoint;
    interrupt_status.status = status;
    usbredirparser_send_interrupt_receiving_status(host->parser, id,
                                                   &interrupt_status);
}

static int usbredirhost_submit_interrupt_in_transfer(struct usbredirhost *host,
    uint8_t ep)
{
    int r;
    struct usbredirtransfer *transfer;

    transfer = host->endpoint[EP2I(ep)].interrupt_in_transfer;
    r = libusb_submit_transfer(transfer->transfer);
    if (r < 0) {
        ERROR("submitting interrupt transfer on ep %02X: %d", ep, r);
        usbredirhost_free_transfer(transfer);
        host->endpoint[EP2I(ep)].interrupt_in_transfer = NULL;
        return usb_redir_stall;
    }
    return usb_redir_success;
}

static void usbredirhost_interrupt_packet_complete(
    struct libusb_transfer *libusb_transfer)
{
    struct usbredirtransfer *transfer = libusb_transfer->user_data;
    uint8_t ep = libusb_transfer->endpoint;
    struct usb_redir_interrupt_packet_header interrupt_packet;
    struct usbredirhost *host = transfer->host;
    int len, status, r;

    status = libusb_status_or_error_to_redir_status(host,
                                                    libusb_transfer->status);
    len = libusb_transfer->actual_length;
    DEBUG("interrupt complete ep %02X status %d len %d", ep, status, len);

    if (!(ep & LIBUSB_ENDPOINT_IN)) {
        /* Output endpoints are easy */
        interrupt_packet = transfer->interrupt_packet;
        interrupt_packet.status = status;
        interrupt_packet.length = len;
        usbredirparser_send_interrupt_packet(host->parser, transfer->id,
                                             &interrupt_packet, NULL, 0);
        usbredirhost_remove_and_free_transfer(transfer);
        FLUSH(host);
        return;
    }

    /* Everything below is for input endpoints */
    LOCK(host);
    if (transfer->cancelled) {
        host->cancels_pending--;
        usbredirhost_free_transfer(transfer);
        goto unlock;
    }

    usbredirhost_log_data(host, "interrupt data in:",
                          libusb_transfer->buffer, len);
    r = libusb_transfer->status;
    switch (r) {
    case LIBUSB_TRANSFER_COMPLETED:
        break;
    case LIBUSB_TRANSFER_STALL:
        WARNING("interrupt endpoint %02X stalled, clearing stall", ep);
        r = libusb_clear_halt(host->handle, ep);
        if (r < 0) {
            /* Failed to clear stall, stop receiving */
            usbredirhost_send_interrupt_recv_status(host, transfer->id, ep,
                                                    usb_redir_stall);
            usbredirhost_free_transfer(transfer);
            host->endpoint[EP2I(ep)].interrupt_in_transfer = NULL;
            goto unlock;
        }
        transfer->id = 0;
        goto resubmit;
    case LIBUSB_TRANSFER_NO_DEVICE:
        usbredirhost_handle_disconnect(host);
        usbredirhost_free_transfer(transfer);
        host->endpoint[EP2I(ep)].interrupt_in_transfer = NULL;
        goto unlock;
    default:
        ERROR("interrupt in error on endpoint %02X: %d", ep, r);
        len = 0;
    }

    interrupt_packet.endpoint = ep;
    interrupt_packet.status   = status;
    interrupt_packet.length   = len;
    usbredirparser_send_interrupt_packet(host->parser, transfer->id,
                          &interrupt_packet, transfer->transfer->buffer, len);
    transfer->id++;

resubmit:
    status = usbredirhost_submit_interrupt_in_transfer(host, ep);
    if (status != usb_redir_success) {
        usbredirhost_send_interrupt_recv_status(host, transfer->id, ep,
                                                status);
    }
unlock:
    UNLOCK(host);
    FLUSH(host);
}

static int usbredirhost_alloc_interrupt_in_transfer(struct usbredirhost *host,
    uint8_t ep)
{
    int buf_size;
    unsigned char *buffer;
    struct usbredirtransfer *transfer;

    if (host->endpoint[EP2I(ep)].type != usb_redir_type_interrupt) {
        ERROR("received start interrupt packet for non interrupt ep %02X", ep);
        return usb_redir_inval;
    }

    if (!(ep & LIBUSB_ENDPOINT_IN)) {
        ERROR("received start interrupt packet for non input ep %02X", ep);
        return usb_redir_inval;
    }

    transfer = usbredirhost_alloc_transfer(host, 0);
    if (!transfer) {
        return usb_redir_ioerror;
    }

    buf_size = host->endpoint[EP2I(ep)].max_packetsize;
    buffer = malloc(buf_size);
    if (!buffer) {
        ERROR("out of memory allocating interrupt buffer");
        usbredirhost_free_transfer(transfer);
        return usb_redir_ioerror;
    }

    libusb_fill_interrupt_transfer(transfer->transfer, host->handle, ep,
        buffer, buf_size, usbredirhost_interrupt_packet_complete, transfer,
        INTERRUPT_TIMEOUT);
    host->endpoint[EP2I(ep)].interrupt_in_transfer = transfer;
    return usb_redir_success;
}

static void usbredirhost_cancel_interrupt_in_transfer_unlocked(
    struct usbredirhost *host, uint8_t ep)
{
    struct usbredirtransfer *transfer;

    transfer = host->endpoint[EP2I(ep)].interrupt_in_transfer;
    if (!transfer)
        return; /* Already stopped */

    libusb_cancel_transfer(transfer->transfer);
    transfer->cancelled = 1;
    host->cancels_pending++;
    host->endpoint[EP2I(ep)].interrupt_in_transfer = NULL;
}

/**************************************************************************/

static void usbredirhost_hello(void *priv, struct usb_redir_hello_header *h)
{
    struct usbredirhost *host = priv;

    if (host->connect_pending)
        usbredirhost_send_device_connect(host);
}

static void usbredirhost_reset(void *priv)
{
    struct usbredirhost *host = priv;
    int r;

    if (host->disconnected) {
        return;
    }

    r = libusb_reset_device(host->handle);
    if (r == 0) {
        /* Some devices need some time to settle before firing more cmds */
        usleep(100000);
    } else {
        ERROR("resetting device: %d", r);
        usbredirhost_handle_disconnect(host);
        FLUSH(host);
    }
}

static void usbredirhost_set_configuration(void *priv, uint32_t id,
    struct usb_redir_set_configuration_header *set_config)
{
    struct usbredirhost *host = priv;
    int r;
    struct usb_redir_configuration_status_header status = {
        .status = usb_redir_success,
    };

    if (host->disconnected) {
        status.status = usb_redir_ioerror;
        goto exit;
    }

    if (set_config->configuration == host->active_config) {
        goto exit;
    }

    usbredirhost_cancel_pending_urbs(host);

    status.status = usbredirhost_release(host, 0);
    if (status.status != usb_redir_success) {
        goto exit;
    }

    r = libusb_set_configuration(host->handle, set_config->configuration);
    if (r < 0) {
        ERROR("could not set active configuration to %d: %d",
              (int)set_config->configuration, r);
        status.status = usb_redir_ioerror;
        goto exit;
    }

    host->active_config = set_config->configuration;
    status.status = usbredirhost_claim(host);
    if (status.status != usb_redir_success) {
        usbredirhost_clear_device(host);
        host->read_status = usbredirhost_read_device_lost;
        goto exit;
    }

    usbredirhost_send_interface_n_ep_info(host);

exit:
    status.configuration = host->active_config;
    usbredirparser_send_configuration_status(host->parser, id, &status);
    FLUSH(host);
}

static void usbredirhost_get_configuration(void *priv, uint32_t id)
{
    struct usbredirhost *host = priv;
    struct usb_redir_configuration_status_header status;

    if (host->disconnected)
        status.status = usb_redir_ioerror;
    else
        status.status = usb_redir_success;
    status.configuration = host->active_config;
    usbredirparser_send_configuration_status(host->parser, id, &status);
    FLUSH(host);
}

static void usbredirhost_set_alt_setting(void *priv, uint32_t id,
    struct usb_redir_set_alt_setting_header *set_alt_setting)
{
    struct usbredirhost *host = priv;
    int i, j, r;
    struct usb_redir_alt_setting_status_header status = {
        .status = usb_redir_success,
    };

    if (host->disconnected) {
        status.status = usb_redir_ioerror;
        status.alt = -1;
        goto exit_unknown_interface;
    }

    i = usbredirhost_bInterfaceNumber_to_index(host,
                                               set_alt_setting->interface);
    if (i == -1) {
        status.status = usb_redir_inval;
        status.alt = -1;
        goto exit_unknown_interface;
    }

    usbredirhost_cancel_pending_urbs_on_interface(host, i);

    r = libusb_set_interface_alt_setting(host->handle,
                                         set_alt_setting->interface,
                                         set_alt_setting->alt);
    if (r < 0) {
        ERROR("could not set alt setting for interface %d to %d: %d",
              (int)set_alt_setting->interface, (int)set_alt_setting->alt, r);
        status.status = libusb_status_or_error_to_redir_status(host, r);
        goto exit;
    }

    /* The new alt setting may have lost endpoints compared to the old! ->
       Clear settings for all endpoints which used to be part of the intf. */
    for (j = 0; j < MAX_ENDPOINTS; j++) {
        if (host->endpoint[j].interface != set_alt_setting->interface)
            continue;

        if ((i & 0x0f) == 0) {
            host->endpoint[j].type = usb_redir_type_control;
        } else {
            host->endpoint[j].type = usb_redir_type_invalid;
        }
        host->endpoint[j].interval = 0;
        host->endpoint[j].interface = 0;
    }

    host->alt_setting[i] = set_alt_setting->alt;
    usbredirhost_parse_interface(host, i);
    usbredirhost_send_interface_n_ep_info(host);

exit:
    status.alt = host->alt_setting[i];
exit_unknown_interface:
    status.interface = set_alt_setting->interface;
    usbredirparser_send_alt_setting_status(host->parser, id, &status);
    FLUSH(host);
}

static void usbredirhost_get_alt_setting(void *priv, uint32_t id,
    struct usb_redir_get_alt_setting_header *get_alt_setting)
{
    struct usbredirhost *host = priv;
    struct usb_redir_alt_setting_status_header status;
    int i;

    if (host->disconnected) {
        status.status = usb_redir_ioerror;
        status.alt = -1;
        goto exit;
    }

    i = usbredirhost_bInterfaceNumber_to_index(host,
                                               get_alt_setting->interface);
    if (i >= 0) {
        status.status = usb_redir_success;
        status.alt = host->alt_setting[i];
    } else {
        status.status = usb_redir_inval;
        status.alt = -1;
    }

exit:
    status.interface = get_alt_setting->interface;
    usbredirparser_send_alt_setting_status(host->parser, id, &status);
    FLUSH(host);
}

static void usbredirhost_start_iso_stream(void *priv, uint32_t id,
    struct usb_redir_start_iso_stream_header *start_iso_stream)
{
    struct usbredirhost *host = priv;
    int i, status;
    uint8_t ep = start_iso_stream->endpoint;

    LOCK(host);

    if (host->disconnected) {
        status = usb_redir_ioerror;
        goto leave;
    }

    status = usbredirhost_alloc_iso_stream(host, ep,
                   start_iso_stream->pkts_per_urb, start_iso_stream->no_urbs);
    if (status != usb_redir_success) {
        status = usb_redir_stall;
        goto leave;
    }

    /* For input endpoints submit the transfers now */
    if (start_iso_stream->endpoint & LIBUSB_ENDPOINT_IN) {
        for (i = 0; i < host->endpoint[EP2I(ep)].iso_transfer_count; i++) {
            host->endpoint[EP2I(ep)].iso_transfer[i]->id =
                i * host->endpoint[EP2I(ep)].iso_pkts_per_transfer;
            status = usbredirhost_submit_iso_transfer_unlocked(host,
                         host->endpoint[EP2I(ep)].iso_transfer[i]);
            if (status != usb_redir_success) {
                status = usb_redir_stall;
                goto leave;
            }
        }
        host->endpoint[EP2I(ep)].iso_started = 1;
    }
leave:
    UNLOCK(host);
    usbredirhost_send_iso_status(host, id, ep, status);
    FLUSH(host);
}

static void usbredirhost_stop_iso_stream(void *priv, uint32_t id,
    struct usb_redir_stop_iso_stream_header *stop_iso_stream)
{
    struct usbredirhost *host = priv;
    uint8_t ep = stop_iso_stream->endpoint;
    uint8_t status = usb_redir_success;

    if (host->disconnected) {
        status = usb_redir_ioerror;
        goto exit;
    }

    usbredirhost_cancel_iso_stream(host, ep);

exit:
    usbredirhost_send_iso_status(host, id, ep, status);
    FLUSH(host);
}

static void usbredirhost_start_interrupt_receiving(void *priv, uint32_t id,
    struct usb_redir_start_interrupt_receiving_header *start_interrupt_receiving)
{
    struct usbredirhost *host = priv;
    uint8_t ep = start_interrupt_receiving->endpoint;
    int status;

    LOCK(host);

    if (host->disconnected) {
        status = usb_redir_ioerror;
        goto leave;
    }

    if (host->endpoint[EP2I(ep)].interrupt_in_transfer) {
        ERROR("received interrupt start for already active ep %02X", ep);
        status = usb_redir_inval;
        goto leave;
    }

    status = usbredirhost_alloc_interrupt_in_transfer(host, ep);
    if (status != usb_redir_success) {
        status = usb_redir_stall;
        goto leave;
    }
    status = usbredirhost_submit_interrupt_in_transfer(host, ep);
leave:
    UNLOCK(host);
    usbredirhost_send_interrupt_recv_status(host, id, ep, status);
    FLUSH(host);
}

static void usbredirhost_stop_interrupt_receiving(void *priv, uint32_t id,
    struct usb_redir_stop_interrupt_receiving_header *stop_interrupt_receiving)
{
    struct usbredirhost *host = priv;
    uint8_t ep = stop_interrupt_receiving->endpoint;
    uint8_t status = usb_redir_success;

    if (host->disconnected) {
        status = usb_redir_ioerror;
        goto exit;
    }

    LOCK(host);
    usbredirhost_cancel_interrupt_in_transfer_unlocked(host, ep);
    UNLOCK(host);

exit:
    usbredirhost_send_interrupt_recv_status(host, id, ep, status);
    FLUSH(host);
}

static void usbredirhost_alloc_bulk_streams(void *priv, uint32_t id,
    struct usb_redir_alloc_bulk_streams_header *alloc_bulk_streams)
{
    /* struct usbredirhost *host = priv; */
}

static void usbredirhost_free_bulk_streams(void *priv, uint32_t id,
    struct usb_redir_free_bulk_streams_header *free_bulk_streams)
{
    /* struct usbredirhost *host = priv; */
}

static void usbredirhost_filter_reject(void *priv)
{
    struct usbredirhost *host = priv;

    if (host->disconnected)
        return;

    INFO("device rejected");
    host->read_status = usbredirhost_read_device_rejected;
}

static void usbredirhost_filter_filter(void *priv,
    struct usbredirfilter_rule *rules, int rules_count)
{
    struct usbredirhost *host = priv;

    free(host->filter_rules);
    host->filter_rules = rules;
    host->filter_rules_count = rules_count;
}

static void usbredirhost_device_disconnect_ack(void *priv)
{
    struct usbredirhost *host = priv;

    if (!host->wait_disconnect) {
        ERROR("Received disconnect ack without sending a disconnect");
        return;
    }

    host->wait_disconnect = 0;

    if (host->connect_pending)
        usbredirhost_send_device_connect(host);
}

/**************************************************************************/

static void usbredirhost_cancel_data_packet(void *priv, uint32_t id)
{
    struct usbredirhost *host = priv;
    struct usbredirtransfer *t;

    /*
     * This is a bit tricky, we are run from a parser read callback, while
     * at the same time the packet completion callback may run from another
     * thread.
     *
     * Since the completion handler will remove the transter from our list
     * and *free* the transfer, we must do the libusb_cancel_transfer()
     * with the lock held to ensure that it is not freed while we try to
     * cancel it.
     *
     * Doing this means libusb taking the transfer lock, while
     * we are holding our own lock, this is ok, since libusb releases the
     * transfer lock before calling the packet completion callback, so there
     * is no deadlock here.
     */

    LOCK(host);
    for (t = host->transfers_head.next; t; t = t->next) {
        if (t->id == id) {
            break;
        }
    }

    /*
     * Note not finding the transfer is not an error, the transfer may have
     * completed by the time we receive the cancel.
     */
    if (t) {
        libusb_cancel_transfer(t->transfer);
    }
    UNLOCK(host);
}

static void usbredirhost_control_packet_complete(
    struct libusb_transfer *libusb_transfer)
{
    struct usb_redir_control_packet_header control_packet;
    struct usbredirtransfer *transfer = libusb_transfer->user_data;
    struct usbredirhost *host = transfer->host;

    control_packet = transfer->control_packet;
    control_packet.status = libusb_status_or_error_to_redir_status(host,
                                                  libusb_transfer->status);
    control_packet.length = libusb_transfer->actual_length;

    DEBUG("control complete ep %02X status %d len %d", control_packet.endpoint,
          control_packet.status, control_packet.length);

    if (control_packet.endpoint & LIBUSB_ENDPOINT_IN) {
        usbredirhost_log_data(host, "ctrl data in:",
                         libusb_transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE,
                         libusb_transfer->actual_length);
        usbredirparser_send_control_packet(host->parser, transfer->id,
                                           &control_packet,
                                           libusb_transfer->buffer +
                                               LIBUSB_CONTROL_SETUP_SIZE,
                                           libusb_transfer->actual_length);
    } else {
        usbredirparser_send_control_packet(host->parser, transfer->id,
                                           &control_packet, NULL, 0);
    }

    usbredirhost_remove_and_free_transfer(transfer);
    FLUSH(host);
}

static void usbredirhost_send_control_status(struct usbredirhost *host,
    uint32_t id, struct usb_redir_control_packet_header *control_packet,
    uint8_t status)
{
    control_packet->status = status;
    control_packet->length = 0;
    usbredirparser_send_control_packet(host->parser, id, control_packet,
                                       NULL, 0);
}

static void usbredirhost_control_packet(void *priv, uint32_t id,
    struct usb_redir_control_packet_header *control_packet,
    uint8_t *data, int data_len)
{
    struct usbredirhost *host = priv;
    uint8_t ep = control_packet->endpoint;
    struct usbredirtransfer *transfer;
    unsigned char *buffer;
    int r;

    if (host->disconnected) {
        usbredirhost_send_control_status(host, id, control_packet,
                                         usb_redir_ioerror);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    /* Verify endpoint type */
    if (host->endpoint[EP2I(ep)].type != usb_redir_type_control) {
        ERROR("control packet on non control ep %02X", ep);
        usbredirhost_send_control_status(host, id, control_packet,
                                         usb_redir_inval);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    /* If it is a clear stall, we need to do an actual clear stall, rather then
       just forward the control packet, so that the usbhost usbstack knows
       the stall is cleared */
    if (control_packet->requesttype == LIBUSB_RECIPIENT_ENDPOINT &&
            control_packet->request == LIBUSB_REQUEST_CLEAR_FEATURE &&
            control_packet->value == 0x00 && data_len == 0) {
        r = libusb_clear_halt(host->handle, control_packet->index);
        r = libusb_status_or_error_to_redir_status(host, r);
        DEBUG("clear halt ep %02X status %d", control_packet->index, r);
        usbredirhost_send_control_status(host, id, control_packet, r);
        FLUSH(host);
        return;
    }

    buffer = malloc(LIBUSB_CONTROL_SETUP_SIZE + control_packet->length);
    if (!buffer) {
        ERROR("out of memory allocating transfer buffer, dropping packet");
        usbredirparser_free_packet_data(host->parser, data);
        return;
    }

    transfer = usbredirhost_alloc_transfer(host, 0);
    if (!transfer) {
        free(buffer);
        usbredirparser_free_packet_data(host->parser, data);
        return;
    }

    libusb_fill_control_setup(buffer,
                              control_packet->requesttype,
                              control_packet->request,
                              control_packet->value,
                              control_packet->index,
                              control_packet->length);

    if (!(ep & LIBUSB_ENDPOINT_IN)) {
        usbredirhost_log_data(host, "ctrl data out:", data, data_len);
        memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, data, data_len);
        usbredirparser_free_packet_data(host->parser, data);
    }

    libusb_fill_control_transfer(transfer->transfer, host->handle, buffer,
                                 usbredirhost_control_packet_complete,
                                 transfer, CTRL_TIMEOUT);
    transfer->id = id;
    transfer->control_packet = *control_packet;

    usbredirhost_add_transfer(host, transfer);

    r = libusb_submit_transfer(transfer->transfer);
    if (r < 0) {
        ERROR("submitting control transfer on ep %02X: %d", ep, r);
        transfer->transfer->actual_length = 0;
        transfer->transfer->status = r;
        usbredirhost_control_packet_complete(transfer->transfer);
    }
}

static void usbredirhost_bulk_packet_complete(
    struct libusb_transfer *libusb_transfer)
{
    struct usb_redir_bulk_packet_header bulk_packet;
    struct usbredirtransfer *transfer = libusb_transfer->user_data;
    struct usbredirhost *host = transfer->host;

    bulk_packet = transfer->bulk_packet;
    bulk_packet.status = libusb_status_or_error_to_redir_status(host,
                                                  libusb_transfer->status);
    bulk_packet.length = libusb_transfer->actual_length;

    DEBUG("bulk complete ep %02X status %d len %d", bulk_packet.endpoint,
          bulk_packet.status, bulk_packet.length);

    if (bulk_packet.endpoint & LIBUSB_ENDPOINT_IN) {
        usbredirhost_log_data(host, "bulk data in:",
                              libusb_transfer->buffer,
                              libusb_transfer->actual_length);
        usbredirparser_send_bulk_packet(host->parser, transfer->id,
                                        &bulk_packet,
                                        libusb_transfer->buffer,
                                        libusb_transfer->actual_length);
    } else {
        usbredirparser_send_bulk_packet(host->parser, transfer->id,
                                        &bulk_packet, NULL, 0);
    }

    usbredirhost_remove_and_free_transfer(transfer);
    FLUSH(host);
}

static void usbredirhost_send_bulk_status(struct usbredirhost *host,
    uint32_t id, struct usb_redir_bulk_packet_header *bulk_packet,
    uint8_t status)
{
    bulk_packet->status = status;
    bulk_packet->length = 0;
    usbredirparser_send_bulk_packet(host->parser, id, bulk_packet, NULL, 0);
}

static void usbredirhost_bulk_packet(void *priv, uint32_t id,
    struct usb_redir_bulk_packet_header *bulk_packet,
    uint8_t *data, int data_len)
{
    struct usbredirhost *host = priv;
    uint8_t ep = bulk_packet->endpoint;
    struct usbredirtransfer *transfer;
    int r;

    DEBUG("bulk submit ep %02X len %d", ep, bulk_packet->length);

    if (host->disconnected) {
        usbredirhost_send_bulk_status(host, id, bulk_packet,
                                      usb_redir_ioerror);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    if (host->endpoint[EP2I(ep)].type != usb_redir_type_bulk) {
        ERROR("bulk packet on non bulk ep %02X", ep);
        usbredirhost_send_bulk_status(host, id, bulk_packet, usb_redir_inval);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    if (ep & LIBUSB_ENDPOINT_IN) {
        data = malloc(bulk_packet->length);
        if (!data) {
            ERROR("out of memory allocating bulk buffer, dropping packet");
            return;
        }
    } else {
        usbredirhost_log_data(host, "bulk data out:", data, data_len);
        /* Note no memcpy, we can re-use the data buffer the parser
           malloc-ed for us and expects us to free */
    }

    transfer = usbredirhost_alloc_transfer(host, 0);
    if (!transfer) {
        free(data);
        return;
    }

    libusb_fill_bulk_transfer(transfer->transfer, host->handle, ep,
                              data, bulk_packet->length,
                              usbredirhost_bulk_packet_complete,
                              transfer, CTRL_TIMEOUT);
    transfer->id = id;
    transfer->bulk_packet = *bulk_packet;

    usbredirhost_add_transfer(host, transfer);

    r = libusb_submit_transfer(transfer->transfer);
    if (r < 0) {
        ERROR("submitting bulk transfer on ep %02X: %d", ep, r);
        transfer->transfer->actual_length = 0;
        transfer->transfer->status = r;
        usbredirhost_bulk_packet_complete(transfer->transfer);
    }
}

static void usbredirhost_iso_packet(void *priv, uint32_t id,
    struct usb_redir_iso_packet_header *iso_packet,
    uint8_t *data, int data_len)
{
    struct usbredirhost *host = priv;
    uint8_t ep = iso_packet->endpoint;
    struct usbredirtransfer *transfer;
    int i, j, status = usb_redir_success;

    LOCK(host);

    if (host->disconnected) {
        status = usb_redir_ioerror;
        goto leave;
    }

    if (host->endpoint[EP2I(ep)].type != usb_redir_type_iso) {
        ERROR("received iso packet for non iso ep %02X", ep);
        status = usb_redir_inval;
        goto leave;
    }

    if (host->endpoint[EP2I(ep)].iso_transfer_count == 0) {
        ERROR("received iso out packet for non started iso stream");
        status = usb_redir_inval;
        goto leave;
    }

    if (data_len > host->endpoint[EP2I(ep)].max_packetsize) {
        ERROR("received iso out packet is larger than wMaxPacketSize");
        status = usb_redir_inval;
        goto leave;
    }

    if (host->endpoint[EP2I(ep)].iso_drop_packets) {
        host->endpoint[EP2I(ep)].iso_drop_packets--;
        goto leave;
    }

    i = host->endpoint[EP2I(ep)].iso_out_idx;
    transfer = host->endpoint[EP2I(ep)].iso_transfer[i];
    j = transfer->iso_packet_idx;
    if (j == ISO_SUBMITTED_IDX) {
        DEBUG("overflow of iso out queue on ep: %02X, dropping packet", ep);
        /* Since we're interupting the stream anyways, drop enough packets to
           get back to our target buffer size */
        host->endpoint[EP2I(ep)].iso_drop_packets =
                     (host->endpoint[EP2I(ep)].iso_pkts_per_transfer *
                      host->endpoint[EP2I(ep)].iso_transfer_count) / 2;
        goto leave;
    }

    /* Store the id of the first packet in the urb */
    if (j == 0) {
        transfer->id = id;
    }
    memcpy(libusb_get_iso_packet_buffer(transfer->transfer, j),
           data, data_len);
    transfer->transfer->iso_packet_desc[j].length = data_len;
    DEBUG("iso-in queue ep %02X urb %d pkt %d len %d id %d",
           ep, i, j, data_len, transfer->id);

    j++;
    transfer->iso_packet_idx = j;
    if (j == host->endpoint[EP2I(ep)].iso_pkts_per_transfer) {
        i = (i + 1) % host->endpoint[EP2I(ep)].iso_transfer_count;
        host->endpoint[EP2I(ep)].iso_out_idx = i;
        j = 0;
    }

    if (host->endpoint[EP2I(ep)].iso_started) {
        if (transfer->iso_packet_idx ==
                host->endpoint[EP2I(ep)].iso_pkts_per_transfer) {
            status = usbredirhost_submit_iso_transfer_unlocked(host, transfer);
            if (status != usb_redir_success) {
                status = usb_redir_stall;
                goto leave;
            }
        }
    } else {
        /* We've not started the stream (submitted some transfers) yet,
           do so once we have half our buffers filled */
        int available = i * host->endpoint[EP2I(ep)].iso_pkts_per_transfer + j;
        int needed = (host->endpoint[EP2I(ep)].iso_pkts_per_transfer *
                      host->endpoint[EP2I(ep)].iso_transfer_count) / 2;
        if (available == needed) {
            DEBUG("iso-in starting stream on ep %02X", ep);
            for (i = 0; i < host->endpoint[EP2I(ep)].iso_transfer_count / 2;
                    i++) {
                status = usbredirhost_submit_iso_transfer_unlocked(host,
                                    host->endpoint[EP2I(ep)].iso_transfer[i]);
                if (status != usb_redir_success) {
                    status = usb_redir_stall;
                    goto leave;
                }
            }
            host->endpoint[EP2I(ep)].iso_started = 1;
        }
    }

leave:
    UNLOCK(host);
    usbredirparser_free_packet_data(host->parser, data);
    if (status != usb_redir_success) {
        usbredirhost_send_iso_status(host, id, ep, status);
        FLUSH(host);
    }
}

static void usbredirhost_send_interrupt_status(struct usbredirhost *host,
    uint32_t id, struct usb_redir_interrupt_packet_header *interrupt_packet,
    uint8_t status)
{
    interrupt_packet->status = status;
    interrupt_packet->length = 0;
    usbredirparser_send_interrupt_packet(host->parser, id, interrupt_packet,
                                         NULL, 0);
}

static void usbredirhost_interrupt_packet(void *priv, uint32_t id,
    struct usb_redir_interrupt_packet_header *interrupt_packet,
    uint8_t *data, int data_len)
{
    struct usbredirhost *host = priv;
    uint8_t ep = interrupt_packet->endpoint;
    struct usbredirtransfer *transfer;
    int r;

    DEBUG("interrupt submit ep %02X len %d", ep, interrupt_packet->length);

    if (host->disconnected) {
        usbredirhost_send_interrupt_status(host, id, interrupt_packet,
                                           usb_redir_ioerror);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    if (host->endpoint[EP2I(ep)].type != usb_redir_type_interrupt) {
        ERROR("received interrupt packet for non interrupt ep %02X", ep);
        usbredirhost_send_interrupt_status(host, id, interrupt_packet,
                                           usb_redir_inval);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    if (data_len > host->endpoint[EP2I(ep)].max_packetsize) {
        ERROR("received interrupt out packet is larger than wMaxPacketSize");
        usbredirhost_send_interrupt_status(host, id, interrupt_packet,
                                           usb_redir_inval);
        usbredirparser_free_packet_data(host->parser, data);
        FLUSH(host);
        return;
    }

    usbredirhost_log_data(host, "interrupt data out:", data, data_len);

    /* Note no memcpy, we can re-use the data buffer the parser
       malloc-ed for us and expects us to free */

    transfer = usbredirhost_alloc_transfer(host, 0);
    if (!transfer) {
        usbredirparser_free_packet_data(host->parser, data);
        return;
    }

    libusb_fill_interrupt_transfer(transfer->transfer, host->handle, ep,
        data, data_len, usbredirhost_interrupt_packet_complete,
        transfer, INTERRUPT_TIMEOUT);
    transfer->id = id;
    transfer->interrupt_packet = *interrupt_packet;

    usbredirhost_add_transfer(host, transfer);

    r = libusb_submit_transfer(transfer->transfer);
    if (r < 0) {
        ERROR("submitting interrupt transfer on ep %02X: %d", ep, r);
        transfer->transfer->actual_length = 0;
        transfer->transfer->status = r;
        usbredirhost_interrupt_packet_complete(transfer->transfer);
    }
}

/**************************************************************************/

void usbredirhost_get_guest_filter(struct usbredirhost *host,
    const struct usbredirfilter_rule **rules_ret, int *rules_count_ret)
{
    *rules_ret = host->filter_rules;
    *rules_count_ret = host->filter_rules_count;
}

int usbredirhost_check_device_filter(const struct usbredirfilter_rule *rules,
    int rules_count, libusb_device *dev, int flags)
{
    int i, r, num_interfaces;
    struct libusb_device_descriptor dev_desc;
    struct libusb_config_descriptor *config;
    uint8_t interface_class[MAX_INTERFACES];
    uint8_t interface_subclass[MAX_INTERFACES];
    uint8_t interface_protocol[MAX_INTERFACES];

    r = libusb_get_device_descriptor(dev, &dev_desc);
    if (r < 0) {
        if (r == LIBUSB_ERROR_NO_MEM)
            return -ENOMEM;
        return -EIO;
    }

    r = libusb_get_active_config_descriptor(dev, &config);
    if (r < 0) {
        if (r == LIBUSB_ERROR_NO_MEM)
            return -ENOMEM;
        return -EIO;
    }
    num_interfaces = config->bNumInterfaces;
    for (i = 0; i < num_interfaces; i++) {
        const struct libusb_interface_descriptor *intf_desc =
            config->interface[i].altsetting;
        interface_class[i] = intf_desc->bInterfaceClass;
        interface_subclass[i] = intf_desc->bInterfaceSubClass;
        interface_protocol[i] = intf_desc->bInterfaceProtocol;
    }
    libusb_free_config_descriptor(config);

    return usbredirfilter_check(rules, rules_count, dev_desc.bDeviceClass,
                dev_desc.bDeviceSubClass, dev_desc.bDeviceProtocol,
                interface_class, interface_subclass, interface_protocol,
                num_interfaces, dev_desc.idVendor, dev_desc.idProduct,
                dev_desc.bcdDevice, flags);
}

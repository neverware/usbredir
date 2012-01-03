/* usbredirproto-compat.h usb redirection compatibility protocol definitions

   Copyright 2011 Red Hat, Inc.

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
#ifndef __USBREDIRPROTO_COMPAT_H
#define __USBREDIRPROTO_COMPAT_H

/* PACK macros borrowed from spice-protocol */
#ifdef __GNUC__

#define ATTR_PACKED __attribute__ ((__packed__))

#ifdef __MINGW32__
#pragma pack(push,1)
#endif

#else

#pragma pack(push)
#pragma pack(1)
#define ATTR_PACKED
#pragma warning(disable:4200)
#pragma warning(disable:4103)

#endif

#include <stdint.h>

struct usb_redir_device_connect_header_no_device_version {
    uint8_t speed;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
} ATTR_PACKED;

#undef ATTR_PACKED

#if defined(__MINGW32__) || !defined(__GNUC__)
#pragma pack(pop)
#endif

#endif
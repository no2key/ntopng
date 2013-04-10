/*
 *
 * (C) 2013 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#ifndef _IP_ADDRESS_H_
#define _IP_ADDRESS_H_

#include "ntop_includes.h"

struct ipAddress {
  u_int8_t ipVersion:3 /* Either 4 or 6 */, 
    localHost:1,
    notUsed:4 /* Future use */;

  union {
    struct in6_addr ipv6;
    u_int32_t ipv4; /* Host byte code */
  } ipType;
};

/* *********************************** */

class IpAddress {
 private:
  struct ipAddress addr;

  char* _intoaV4(unsigned int addr, char* buf, u_short bufLen);
  char* _intoa(char* buf, u_short bufLen);

 public:
  IpAddress();
  IpAddress(u_int32_t _ipv4);
  IpAddress(struct in6_addr _ipv6);

  inline void set_ipv4(u_int32_t _ipv4)       { addr.ipVersion = 4, addr.ipType.ipv4 = _ipv4; }
  inline void set_ipv6(struct in6_addr _ipv6) { addr.ipVersion = 6, memcpy(&addr.ipType.ipv6, &_ipv6, sizeof(_ipv6)); }
  int compare(IpAddress *ip);
  u_int key();

  char* print(char *str, u_int str_len);
};

#endif /* _IP_ADDRESS_H_ */
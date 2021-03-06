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

#ifndef _SIMPLE_STRING_HOST_H_
#define _SIMPLE_STRING_HOST_H_

#include "ntop_includes.h"

class SimpleStringHost : public GenericHost {
 private:
  char *keyname;
  u_int32_t id;

 public:
  SimpleStringHost(char *_key, u_int32_t _id);
  ~SimpleStringHost();
  
  inline char* host_key()     { return(keyname); };
  inline u_int32_t get_id()   { return(id);      };
};

#endif /* _SIMPLE_STRING_HOST_H_ */

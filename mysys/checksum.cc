/* Copyright (c) 2000, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file mysys/checksum.cc
*/

#include <stddef.h>
#include <sys/types.h>
#include <zlib.h>

#include "my_inttypes.h"
#include "my_sys.h"

/* HAVE_ARMV8_CRC32_INTRINSIC is defined for linux + aarch64 + crc32 intrinsic*/
#if defined(HAVE_ARMV8_CRC32_INTRINSIC)
#define ARM_CRC32_INTRINSIC_SUPPORTED
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>

/* There are multiple approaches to calculate crc.
Approach-1: Process 8 bytes then 4 bytes then 2 bytes and then 1 bytes
Approach-2: Process 8 bytes and remaining workload using 1 bytes
Apporach-3: Process 64 bytes at once by issuing 8 crc call and remaining
            using 8/1 combination.

Based on micro-benchmark testing we found that Approach-2 works best especially
given small chunk of variable data. */

MY_ATTRIBUTE((target("+crc")))
inline unsigned long aarch64_crc32_checksum(unsigned long crc32,
                                            const unsigned char *buf,
                                            unsigned int len) {
  uint32_t crc = static_cast<uint32_t>(crc32);
  crc = ~crc;

  const uint64_t *buf8 = (const uint64_t *)buf;
  while (len >= sizeof(uint64_t)) {
    crc = __crc32d(crc, *buf8++);
    len -= sizeof(uint64_t);
  }

  const uint8_t *buf1 = (const uint8_t *)buf8;
  while (len >= sizeof(uint8_t)) {
    crc = __crc32b(crc, *buf1++);
    len -= sizeof(uint8_t);
  }

  return (~crc);
}

/* Linux system call to findout CPU capabilities. */
inline bool aarch64_crc32_supported() {
  return (getauxval(AT_HWCAP) & HWCAP_CRC32);
}

typedef unsigned long (*my_crc32_func_t)(unsigned long crc,
                                         const unsigned char *ptr,
                                         unsigned int len);

/* Ideally all ARM 64 bit processor should support crc32 but if some model
doesn't support better to find it out through auxillary vector. */
my_crc32_func_t my_crc32 =
    aarch64_crc32_supported() ? aarch64_crc32_checksum : crc32;
#endif /* defined(HAVE_ARMV8_CRC32_INTRINSIC) */

/*
  Calculate a long checksum for a memoryblock.

  SYNOPSIS
    my_checksum()
      crc       start value for crc
      pos       pointer to memory block
      length    length of the block
*/

ha_checksum my_checksum(ha_checksum crc, const uchar *pos, size_t length) {
#ifdef ARM_CRC32_INTRINSIC_SUPPORTED
  return (ha_checksum)my_crc32((uint)crc, pos, (uint)length);
#else
  return (ha_checksum)crc32((uint)crc, pos, (uint)length);
#endif /* ARM_CRC32_INTRINSIC_SUPPORTED */
}

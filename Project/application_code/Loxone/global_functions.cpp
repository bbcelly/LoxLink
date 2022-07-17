//
//  global_functions.cpp
//
//  Created by Markus Fritze on 05.03.19.
//  Copyright (c) 2019 Markus Fritze. All rights reserved.
//

#include "global_functions.hpp"
#include <__cross_studio_io.h>
#include <string.h>

uint32_t gRandomSeed = 1;

uint16_t random_range(uint16_t minimum, uint16_t maximum) {
  gRandomSeed = 1103515245 * gRandomSeed + 12345;
  uint16_t value = (gRandomSeed >> 16) & 0x7FFF;
  uint32_t range = (uint32_t)maximum - minimum + 1;
  return value % range + minimum;
}

// setup a random seed
void random_init(uint32_t seed) {
  gRandomSeed = seed;
}

uint8_t crc8_default(const void *data, size_t len) {
  uint8_t crc = 0x00;
  for (int i = 0; i < len; i++) {
    crc ^= ((uint8_t *)data)[i];
    for (int j = 0; j < 8; j++) {
      if ((crc & 0x80) != 0)
        crc = (crc << 1) ^ 0x85;
      else
        crc <<= 1;
    }
  }
  return crc;
}

uint8_t crc8_OneWire(const uint8_t *data, size_t size) {
  uint8_t crc = 0x00;
  for (int i = 0; i < size; i++) {
    uint8_t inbyte = ((uint8_t *)data)[i];
    for (int j = 0; j < 8; j++) {
      bool mix = ((crc ^ inbyte) & 1) == 1;
      crc >>= 1;
      if (mix)
        crc ^= 0x8C;
      inbyte >>= 1;
    }
  }
  return crc;
}

uint16_t crc16_Modus(const void *data, size_t size) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < size; i++) {
    crc ^= ((uint8_t *)data)[i];
    for (int j = 0; j < 8; j++) {
      bool mix = (crc & 1) == 1;
      crc >>= 1;
      if (mix)
        crc ^= 0xA001;
    }
  }
  return crc;
}

uint32_t crc32_stm32_word(uint32_t crc, uint32_t data) {
  crc = crc ^ data;
  for (int i = 0; i < 32; i++) {
    if (crc & 0x80000000)
      crc = (crc << 1) ^ 0x04C11DB7; // STM32 CRC-32
    else
      crc = (crc << 1);
  }
  return crc;
}

uint32_t crc32_stm32_aligned(const void *data, size_t size) {
  uint32_t crc = -1;
  if (size >> 2) {
    for (int i = 0; i < size - (size & 3); i += 4) {
      crc = crc32_stm32_word(crc, *(uint32_t *)((uint8_t *)data + i));
    }
  }
  if (size & 3) { // the remainder is filled with zero bytes
    uint32_t value = 0;
    memmove(&value, (uint8_t *)data + size - (size & 3), size & 3);
    crc = crc32_stm32_word(crc, value);
  }
  return crc;
}

#if DEBUG
void debug_print_buffer(const void *data, size_t size, const char *header) {
  const int LineLength = 16;
  const uint8_t *dp = (const uint8_t *)data;
  for (int loffset = 0; loffset < size; loffset += LineLength) {
    if (header)
      debug_printf("%s ", header);
    debug_printf("%04x : ", loffset);
    for (int i = 0; i < LineLength; ++i) {
      int offset = loffset + i;
      if (offset < size) {
        debug_printf("%02x ", dp[offset]);
      } else {
        debug_printf("   ");
      }
    }
    debug_printf(" ");
    for (int i = 0; i < LineLength; ++i) {
      int offset = loffset + i;
      if (offset >= size)
        break;
      uint8_t c = dp[offset];
      if (c < 0x20 || c >= 0x7f)
        c = '.';
      debug_printf("%c", c);
    }
    debug_printf("\n");
  }
}
#endif
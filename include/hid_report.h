#ifndef HID_REPORT_H
#define HID_REPORT_H

#include <stdint.h>

// UPS HID descriptor as hex array
// Generated from hex descriptor data
extern const uint8_t ups_hid_descriptor[];
extern const unsigned int ups_hid_descriptor_len;

void write_uint16_le(uint8_t* buf, uint16_t val);

#endif // HID_REPORT_H

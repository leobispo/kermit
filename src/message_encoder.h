#ifndef MESSAGE_ENCODER_H
#define MESSAGE_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>

#define NEXIST  0 
#define PDENIED 2
#define OSPACE  3

#define CD          0x0
#define ACK         0x1
#define NACK        0x2
#define LS          0x3
#define SHOW        0x4
#define PUT         0x5
#define GET         0x6
#define KEEPALIVE   0x7
#define ATTRIBUTES  0xA
#define DATA        0xD
#define ERROR       0xE
#define END         0xF

void
register_decode_callback(uint8_t cmd, void (*callback)(void *, uint8_t, const uint8_t * const, uint8_t));

void
register_decode_error_callback(void (*error_callback)(void *));

uint8_t
encode_msg(uint8_t *ret, uint8_t size, uint8_t sequence, uint8_t type, const uint8_t * const payload);

bool
decode_msg(void *ctx, const uint8_t * const msg);

ssize_t
decode_msg_size_by_header(uint16_t _header);
#endif

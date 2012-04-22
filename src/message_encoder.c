#include "message_encoder.h"

#include <stdio.h>

#include <string.h>

#define MARK    0x1e
#define INITIAL 0x00
#define POLYNOM 0x07

struct header
{
#ifdef LITTLE_ENDIAN
  uint16_t type     : 4;
  uint16_t sequence : 2;
  uint16_t size     : 4;
  uint16_t mark    : 6;
#else
  uint16_t mark    : 6;
  uint16_t size     : 4;
  uint16_t sequence : 2;
  uint16_t type     : 4;
#endif
};

static uint8_t
calculate_crc(const uint8_t *payload, uint8_t size)
{
  uint8_t remainder = INITIAL;
  uint8_t byte, bit;

  for (byte = 0; byte < size; ++byte) {
    remainder ^= payload[byte];

    for (bit = 8; bit > 0 ; --bit) {
      if (remainder & 0x80)
        remainder = (remainder << 1) ^ POLYNOM;
      else
        remainder <<= 1;
      remainder = remainder & 0xFF;
    }
  }

  return remainder;
}

void (*_cmd_error_callback)(void *ctx) = NULL;

void
invalid_callback(void *ctx, uint8_t sequence, const uint8_t * const payload, uint8_t payload_size)
{
  if (_cmd_error_callback)
    _cmd_error_callback(ctx);
}

static struct {
  void (*callback)(void *, uint8_t, const uint8_t * const, uint8_t);
} _cmd_callbacks[] = {
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback },
  { &invalid_callback }
};

void
register_decode_callback(uint8_t cmd, void (*callback)(void *, uint8_t, const uint8_t * const, uint8_t))
{
  _cmd_callbacks[cmd].callback = callback;
}

void
register_decode_error_callback(void (*error_callback)(void *))
{
  _cmd_error_callback = error_callback; 
}

uint8_t
encode_msg(uint8_t *ret, uint8_t size, uint8_t sequence, uint8_t type, const uint8_t * const payload)
{
  struct header *h = (struct header *) ret;

  uint8_t s = MARK;

  h->mark    = s;
  h->size     = size;
  h->sequence = sequence;
  h->type     = type;

#ifdef LITTLE_ENDIAN
  (*(uint16_t *) ret) = ntohs(*((uint16_t *) ret));
#endif

  memcpy(ret + 2, payload, size);
  ret[2 + size] = calculate_crc(payload, size);

  return size + 3;
}

bool
decode_msg(void *ctx, const uint8_t * const msg)
{
  uint8_t crc;

#ifdef LITTLE_ENDIAN
  (*(uint16_t *) msg) = ntohs(*((uint16_t *) msg));
#endif

  struct header *h = (struct header *) msg;

  if (h->mark != MARK) {
    if (_cmd_error_callback)
      _cmd_error_callback(ctx);
    return false;
  }

  if (h->type == KEEPALIVE)
    return true;

  crc = msg[2 + h->size];

  const uint8_t * payload = msg + 2;

  uint8_t sequence = h->sequence;
  if (calculate_crc(payload, h->size) == crc) {
    _cmd_callbacks[h->type].callback(ctx, sequence, payload, h->size);
    return true;
  }

  if (_cmd_error_callback)
    _cmd_error_callback(ctx);

  return false;
}

ssize_t
decode_msg_size_by_header(uint16_t _header)
{
  _header = ntohs(_header);
  struct header *h = (struct header *) &_header;

  if (h->mark != MARK)
    return -1;

  return 3 + h->size;
}

#ifndef AVIAN_STATE_CAPTURE_H_
#define AVIAN_STATE_CAPTURE_H_

#include <stdint.h>

/* Malloc buffers from CaptureState. A null pointer with a zero length is
   an empty string or a missing name (has_name == 0). body is the versioned
   state graph, including the version byte. The caller frees every non-null
   pointer with free. */
struct StateCapture {
  uint8_t* plugin;
  uint32_t plugin_n;
  uint8_t* type;
  uint32_t type_n;
  uint8_t* name;
  uint32_t name_n;
  int has_name;
  uint8_t* body;
  uint32_t body_n;
};

#endif

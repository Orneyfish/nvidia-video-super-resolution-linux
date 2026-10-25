/* Compile-only check: nvvsr.h is valid C and pulls in no NVIDIA header. */
#include "nvvsr.h"

#ifdef __NVCVIMAGE_H__
#error "nvvsr.h must not include NVIDIA headers"
#endif

int nvvsr_header_c_check(void) {
  nvvsr_config cfg;
  nvvsr_image_in in;
  nvvsr_image_out out;
  nvvsr_timing t;
  nvvsr_config_init(&cfg);
  (void)in;
  (void)out;
  (void)t;
  return (int)cfg.struct_size + NVVSR_API_VERSION;
}

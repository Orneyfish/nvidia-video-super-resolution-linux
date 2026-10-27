/* Is super resolution usable here? Prints the reason when not. Exit 0 = usable. No GPU memory is allocated. */
#include <stdio.h>

#include "nvvsr.h"

int main(void) {
  int rc = nvvsr_probe();
  if (rc == NVVSR_OK) {
    puts("ok");
    return 0;
  }
  printf("%s: %s\n", nvvsr_strerror(rc), nvvsr_last_error());
  return 1;
}

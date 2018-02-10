#ifndef ZHE_ASSERT_H
#define ZHE_ASSERT_H

#include <stdint.h>

#define PANIC(code) do { xrce_panic(__LINE__, (code)); } while (0)
#define PANIC0 PANIC(0)

#ifndef NDEBUG
#define zhe_assert(e) (!(e) ? xrce_panic(__LINE__, 0), (void)0 : (void)0)
#else
#define zhe_assert(e) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif
  void xrce_panic(uint16_t line, uint16_t code);
#ifdef __cplusplus
}
#endif

#endif

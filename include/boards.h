/**
 * @file boards.h
 * @brief Minimal LED stubs so role_*.c compile without Nordic BSP.
 */
#ifndef BOARDS_H_
#define BOARDS_H_

#define BSP_LED_0_MASK  1u
#define BSP_LED_1_MASK  2u
#define BSP_LED_2_MASK  4u
#define BSP_LED_3_MASK  8u

#ifndef LEDS_INVERT
#define LEDS_INVERT(mask)  ((void)(mask))
#endif
#ifndef LEDS_ON
#define LEDS_ON(mask)      ((void)(mask))
#endif
#ifndef LEDS_OFF
#define LEDS_OFF(mask)     ((void)(mask))
#endif
#ifndef LEDS_CONFIGURE
#define LEDS_CONFIGURE(mask) ((void)(mask))
#endif

#endif /* BOARDS_H_ */

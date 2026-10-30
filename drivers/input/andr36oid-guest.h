/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ANDR36OID_GUEST_INPUT_H
#define _ANDR36OID_GUEST_INPUT_H
#include <linux/pid_namespace.h>
#include <linux/input.h>

/* Map the Android GO pad wiring to the unmodified dArkOSen rg351 ABI.
 * Applied to a reader, never to the shared input_dev or its physical driver.
 */
static inline bool a36_guest_pad(struct input_dev *dev)
{
	return READ_ONCE(task_active_pid_ns(current)->andr36oid_guest) &&
		dev->id.vendor == 0x484b && dev->id.product == 0x1100 &&
		test_bit(KEY_UP, dev->keybit) && test_bit(KEY_HOMEPAGE, dev->keybit);
}
static inline unsigned int a36_guest_key(unsigned int code)
{
	/* Keep optional Android extra buttons distinct from the five native keys. */
	if (code >= BTN_TRIGGER_HAPPY1 && code <= BTN_TRIGGER_HAPPY16)
		return code + 5;
	switch (code) {
	case KEY_UP: return BTN_DPAD_UP;
	case KEY_DOWN: return BTN_DPAD_DOWN;
	case KEY_LEFT: return BTN_DPAD_LEFT;
	case KEY_RIGHT: return BTN_DPAD_RIGHT;
	case BTN_SOUTH: return BTN_EAST;
	case BTN_EAST: return BTN_SOUTH;
	case BTN_SELECT: return BTN_TRIGGER_HAPPY1;
	case BTN_START: return BTN_TRIGGER_HAPPY2;
	case KEY_BACK: return BTN_TRIGGER_HAPPY3;
	case KEY_LEFTMETA: return BTN_TRIGGER_HAPPY4;
	case KEY_HOMEPAGE: return BTN_TRIGGER_HAPPY5;
	default: return code;
	}
}
static inline void a36_guest_bits(unsigned long *dst, const unsigned long *src)
{
	unsigned int key;
	bitmap_zero(dst, KEY_CNT);
	for_each_set_bit(key, src, KEY_CNT)
		__set_bit(a36_guest_key(key), dst);
}
#endif

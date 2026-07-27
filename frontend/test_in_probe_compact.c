/* EROSQ host test for in_probe()'s device-table cleanup in libpicofe/input.c.
 *
 * Why it exists: in_probe() used to be reached only at startup and from the
 * menu's "Rescan devices". It is now called from the frame loop every time a
 * BT pad connects or disconnects, so its cleanup pass runs constantly and its
 * bugs became reachable:
 *
 *   - the pass compacted the table with memmove() but left the vacated tail
 *     slot holding a byte-copy of a live device (probed = 1, same drv_data).
 *     The next re-probe frees that drv_data through the live copy, and the
 *     stale copy is left pointing at freed memory -- which in_adev[] can still
 *     name, because get_dev() bounds-checks against IN_MAX_DEVS, not
 *     in_dev_count. That is a use-after-free on every frame.
 *   - the pass "continue"d after a removal, so the entry that the memmove()
 *     had just moved *into* the current slot was never examined and survived.
 *
 * Pure logic, no deps:
 *   gcc -O2 -Wall -fsanitize=address -o /tmp/test_in_probe_compact \
 *       frontend/test_in_probe_compact.c -Iinclude -Ifrontend && \
 *       /tmp/test_in_probe_compact
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- stubs for the libpicofe symbols input.c references ------------------ */
void lprintf(const char *fmt, ...) { (void)fmt; }
unsigned int plat_get_ticks_ms(void) { return 0; }
void plat_sleep_ms(int ms) { (void)ms; }
int plat_wait_event(int *fds_hnds, int count, int timeout_ms)
{
	(void)fds_hnds; (void)count; (void)timeout_ms;
	return -1;
}

#include "libpicofe/input.c"

#define check(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: " #cond " ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); exit(1); \
	} \
} while (0)

/* ---- a driver that registers whatever the test tells it to ---------------
 * Mirrors what in_evdev does: one in_register() per device it finds, and a
 * clean_binds() that returns 0 for a device whose keys match no bind -- which
 * is how a real device ends up with binds == NULL. A BT pad's second evdev
 * node (consumer-control/media keys) is exactly such a device. */
#define MAX_FAKE_DEVS 6
#define FAKE_PREFIX "fake:"

static const char *probe_names[MAX_FAKE_DEVS];
static int probe_count;
static unsigned int nobind_mask;   /* bit i: probe_names[i] has no usable binds */
static int live_drv_data;          /* allocated - freed, must end at 0 */

static void fake_free(void *drv_data)
{
	check(drv_data != NULL, "free(NULL)");
	live_drv_data--;
	free(drv_data);
}

static int fake_clean_binds(void *drv_data, int *binds, int *def_binds)
{
	int idx = *(int *)drv_data;
	(void)binds; (void)def_binds;
	return (nobind_mask & (1u << idx)) ? 0 : 1;
}

static void fake_probe(const in_drv_t *drv)
{
	int i;

	for (i = 0; i < probe_count; i++) {
		int *tag = malloc(sizeof(*tag));
		char name[64];

		check(tag != NULL, "oom");
		*tag = i;
		live_drv_data++;
		/* the prefix is the driver's job, as in in_evdev_probe() */
		snprintf(name, sizeof name, "%s%s", FAKE_PREFIX, probe_names[i]);
		/* drv_fd_hnd 1: a normal polled fd, not an async device */
		in_register(name, 1, tag, 4, NULL, 0);
	}
	(void)drv;
}

static const in_drv_t fake_drv = {
	.prefix      = FAKE_PREFIX,
	.probe       = fake_probe,
	.free        = fake_free,
	.clean_binds = fake_clean_binds,
};

static void connect(const char *n0, const char *n1, const char *n2,
		unsigned int no_binds)
{
	probe_count = 0;
	if (n0) probe_names[probe_count++] = n0;
	if (n1) probe_names[probe_count++] = n1;
	if (n2) probe_names[probe_count++] = n2;
	nobind_mask = no_binds;
	in_probe();
}

static void disconnect_all(void)
{
	probe_count = 0;
	nobind_mask = 0;
	in_probe();
}

static int find_dev(const char *name)
{
	int i;

	for (i = 0; i < IN_MAX_DEVS; i++)
		if (in_devices[i].name != NULL && strcmp(in_devices[i].name, name) == 0)
			return i;
	return -1;
}

static void check_slot_clear(int i)
{
	check(in_devices[i].name == NULL, "slot %d: stale name \"%s\"",
		i, in_devices[i].name);
	check(in_devices[i].drv_data == NULL, "slot %d: stale drv_data %p",
		i, in_devices[i].drv_data);
	check(in_devices[i].binds == NULL, "slot %d: stale binds", i);
	check(!in_devices[i].probed, "slot %d: still marked probed", i);
}

/* ---- 1. a compacted-away slot must not keep a copy of a live device -------
 * "decoy" registers first and has no usable binds, so the re-probe after a
 * disconnect drops it and shifts "pad" down into its slot. The vacated tail
 * slot is what used to keep the duplicate. */
static void test_no_ghost_after_compaction(void)
{
	int pad;

	connect("decoy", "pad", NULL, 1u << 0);
	check(in_dev_count == 2, "both devices registered, got %d", in_dev_count);
	check(find_dev(FAKE_PREFIX "pad") >= 0, "pad missing after first probe");

	disconnect_all();
	check(in_dev_count == 1, "decoy should be gone, count %d", in_dev_count);
	pad = find_dev(FAKE_PREFIX "pad");
	check(pad == 0, "pad should have moved to slot 0, at %d", pad);
	check_slot_clear(1);
	printf("  compaction leaves no duplicate in the vacated slot      OK\n");
}

/* ---- 2. that duplicate is a use-after-free, not just untidy ---------------
 * Second disconnect cycle: in_unprobe() walks only up to in_dev_count and frees
 * the live copy's drv_data. Anything the stale copy still points at is freed
 * memory. Reading it here is what ASan traps; the accounting check below
 * catches it in a plain build too (the ghost's drv_data was never freed). */
static void test_no_double_reference_after_two_cycles(void)
{
	int i;

	connect("decoy", "pad", NULL, 1u << 0);
	disconnect_all();
	connect("decoy", "pad", NULL, 1u << 0);
	disconnect_all();

	for (i = 0; i < IN_MAX_DEVS; i++) {
		if (i < in_dev_count)
			continue;
		check_slot_clear(i);
	}
	printf("  no stale drv_data survives two connect/disconnect cycles OK\n");
}

/* ---- 3. two removals in a row must both take effect -----------------------
 * The old pass advanced the loop index after a removal, so whatever the
 * memmove() slid into the current slot was skipped. */
static void test_adjacent_removals(void)
{
	connect("d1", "d2", "pad", (1u << 0) | (1u << 1));
	check(in_dev_count == 3, "three devices, got %d", in_dev_count);

	disconnect_all();
	check(find_dev(FAKE_PREFIX "d1") < 0, "d1 survived the cleanup");
	check(find_dev(FAKE_PREFIX "d2") < 0, "d2 survived the cleanup (skipped by continue)");
	check(find_dev(FAKE_PREFIX "pad") == 0, "pad should be the only device left");
	check(in_dev_count == 1, "only pad should remain, count %d", in_dev_count);
	printf("  two adjacent removals both take effect                  OK\n");
}

/* ---- 4. a device with binds is kept across a disconnect ------------------
 * The point of keeping it is that a reconnect reattaches the same binds by
 * name, so this is the behaviour the cleanup must not break. */
static void test_binds_survive_reconnect(void)
{
	int *binds_before;

	connect("pad", NULL, NULL, 0);
	binds_before = in_devices[find_dev(FAKE_PREFIX "pad")].binds;
	check(binds_before != NULL, "pad should have binds");

	disconnect_all();
	check(find_dev(FAKE_PREFIX "pad") == 0, "pad kept for its binds while away");
	check(!in_devices[0].probed, "pad must be marked unprobed while away");
	check(in_devices[0].drv_data == NULL, "unprobe must drop drv_data");

	connect("pad", NULL, NULL, 0);
	check(find_dev(FAKE_PREFIX "pad") == 0, "pad back in the same slot");
	check(in_devices[0].probed, "pad probed again");
	check(in_devices[0].binds == binds_before, "binds reattached, not realloced");
	printf("  a bound device keeps its slot and binds across a drop   OK\n");
}

int main(void)
{
	printf("in_probe device-table cleanup:\n");
	in_register_driver(&fake_drv, NULL, NULL, NULL);

	test_no_ghost_after_compaction();
	test_no_double_reference_after_two_cycles();
	test_adjacent_removals();
	test_binds_survive_reconnect();

	disconnect_all();
	check(live_drv_data == 0, "%d drv_data allocations leaked", live_drv_data);
	printf("all OK\n");
	return 0;
}

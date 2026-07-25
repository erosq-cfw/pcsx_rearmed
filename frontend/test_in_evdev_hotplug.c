/* EROSQ host test for the in_evdev hotplug detector (scan_node_mask() and
 * in_evdev_hotplug_check() in libpicofe/linux/in_evdev.c).
 *
 * Why it exists: a BT pad's /dev/input node vanishes on disconnect and a NEW
 * one appears on reconnect, so the emulator has to notice and re-probe. The
 * subtle part is that a reconnect usually reuses the SAME node number, so the
 * node-set snapshot alone sees no change -- the ENODEV "device lost" trigger is
 * what makes that case work. That interlock is what test_reconnect_same_node()
 * pins down; it is the exact bug seen on hardware.
 *
 * Pure logic, no deps:
 *   gcc -O2 -o /tmp/test_in_evdev_hotplug frontend/test_in_evdev_hotplug.c \
 *       -Iinclude -Ifrontend && /tmp/test_in_evdev_hotplug
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Point the production code at a directory we can fabricate. In the real build
 * EVDEV_DIR is the "/dev/input" string literal; here it is a variable, which is
 * fine because it is only ever passed as a const char *. */
static const char *test_dir;
#define EVDEV_DIR test_dir

/* ---- stubs for the two libpicofe symbols in_evdev.c references ------------ */
#include "libpicofe/input.h"     /* for the types the stubs have to match */

static unsigned int fake_ms;
unsigned int plat_get_ticks_ms(void) { return fake_ms; }
void in_register(const char *nname, int drv_fd_hnd, void *drv_data,
	int key_count, const char * const *key_names, int combos)
{
	(void)nname; (void)drv_fd_hnd; (void)drv_data;
	(void)key_count; (void)key_names; (void)combos;
}
int in_register_driver(const in_drv_t *drv, const struct in_default_bind *defbinds,
	const struct in_default_bind *kbd_binds, const void *pdata)
{
	(void)drv; (void)defbinds; (void)kbd_binds; (void)pdata;
	return 0;
}

#include "libpicofe/linux/in_evdev.c"

#define check(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: " #cond " ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); printf("\n"); exit(1); \
	} \
} while (0)

/* ---- fixture: a fake /dev/input ------------------------------------------ */
static char dir_buf[64];

static void fs_init(void)
{
	snprintf(dir_buf, sizeof dir_buf, "/tmp/evdevtest.%d", (int)getpid());
	test_dir = dir_buf;
	mkdir(dir_buf, 0700);
}
static void node_add(const char *name)
{
	char p[128];
	snprintf(p, sizeof p, "%s/%s", dir_buf, name);
	close(open(p, O_CREAT | O_WRONLY, 0600));
}
static void node_del(const char *name)
{
	char p[128];
	snprintf(p, sizeof p, "%s/%s", dir_buf, name);
	unlink(p);
}

/* The rate limiter is a function-static, so tests step a fake clock instead.
 * It starts where the real one does: plat_get_ticks_ms() is gettimeofday-based,
 * so it is epoch-ms truncated to 32 bits and its top bit is SET at the time of
 * writing (2554470160 on the device). A zero-based fake clock hides sign-bit
 * bugs in the rate limiter -- the first version of this code shipped with one
 * and passed a zero-based version of these very tests. */
#define CLOCK_START 2554470160u
static void clock_advance(void) { fake_ms += 1001; }

/* ---- 1. node-set parsing ------------------------------------------------- */
static void test_scan_mask(void)
{
	unsigned int m;

	node_add("event0"); node_add("event3"); node_add("event12");
	node_add("mouse0"); node_add("mice"); node_add("js0");
	node_add("event");        /* no number       -> ignore */
	node_add("eventX");       /* not a number    -> ignore */
	node_add("event2x");      /* trailing junk   -> ignore */
	m = scan_node_mask(test_dir);
	check(m == ((1u << 0) | (1u << 3) | (1u << 12)), "got 0x%x", m);

	node_del("event3");
	m = scan_node_mask(test_dir);
	check(m == ((1u << 0) | (1u << 12)), "got 0x%x after removal", m);

	check(scan_node_mask("/tmp/definitely-not-here") == 0, "missing dir");
	printf("  node-set parse: eventN only, absent dir -> 0            OK\n");

	node_del("event0"); node_del("event12"); node_del("mouse0");
	node_del("mice"); node_del("js0"); node_del("event");
	node_del("eventX"); node_del("event2x");
}

/* ---- 2. change detection + rate limit ------------------------------------ */
static void test_change_detect(void)
{
	node_add("event0"); node_add("event1");
	evdev_node_mask = scan_node_mask(test_dir);   /* as in_evdev_probe() does */
	evdev_dev_lost = 0;

	clock_advance();
	check(in_evdev_hotplug_check() == 0, "quiet when nothing changed");

	node_add("event4");                            /* pad connects */
	check(in_evdev_hotplug_check() == 0, "rate limit holds within 1s");
	clock_advance();
	check(in_evdev_hotplug_check() == 1, "connect detected");

	evdev_node_mask = scan_node_mask(test_dir);    /* frontend re-probed */
	clock_advance();
	check(in_evdev_hotplug_check() == 0, "quiet again after re-probe");

	node_del("event4");                            /* pad disconnects */
	clock_advance();
	check(in_evdev_hotplug_check() == 1, "disconnect detected");
	printf("  connect + disconnect detected, rate-limited to 1/s      OK\n");
}

/* ---- 3. the ENODEV trigger ---------------------------------------------- */
static void test_dev_gone(void)
{
	evdev_dev_lost = 0;

	errno = EAGAIN;
	check(dev_gone() == 0 && !evdev_dev_lost, "EAGAIN is not a dead device");
	errno = EINTR;
	check(dev_gone() == 0 && !evdev_dev_lost, "EINTR is not a dead device");

	errno = ENODEV;
	check(dev_gone() == 1 && evdev_dev_lost, "ENODEV means gone");
	/* and it must report immediately, without waiting for the scan window */
	check(in_evdev_hotplug_check() == 1, "loss bypasses the rate limit");
	printf("  ENODEV = device lost, reported without rate limit       OK\n");
}

/* ---- 4. THE BUG: reconnect reusing the same node number ------------------ */
static void test_reconnect_same_node(void)
{
	node_add("event4");                            /* pad connected */
	evdev_node_mask = scan_node_mask(test_dir);
	evdev_dev_lost = 0;
	clock_advance();
	check(in_evdev_hotplug_check() == 0, "steady state");

	/* BT drops: the node goes away and the stale fd starts failing */
	node_del("event4");
	errno = ENODEV;
	dev_gone();
	check(in_evdev_hotplug_check() == 1, "loss reported");

	/* frontend re-probes, which re-snapshots WITHOUT event4 -- this is what
	 * makes the same-numbered reconnect visible below */
	evdev_node_mask = scan_node_mask(test_dir);
	evdev_dev_lost = 0;
	clock_advance();
	check(in_evdev_hotplug_check() == 0, "quiet while disconnected");

	/* BT comes back and the kernel hands out the SAME minor as before */
	node_add("event4");
	clock_advance();
	check(in_evdev_hotplug_check() == 1,
		"reconnect on the same node number must still be detected");
	printf("  reconnect reusing event4 detected (the reported bug)    OK\n");

	node_del("event4");
}

/* ---- 5. the rate limiter must work at any point on the 32-bit clock ------- */
static void test_clock_epochs(void)
{
	static const unsigned int starts[] = {
		0u,           /* fresh boot-style clock          */
		CLOCK_START,  /* real device value, top bit set  */
		0x7ffffe00u,  /* just below the sign flip        */
		0xfffffe00u,  /* about to wrap through zero      */
	};
	unsigned int i;

	for (i = 0; i < sizeof starts / sizeof *starts; i++) {
		fake_ms = starts[i];
		node_add("event7");
		evdev_node_mask = scan_node_mask(test_dir);
		evdev_dev_lost = 0;
		clock_advance();
		check(in_evdev_hotplug_check() == 0, "start 0x%x: steady", starts[i]);

		node_del("event7");
		clock_advance();
		check(in_evdev_hotplug_check() == 1,
			"start 0x%x: change must be seen (sign-bit/wrap bug)", starts[i]);
	}
	printf("  detection works at 0, sign-bit-set, and across the wrap  OK\n");
}

int main(void)
{
	printf("in_evdev hotplug detection:\n");
	fake_ms = CLOCK_START;
	fs_init();
	test_scan_mask();
	test_change_detect();
	test_dev_gone();
	test_reconnect_same_node();
	test_clock_epochs();
	node_del("event0"); node_del("event1");
	rmdir(dir_buf);
	printf("all OK\n");
	return 0;
}

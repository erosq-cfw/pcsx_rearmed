/* EROSQ test fixture: a uinput device that pcsx keeps NO binds for.
 *
 * Companion to fakepad. fakepad impersonates the 8BitDo, so libpicofe finds
 * default binds for it and in_evdev_clean_binds() returns nonzero. This one
 * advertises a single key that no pcsx bind uses (KEY_SOUND), so clean_binds()
 * returns 0 and in_register() sets the device's binds to NULL -- which is the
 * precondition for in_probe()'s cleanup pass to actually remove an entry and
 * compact the device table. A real BT pad's second evdev node (the
 * consumer-control/media-key one) is such a device.
 *
 * Bring it up BEFORE the pad so it lands on a lower event node and is therefore
 * NOT the last entry in the table: only a non-last removal does a memmove, and
 * only a memmove leaves the stale duplicate behind.
 *
 * usage: nobind ["device name"]   -- runs until killed; killing it = unplug
 * build: mipsel-linux-gcc -O2 -o nobind nobind.c
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "nobind probe";
	struct uinput_user_dev ud;
	int fd;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("nobind: open /dev/uinput");
		return 1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0
	    || ioctl(fd, UI_SET_KEYBIT, KEY_SOUND) < 0) {
		perror("nobind: UI_SET_*BIT");
		return 1;
	}

	memset(&ud, 0, sizeof ud);
	snprintf(ud.name, sizeof ud.name, "%s", name);
	ud.id.bustype = BUS_VIRTUAL;
	ud.id.vendor  = 0x2dc8;
	ud.id.product = 0x9000;
	ud.id.version = 1;

	if (write(fd, &ud, sizeof ud) != sizeof ud) {
		perror("nobind: write uinput_user_dev");
		return 1;
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("nobind: UI_DEV_CREATE");
		return 1;
	}

	printf("nobind: up as \"%s\" (pid %d)\n", name, (int)getpid());
	fflush(stdout);

	for (;;)
		pause();
	return 0;
}

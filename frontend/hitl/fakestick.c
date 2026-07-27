/* EROSQ test fixture: a gamepad with a REAL analog stick.
 *
 * fakepad only drives digital keys, so it cannot tell the analog path from the
 * stick-as-d-pad path -- which is exactly the distinction the 2026-07-27 analog
 * work turns on. This one exposes the same shape as the 8BitDo: face/shoulder
 * buttons, a d-pad as ABS_HAT0X/Y, and a left stick as ABS_X/Y.
 *
 * Commands on stdin, one per line:
 *   x <v> / y <v>   left stick absolute value, -32768..32767 (0 = centred)
 *   hat l|r|u|d|c   d-pad via ABS_HAT0X/Y (c = centre)
 *   mode            BTN_MODE tap (pcsx: open/close the emulator menu)
 *   a b sel start   button taps
 *   quit            exit (or just kill the process = unplug)
 *
 * build: mipsel-linux-gcc -O2 -o fakestick fakestick.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

static int fd;

static void emit(int type, int code, int val)
{
	struct input_event ev;

	memset(&ev, 0, sizeof ev);
	ev.type = type;
	ev.code = code;
	ev.value = val;
	if (write(fd, &ev, sizeof ev) != sizeof ev)
		perror("fakestick: write");
}

static void sync_now(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void tap(int code)
{
	emit(EV_KEY, code, 1);
	sync_now();
	usleep(120000);
	emit(EV_KEY, code, 0);
	sync_now();
}

int main(int argc, char **argv)
{
	static const int keys[] = {
		BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR,
		BTN_SELECT, BTN_START, BTN_MODE,
	};
	const char *name = argc > 1 ? argv[1] : "8BitDo Ultimate 2 Wireless";
	struct uinput_user_dev ud;
	char line[64];
	unsigned i;

	fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("fakestick: open /dev/uinput");
		return 1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	for (i = 0; i < sizeof keys / sizeof *keys; i++)
		ioctl(fd, UI_SET_KEYBIT, keys[i]);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);

	memset(&ud, 0, sizeof ud);
	snprintf(ud.name, sizeof ud.name, "%s", name);
	ud.id.bustype = BUS_BLUETOOTH;
	ud.id.vendor  = 0x2dc8;
	ud.id.product = 0x3106;
	ud.id.version = 1;
	/* a real stick's range; the hat is the usual -1..1 */
	ud.absmin[ABS_X] = -32768; ud.absmax[ABS_X] = 32767;
	ud.absmin[ABS_Y] = -32768; ud.absmax[ABS_Y] = 32767;
	ud.absmin[ABS_HAT0X] = -1; ud.absmax[ABS_HAT0X] = 1;
	ud.absmin[ABS_HAT0Y] = -1; ud.absmax[ABS_HAT0Y] = 1;

	if (write(fd, &ud, sizeof ud) != sizeof ud) {
		perror("fakestick: write uinput_user_dev");
		return 1;
	}
	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("fakestick: UI_DEV_CREATE");
		return 1;
	}

	/* centre everything so the first read is neutral, not 0-from-nowhere */
	emit(EV_ABS, ABS_X, 0);
	emit(EV_ABS, ABS_Y, 0);
	emit(EV_ABS, ABS_HAT0X, 0);
	emit(EV_ABS, ABS_HAT0Y, 0);
	sync_now();

	printf("fakestick: up as \"%s\" (pid %d)\n", name, (int)getpid());
	fflush(stdout);

	while (fgets(line, sizeof line, stdin) != NULL) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;

		if (strncmp(line, "x ", 2) == 0) {
			emit(EV_ABS, ABS_X, atoi(line + 2)); sync_now();
		} else if (strncmp(line, "y ", 2) == 0) {
			emit(EV_ABS, ABS_Y, atoi(line + 2)); sync_now();
		} else if (strncmp(line, "hat ", 4) == 0) {
			int hx = 0, hy = 0;
			switch (line[4]) {
			case 'l': hx = -1; break;
			case 'r': hx =  1; break;
			case 'u': hy = -1; break;
			case 'd': hy =  1; break;
			default: break;              /* 'c' = centre */
			}
			emit(EV_ABS, ABS_HAT0X, hx);
			emit(EV_ABS, ABS_HAT0Y, hy);
			sync_now();
		}
		else if (!strcmp(line, "mode"))  tap(BTN_MODE);
		else if (!strcmp(line, "a"))     tap(BTN_A);
		else if (!strcmp(line, "b"))     tap(BTN_B);
		else if (!strcmp(line, "sel"))   tap(BTN_SELECT);
		else if (!strcmp(line, "start")) tap(BTN_START);
		else if (!strcmp(line, "quit"))  break;

		fflush(stdout);
	}

	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}

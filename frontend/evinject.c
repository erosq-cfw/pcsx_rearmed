/* Tiny evdev injector for automated on-device testing (throwaway, not shipped).
 * Writes raw input_event structs to /dev/input/eventN. Struct is hand-defined to
 * the 32-bit kernel ABI (8-byte timeval) so it works regardless of musl's 64-bit
 * time_t. Injected keycodes must be in the target device's capability bitmap
 * (e.g. gpio-keys=event3 declares KEY_MENU/KEY_BACK/KEY_VOLUME*).
 *
 * usage: evinject /dev/input/eventN <tok>...
 *   kd:<code>  key down (no syn)   ku:<code> key up (no syn)
 *   syn        SYN_REPORT           ms:<n>    sleep n ms
 * e.g. chord Menu+Back: evinject /dev/input/event3 kd:139 kd:158 syn ms:600 ku:139 ku:158 syn
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

struct kev { int32_t s, us; uint16_t type, code; int32_t value; };
static int fd;
static void emit(uint16_t t, uint16_t c, int32_t v) {
	struct kev e = { 0, 0, t, c, v };
	if (write(fd, &e, sizeof e) != (long)sizeof e) perror("write");
}
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: %s DEV tok...\n", argv[0]); return 2; }
	fd = open(argv[1], O_WRONLY);
	if (fd < 0) { perror("open"); return 1; }
	for (int i = 2; i < argc; i++) {
		if (!strncmp(argv[i], "kd:", 3))      emit(1, atoi(argv[i]+3), 1);
		else if (!strncmp(argv[i], "ku:", 3)) emit(1, atoi(argv[i]+3), 0);
		else if (!strcmp(argv[i], "syn"))     emit(0, 0, 0);
		else if (!strncmp(argv[i], "ms:", 3)) usleep(atoi(argv[i]+3) * 1000);
	}
	close(fd);
	return 0;
}

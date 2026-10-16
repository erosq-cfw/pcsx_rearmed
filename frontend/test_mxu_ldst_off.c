/* EROSQ on-device selftest for the immediate-offset MXU S32LDDO/S32STDO
 * encodings (mxu_x1000.h). Run this on the X1000 BEFORE trusting a binary
 * that uses them; it must print "...OK" and exit 0.
 *
 *   mipsel-buildroot-linux-musl-gcc -O2 -static -o test_mxu_ldst_off \
 *     frontend/test_mxu_ldst_off.c
 *   (also runs under: qemu-mipsel-static -cpu XBurstR2 ./test_mxu_ldst_off)
 */
#include <stdio.h>
#include <stdint.h>
#include "../plugins/gpu_unai/mxu_x1000.h"
static uint32_t buf[1200], out[1200];
int main(void){
	int bad=0;
	for (int i=0;i<1200;i++) buf[i]=0x1000+i*7+(i<<20), out[i]=0;
	uint32_t *mid = buf+600, *omid = out+600;
	MXU_S32I2M(MXU_XR16, 1);
#define T(off) do { MXU_S32I2M(MXU_XR3, 0); MXU_S32LDDO(MXU_XR3, mid, off); uint32_t v = MXU_S32M2I(MXU_XR3); \
	if (v != mid[(off)/4]) { printf("LDD off %d: got %x want %x\n", off, v, mid[(off)/4]); bad++; } \
	MXU_S32STDO(MXU_XR3, omid, off); if (omid[(off)/4] != v) { printf("STD off %d bad\n", off); bad++; } } while(0)
	T(0); T(4); T(8); T(12); T(16); T(20); T(24); T(28); T(32); T(64); T(-4); T(-8); T(1020); T(2044); T(-2048); T(100);
	/* neighbours untouched by STD */
	for (int i=0;i<1200;i++) { int o=i-600; int w = (o==0||o==1||o==2||o==3||o==4||o==5||o==6||o==7||o==8||o==16||o==-1||o==-2||o==255||o==511||o==-512||o==25); if(!w && out[i]) { printf("stray store at %d\n", o); bad++; } }
	printf("%s\n", bad ? "FAIL" : "S32LDDO/S32STDO offset encoding OK");
	return bad;
}

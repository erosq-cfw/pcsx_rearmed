#include <stdarg.h>
#include <stdio.h>
unsigned int in_keystate[8];
char ndrc_g[65536];
unsigned char PAD1_readPort(unsigned char v){return 0;}
unsigned char PAD2_readPort(unsigned char v){return 0;}
void plat_trigger_vibrate(int a,int b,int c){}
void pl_frame_limit(void){}
int pl_gun_byte2(int p, unsigned char b){return 0;}
void scond_signal(void*c){} void slock_lock(void*l){} void slock_unlock(void*l){}
void SysMessage(const char*f,...){}
void SysPrintf(const char*f,...){va_list a;va_start(a,f);vfprintf(stderr,f,a);va_end(a);}
void pl_dynarec_notify(void){}

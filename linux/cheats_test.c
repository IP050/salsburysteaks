/* runtime/cheats.c on its own, against fake guest memory: every opcode family the cheat files use and the FOV hook.
   Run: bash linux/cheats_test.sh (prints FAIL lines and exits 1 on a mismatch) */
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "rt.h"
u8* g_mem; const SwrModule* g_mod; char g_datadir[1024], g_exename[256], g_exedir[1024]; int g_verbose=1;
void logf_(int l, const char* f, ...){ (void)l; va_list a; va_start(a,f); vprintf(f,a); va_end(a); }
void pm_init(PMutex* m){ (void)m; } void pm_lock(PMutex* m){ (void)m; } void pm_unlock(PMutex* m){ (void)m; }
int plat_mkdir(const char* p){ return mkdir(p,0755); }
int plat_thread(PThreadFn fn, void* arg, size_t stack){ (void)fn; (void)arg; (void)stack; return 0; }   /* the region 4 search, not used here */
void plat_thread_low_priority(void){}
void plat_sleep_ns(i64 ns){ (void)ns; }
void mem_query(u64 a, MemBlock* b){ memset(b,0,sizeof *b);
  if(a>=0x8000000 && a<0x9000000){ b->addr=0x8000000; b->size=0x1000000; b->perm=PERM_RW; }
  else if(a>=HEAP_BASE && a<HEAP_BASE+0x100000){ b->addr=HEAP_BASE; b->size=0x100000; b->perm=PERM_RW; } }
u64 g_pad_buttons_now;
void cheats_tick(void); int cheats_count(void); int cheats_info(int,char*,int,int*,int*); void cheats_set(int,int); extern int g_opt_fov;
int main(void){
  g_mem=mmap(NULL,AS_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
  static SwrModInfo mi={0}; mi.name="main"; mi.base=0x8000000; for(int i=0;i<8;i++) mi.build_id[i]=0x82+i;
  static SwrModule m={0}; m.nmods=1; m.mods=&mi; g_mod=&m;
  snprintf(g_datadir,sizeof g_datadir,"game_data/");
  FILE* f=fopen("game_data/cheats/8283848586878889.txt","w");
  fprintf(f,"{Master}\n04000000 00001000 00000063\n\n"            /* main+0x1000 = 99 (u32), always */
            "[Infinite missiles]\n04100000 00000100 0000012C\n\n"   /* heap+0x100 = 300 */
            "[Pointer chain]\n580F0000 00002000\n780F0000 00000010\n680F0000 00000000 00000777\n\n"   /* [[main+0x2000]+0x10] = 0x777 */
            "[Hold ZL]\n80000100\n04000000 00003000 00000001\n20000000\n\n"   /* only while ZL held */
            "[If/else]\n14050000 00001000 00000063\n04000000 00003100 000000AA\n21000000\n04000000 00003100 000000BB\n20000000\n\n"
            "[Loop]\n40000000 00000001 00000600\n40010000 00000000 00000000\n30020000 00000003\n94011100 00000002\nA4101000\n31020000\n"); /* write r1 to [r1] style: tests 9 and A */
  fclose(f);
  gw64(0x8000000+0x2000,HEAP_BASE+0x400);   /* the pointer */
  cheats_tick();
  int n=cheats_count(); printf("count=%d\n",n);
  for(int i=0;i<n;i++){ char nm[64]; int on,ms; cheats_info(i,nm,64,&on,&ms); printf("  %s on=%d master=%d\n",nm,on,ms); if(!ms) cheats_set(i,1); }
  g_pad_buttons_now=0; cheats_tick();
  printf("master  main+1000=%u (want 99)\n",gr32(0x8001000));
  printf("heap+100=%u (want 300)\n",gr32(HEAP_BASE+0x100));
  printf("chain   heap+410=%#llx (want 0x777)\n",(unsigned long long)gr64(HEAP_BASE+0x410));
  printf("ZL off  main+3000=%u (want 0)\n",gr32(0x8003000));
  printf("if/else main+3100=%#x (want 0xaa)\n",gr32(0x8003100));
  g_pad_buttons_now=0x100; cheats_tick(); printf("ZL on   main+3000=%u (want 1)\n",gr32(0x8003000));
  printf("loop    heap+600..608=%u %u %u (want 2 4 6)\n",gr32(HEAP_BASE+0x600),gr32(HEAP_BASE+0x604),gr32(HEAP_BASE+0x608));
  gw32(0x8001000,5); cheats_tick();  /* master writes 99 back before the if/else runs */
  FILE* e=fopen("game_data/cheats/8283848586878889.enabled","r"); char ln[200]; printf("enabled file:\n"); while(e && fgets(ln,200,e)) printf("  %s",ln); if(e) fclose(e);
  /* FOV hook: float at heap+0x800 via a pointer at main+0x5000 */
  f=fopen("game_data/cheats/8283848586878889.fov","w"); fprintf(f,"8283848586878889 main+5000 8\n"); fclose(f);
  gw64(0x8005000,HEAP_BASE+0x7F8); float fv=60.0f; u32 r; memcpy(&r,&fv,4); gw32(HEAP_BASE+0x800,r);
  g_opt_fov=125; extern void cheats_reload(void); cheats_reload(); cheats_tick(); r=gr32(HEAP_BASE+0x800); memcpy(&fv,&r,4); printf("fov 125%%: %.2f (want 75.00)\n",fv);
  cheats_tick(); r=gr32(HEAP_BASE+0x800); memcpy(&fv,&r,4); printf("fov again: %.2f (want 75.00, not compounded)\n",fv);
  fv=90.0f; memcpy(&r,&fv,4); gw32(HEAP_BASE+0x800,r); cheats_tick(); r=gr32(HEAP_BASE+0x800); memcpy(&fv,&r,4); printf("game sets 90: %.2f (want 112.50)\n",fv);
  return 0; }

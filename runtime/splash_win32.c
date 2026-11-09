/* Splash screen between the Play button and the game's first frame: the game's logo on black, over the window the game
   will use. Its own layered window on its own thread: it never touches the game's window, whose presents are particular
   (see win_thread), and it keeps fading while the game loads. Ends at the first swap (splash_first_swap), or after a
   minute whatever happens. ini splash=0: none */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include "rt.h"

int g_opt_splash=1;
static volatile LONG g_stop;
static const GUID S_CLSID_Factory={0xcacaf262,0x9370,0x4615,{0xa1,0x3b,0x9f,0x55,0x39,0xda,0x4c,0x0a}};
static const GUID S_IID_Factory={0xec5ec8a9,0xc395,0x4314,{0x9c,0x77,0x54,0xd7,0xa9,0x35,0xff,0x70}};
static const GUID S_PBGRA={0x6fddc324,0x4e03,0x4bfe,{0xb1,0x85,0x3d,0x77,0x76,0x8d,0xc9,0x10}};

typedef struct { RECT r; } SplashArg;

/* the embedded PNG (app.rc, RCDATA 3), scaled to fit w x h, premultiplied BGRA */
static BYTE* logo_decode(UINT maxw, UINT maxh, UINT* ow, UINT* oh){
  HRSRC rs=FindResourceA(NULL,MAKEINTRESOURCEA(3),MAKEINTRESOURCEA(10)); if(!rs) return NULL;
  HGLOBAL g=LoadResource(NULL,rs); void* data= g? LockResource(g) : NULL; DWORD size=SizeofResource(NULL,rs); if(!data || !size) return NULL;
  IWICImagingFactory* f=NULL; IWICStream* st=NULL; IWICBitmapDecoder* d=NULL; IWICBitmapFrameDecode* fr=NULL; IWICBitmapScaler* sc=NULL; IWICFormatConverter* cv=NULL; BYTE* px=NULL;
  if(FAILED(CoCreateInstance(&S_CLSID_Factory,NULL,CLSCTX_INPROC_SERVER,&S_IID_Factory,(void**)&f))) goto done;
  if(FAILED(IWICImagingFactory_CreateStream(f,&st))) goto done;
  if(FAILED(IWICStream_InitializeFromMemory(st,(BYTE*)data,size))) goto done;
  if(FAILED(IWICImagingFactory_CreateDecoderFromStream(f,(IStream*)st,NULL,WICDecodeMetadataCacheOnLoad,&d))) goto done;
  if(FAILED(IWICBitmapDecoder_GetFrame(d,0,&fr))) goto done;
  UINT iw=0, ih=0; IWICBitmapFrameDecode_GetSize(fr,&iw,&ih); if(!iw || !ih) goto done;
  double k=(double)maxw/iw, k2=(double)maxh/ih; if(k2<k) k=k2; UINT w=(UINT)(iw*k+0.5), h=(UINT)(ih*k+0.5); if(!w) w=1; if(!h) h=1;
  if(FAILED(IWICImagingFactory_CreateBitmapScaler(f,&sc))) goto done;
  if(FAILED(IWICBitmapScaler_Initialize(sc,(IWICBitmapSource*)fr,w,h,WICBitmapInterpolationModeFant))) goto done;
  if(FAILED(IWICImagingFactory_CreateFormatConverter(f,&cv))) goto done;
  if(FAILED(IWICFormatConverter_Initialize(cv,(IWICBitmapSource*)sc,&S_PBGRA,WICBitmapDitherTypeNone,NULL,0.0,WICBitmapPaletteTypeCustom))) goto done;
  px=(BYTE*)malloc((size_t)w*h*4); if(!px) goto done;
  if(FAILED(IWICFormatConverter_CopyPixels(cv,NULL,w*4,w*h*4,px))){ free(px); px=NULL; goto done; }
  *ow=w; *oh=h;
done:
  if(cv) IWICFormatConverter_Release(cv); if(sc) IWICBitmapScaler_Release(sc); if(fr) IWICBitmapFrameDecode_Release(fr);
  if(d) IWICBitmapDecoder_Release(d); if(st) IWICStream_Release(st); if(f) IWICImagingFactory_Release(f);
  return px; }

static DWORD WINAPI splash_thread(void* p){
  SplashArg a=*(SplashArg*)p; free(p); u64 t0=plat_ns();
  int w=a.r.right-a.r.left, h=a.r.bottom-a.r.top; if(w<16 || h<16) return 0;
  CoInitializeEx(NULL,COINIT_MULTITHREADED);
  UINT lw=0, lh=0; BYTE* logo=logo_decode((UINT)(w*7/10),(UINT)(h*7/10),&lw,&lh); CoUninitialize();
  if(!logo){ LOG("splash: the logo could not be read\n"); return 0; }
  HDC scr=GetDC(NULL), mem=CreateCompatibleDC(scr); void* bits=NULL;
  BITMAPINFO bi; memset(&bi,0,sizeof bi); bi.bmiHeader.biSize=sizeof bi.bmiHeader; bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
  HBITMAP bmp=CreateDIBSection(scr,&bi,DIB_RGB_COLORS,&bits,NULL,0);
  if(!bmp || !bits){ free(logo); if(bmp) DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL,scr); return 0; }
  HGDIOBJ old=SelectObject(mem,bmp);
  /* black, opaque: the logo's see-through parts show black, and the launcher behind is covered */
  { u32* d=(u32*)bits; for(int i=0;i<w*h;i++) d[i]=0xFF000000u; int ox=(w-(int)lw)/2, oy=(h-(int)lh)/2;
    for(UINT y=0;y<lh;y++) for(UINT x=0;x<lw;x++){ const BYTE* s=logo+((size_t)y*lw+x)*4; d[(size_t)(y+oy)*w+x+ox]=0xFF000000u|((u32)s[2]<<16)|((u32)s[1]<<8)|s[0]; } }
  free(logo);
  WNDCLASSA wc; memset(&wc,0,sizeof wc); wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandle(NULL); wc.lpszClassName="MetroidPrime4Splash"; wc.hIcon=LoadIcon(wc.hInstance,MAKEINTRESOURCE(1)); RegisterClassA(&wc);
  HWND hw=CreateWindowExA(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,"MetroidPrime4Splash","Metroid Prime 4 Beyond",WS_POPUP,a.r.left,a.r.top,w,h,NULL,NULL,wc.hInstance,NULL);
  if(hw){
    ShowWindow(hw,SW_SHOWNOACTIVATE);
    POINT dst={a.r.left,a.r.top}, src={0,0}; SIZE sz={w,h}; BLENDFUNCTION bf={AC_SRC_OVER,0,0,AC_SRC_ALPHA};
    #define S_PUT(al) do{ bf.SourceConstantAlpha=(BYTE)(al); UpdateLayeredWindow(hw,NULL,&dst,&sz,mem,&src,0,&bf,ULW_ALPHA); }while(0)
    const double fade=0.35; double out=0; u64 tstop=0; int shown=0;
    for(;;){ MSG m; while(PeekMessageA(&m,NULL,0,0,PM_REMOVE)){ TranslateMessage(&m); DispatchMessageA(&m); }
      double t=(plat_ns()-t0)/1e9;
      if(!tstop && (g_stop || t>60.0) && t>=1.2){ tstop=plat_ns(); LOG("splash: shown %.1f s, the game has its first frame%s\n",t,g_stop? "" : " (not seen: gave up waiting)"); }   /* never just a flash */
      double a1= t>=fade? 1.0 : t/fade; if(tstop){ out=(plat_ns()-tstop)/1e9/fade; if(out>=1.0) break; a1*=1.0-out; }
      S_PUT(a1*255.0); if(!shown){ shown=1; LOG("splash: on (%dx%d)\n",w,h); }
      Sleep(16); }
    DestroyWindow(hw); }
  SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL,scr);
  return 0; }

/* r: the area the game's picture will fill, in screen pixels */
void splash_start(int x0, int y0, int x1, int y1){
  if(!g_opt_splash || getenv("SWR_NO_SPLASH")) return;
  SplashArg* a=(SplashArg*)malloc(sizeof *a); if(!a) return; SetRect(&a->r,x0,y0,x1,y1);
  HANDLE t=CreateThread(NULL,256*1024,splash_thread,a,0,NULL); if(t) CloseHandle(t); else free(a); }
void splash_first_swap(void){ InterlockedExchange(&g_stop,1); }

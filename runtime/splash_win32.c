/* Splash screen between the Play button and the game's first frame: the game's logo on black, over the window the game
   will use. Its own layered window on its own thread: it never touches the game's window, whose presents are particular
   (see win_thread), and it keeps fading while the game loads. Ends at the first swap (splash_first_swap), or after a
   minute whatever happens. ini splash=0: none */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdio.h>
#include <math.h>
#include "rt.h"

int g_opt_splash=1;
static volatile LONG g_stop;
/* progress, from the runtime: phase 0 = shader programs being prepared (frac = how far through the pack file, most of
   the launch), phase 1 = done, the game is booting (about 1.5 s more). -1: nothing reported (no pack yet, or a renderer
   that doesn't report), the bar then only creeps by time */
static volatile LONG g_phase=-1; static volatile float g_frac; static volatile LONGLONG g_t_boot;
void splash_progress(int phase, float frac){ if(frac<0) frac=0; if(frac>1) frac=1; g_frac=frac;
  if(phase==1 && g_phase!=1) g_t_boot=(LONGLONG)plat_ns(); InterlockedExchange(&g_phase,phase); }
static u32 mix(u32 a, u32 b, double t){ u32 r=0; for(int s=0;s<24;s+=8){ double x=((a>>s)&255)*(1-t)+((b>>s)&255)*t; r|=(u32)(x+0.5)<<s; } return r; }
/* the bar, its glow and the label, over the saved logo image (base) */
static void draw_bar(u32* d, const u32* base, HDC mem, HFONT font, int w, int h, int by, double p){
  int bw=w*34/100, bh= h/300>3? h/300 : 3, bx=(w-bw)/2, gl= bh*2, y0=by-gl, y1=by+bh+gl+h/14; if(y1>h) y1=h; if(y0<0) y0=0;
  for(int y=y0;y<y1;y++) memcpy(d+(size_t)y*w,base+(size_t)y*w,(size_t)w*4);
  int fill=(int)(bw*p+0.5);
  for(int y=by-gl;y<by+bh+gl;y++){ if(y<0 || y>=h) continue; int in= y>=by && y<by+bh; int dy= y<by? by-y : y-(by+bh-1); double gk= in? 1 : 0.28*(1.0-(double)dy/(gl+1));
    for(int x=bx-gl;x<bx+bw+gl;x++){ if(x<0 || x>=w) continue; u32 c;
      if(in && x>=bx && x<bx+bw){ c= x<bx+fill? mix(0x7B4DFF,0xD2B4FF,(double)(x-bx)/(bw>1? bw-1 : 1)) : 0x2A2340; }
      else if(x>=bx && x<bx+fill){ c=mix(0x000000,0x9A70FF,gk); }   /* glow around the filled part only */
      else continue;
      d[(size_t)y*w+x]=0xFF000000u|c; } }
  if(font){ char lab[64]; LONG ph=g_phase; snprintf(lab,sizeof lab,"%s  %d%%", ph==0? "Preparing shaders" : ph==1 || g_stop? "Starting the game" : "Loading",(int)(p*100+0.5));
    GdiFlush(); HGDIOBJ of=SelectObject(mem,font); SetBkMode(mem,TRANSPARENT); SetTextColor(mem,RGB(178,166,214));
    RECT r={0,by+bh+h/60,w,y1}; DrawTextA(mem,lab,-1,&r,DT_CENTER|DT_TOP|DT_SINGLELINE|DT_NOPREFIX); SelectObject(mem,of); GdiFlush();
    for(int y=r.top;y<r.bottom && y<h;y++) for(int x=0;x<w;x++) d[(size_t)y*w+x]|=0xFF000000u; } }   /* GDI text leaves alpha 0 */
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
  int by=(h+(int)lh)/2+h*4/100; if(by>h*88/100) by=h*88/100;
  u32* base=(u32*)malloc((size_t)w*h*4); if(base) memcpy(base,bits,(size_t)w*h*4);
  HFONT font=CreateFontA(-(h*2/100>12? h*2/100 : 12),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
  double shown_p=0;
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
      if(base){ LONG ph=g_phase; double tg;
        if(g_stop) tg=1.0;
        else if(ph==0) tg=0.04+0.84*g_frac;
        else if(ph==1) tg=0.88+0.11*(1.0-exp(-((double)plat_ns()-(double)g_t_boot)/1e9/0.8));
        else tg=0.9*(1.0-exp(-t/5.0));
        if(tg>shown_p) shown_p+= (tg-shown_p)*(g_stop? 0.35 : 0.15);   /* never goes back */
        draw_bar((u32*)bits,base,mem,font,w,h,by,shown_p); }
      S_PUT(a1*255.0); if(!shown){ shown=1; LOG("splash: on (%dx%d)\n",w,h); }
      Sleep(16); }
    DestroyWindow(hw); }
  free(base); if(font) DeleteObject(font);
  SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL,scr);
  return 0; }

/* r: the area the game's picture will fill, in screen pixels */
void splash_start(int x0, int y0, int x1, int y1){
  if(!g_opt_splash || getenv("SWR_NO_SPLASH")) return;
  SplashArg* a=(SplashArg*)malloc(sizeof *a); if(!a) return; SetRect(&a->r,x0,y0,x1,y1);
  HANDLE t=CreateThread(NULL,256*1024,splash_thread,a,0,NULL); if(t) CloseHandle(t); else free(a); }
void splash_first_swap(void){ InterlockedExchange(&g_stop,1); }

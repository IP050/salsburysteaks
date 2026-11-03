/* The game's own mouse pointer (Play screen, F1 menu, file pickers): drawn here at start, sized for the display, so
   there is no image file. An arrow in a 32-unit box with its tip at (1,1): a dark outline, a cyan visor gradient, a
   light edge along its left side and an orange tail. cursor_pixels is plain C so a test can write it to a picture. */
#include <math.h>
#include <string.h>

static const float CP[][2]={{1.0f,1.0f},{1.0f,23.0f},{5.9f,18.5f},{9.5f,27.3f},{14.3f,25.3f},{10.8f,16.9f},{17.0f,16.9f}};
#define CPN ((int)(sizeof CP/sizeof CP[0]))

/* signed distance from (x,y) to the arrow in units: negative inside */
static float c_sdist(float x, float y){
  float d=1e9f; int in=0;
  for(int i=0,j=CPN-1;i<CPN;j=i++){
    float ax=CP[j][0], ay=CP[j][1], bx=CP[i][0], by=CP[i][1], ex=bx-ax, ey=by-ay, wx=x-ax, wy=y-ay;
    float t=(wx*ex+wy*ey)/(ex*ex+ey*ey); if(t<0) t=0; if(t>1) t=1;
    float dx=wx-ex*t, dy=wy-ey*t, e=dx*dx+dy*dy; if(e<d) d=e;
    if((ay>y)!=(by>y) && x<ax+(y-ay)*ex/ey) in=!in; }
  d=sqrtf(d); return in? -d : d; }

static float c_clamp(float v){ return v<0? 0 : v>1? 1 : v; }
static float c_mix(float a, float b, float t){ return a+(b-a)*t; }

/* n x n pixels, 0xAARRGGBB, straight alpha (what a 32-bit Windows cursor takes) */
void cursor_pixels(unsigned* argb, int n){
  const float u=32.0f/(float)n, ol=1.5f;   /* units per pixel, outline width */
  for(int py=0;py<n;py++) for(int px=0;px<n;px++){
    float x=((float)px+0.5f)*u, y=((float)py+0.5f)*u, sd=c_sdist(x,y);
    float cov=c_clamp(0.5f-sd/u);   /* antialiased silhouette */
    float sh=c_clamp(0.5f-c_sdist(x-1.1f,y-1.5f)/(u*2.5f))*0.38f;   /* soft drop shadow down and right */
    /* fill: top to bottom cyan gradient, a lighter band along the left edge, the tail orange */
    float t=c_clamp((y-1.0f)/20.0f), r=c_mix(150,20,t), g=c_mix(248,150,t), b=c_mix(255,205,t);
    if(x<1.0f+ol+1.6f && y>3.0f){ float k=0.45f*c_clamp((1.0f+ol+1.6f-x)/1.6f); r=c_mix(r,255,k); g=c_mix(g,255,k); b=c_mix(b,255,k); }
    if(y>19.2f){ float k=c_clamp((y-19.2f)/1.2f), q=c_clamp((y-20.0f)/6.0f); r=c_mix(r,c_mix(255,255,q),k); g=c_mix(g,c_mix(184,112,q),k); b=c_mix(b,c_mix(80,24,q),k); }
    float f=c_clamp((-sd-ol)/u+0.5f);   /* fill over the outline (#08111C) */
    r=c_mix(8,r,f); g=c_mix(17,g,f); b=c_mix(28,b,f);
    float a=cov+sh*(1.0f-cov);   /* the arrow over its shadow (black) */
    if(a>0){ r=r*cov/a; g=g*cov/a; b=b*cov/a; }
    unsigned A=(unsigned)(a*255.0f+0.5f), R=(unsigned)(r+0.5f), G=(unsigned)(g+0.5f), B=(unsigned)(b+0.5f);
    argb[py*n+px]=A<<24|R<<16|G<<8|B; } }

#ifndef CURSOR_TEST
#include <windows.h>
/* the pointer as a Windows cursor, at the size Windows uses for cursors on this display */
HCURSOR cursor_game(void){
  static HCURSOR c; static int done; if(done) return c? c : LoadCursor(NULL,IDC_ARROW); done=1;
  int n=GetSystemMetrics(SM_CXCURSOR); if(n<32) n=32; if(n>128) n=128;
  BITMAPV5HEADER bi; memset(&bi,0,sizeof bi); bi.bV5Size=sizeof bi; bi.bV5Width=n; bi.bV5Height=-n; bi.bV5Planes=1; bi.bV5BitCount=32;
  bi.bV5Compression=BI_BITFIELDS; bi.bV5RedMask=0x00FF0000; bi.bV5GreenMask=0x0000FF00; bi.bV5BlueMask=0x000000FF; bi.bV5AlphaMask=0xFF000000;
  void* bits=NULL; HDC dc=GetDC(NULL); HBITMAP col=CreateDIBSection(dc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,&bits,NULL,0); ReleaseDC(NULL,dc);
  HBITMAP mask=CreateBitmap(n,n,1,1,NULL);
  if(col && mask && bits){ cursor_pixels((unsigned*)bits,n); int hs=(int)(1.0f*n/32.0f+0.5f);
    ICONINFO ii={FALSE,(DWORD)hs,(DWORD)hs,mask,col}; c=CreateIconIndirect(&ii); }
  if(col) DeleteObject(col); if(mask) DeleteObject(mask);
  return c? c : LoadCursor(NULL,IDC_ARROW); }
#endif

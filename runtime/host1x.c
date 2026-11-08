/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* nvhost-nvdec / nvhost-vic (host1x channels).
   We parse the host1x stream, decode NVDEC H.264 jobs with OpenH264 (SPS/PPS get rebuilt from the
   picture setup the game fills in) and VIC jobs copy the last decoded picture into the output surface.
   Struct layouts / method offsets come from NVIDIA's open-gpu-doc (nvdec_drv.h, clc5b0.h, VIC). */
#include "rt.h"
#ifndef SWR_NO_H264
#include "wels/codec_api.h"
#ifdef SWR_H264_DLOPEN
/* no static OpenH264 in this build (Linux, linux/build_openh264.sh not run): the system's libopenh264 (2.x, the
   distribution's package or Cisco's binary), found at the first video. The decoder is used through WelsCreateDecoder and
   its function table only, which OpenH264 2.x keeps compatible */
#include <dlfcn.h>
static long (*p_WelsCreateDecoder)(ISVCDecoder**);
static int h264_load(void){
  static const char* const names[]={"libopenh264.so.8","libopenh264.so.7","libopenh264.so.6","libopenh264.so"};
  for(int i=0;i<4;i++){ void* h=dlopen(names[i],RTLD_NOW|RTLD_LOCAL); if(!h) continue;
    OpenH264Version (*ver)(void)=(OpenH264Version(*)(void))dlsym(h,"WelsGetCodecVersion");
    p_WelsCreateDecoder=(long(*)(ISVCDecoder**))dlsym(h,"WelsCreateDecoder");
    OpenH264Version v={0,0,0,0}; if(ver) v=ver();
    if(p_WelsCreateDecoder && v.uMajor==2){ LOG("video: H.264 from the system's %s (OpenH264 %u.%u.%u)\n",names[i],v.uMajor,v.uMinor,v.uRevision); return 1; }
    LOG("video: %s is OpenH264 %u.%u, not 2.x: not used\n",names[i],v.uMajor,v.uMinor); p_WelsCreateDecoder=NULL; dlclose(h); }
  LOG("video: no H.264 decoder: install OpenH264 2.x (package openh264 / libopenh264) for the game's videos\n"); return 0; }
#define WelsCreateDecoder(p) p_WelsCreateDecoder(p)
#endif
#endif

u64 nvmap_addr(u32 h); u64 nvmap_addr_by_id(u32 id);
u32 syncpt_read(u32 id); void syncpt_incr(u32 id);

/* ------------------------------------------------------------ IOVA (what the video engines see through the SMMU) */
/* A buffer keeps its device address (iova) from its first MAP_BUFFER until the buffer itself goes away, as on the
   console, where UNMAP_BUFFER only unpins. refs counts the maps; live stays set after the last unmap. Metroid Prime 4
   maps its video buffers around each job and keeps using the addresses after the unmap: freeing them at refs 0 gave
   the next buffer the same iova, and the decoder lost its picture setup after three frames */
typedef struct { u32 iova, size; u64 addr; u32 h,refs,live; } IoMap;
static IoMap g_iom[1024]; static int g_niom;  static PMutex g_hlk; static int g_hinit;

static void h_init(void){ if(!g_hinit){ pm_init(&g_hlk); g_hinit=1; } }

/* romfs hook (svc_fs.c): remember the .webm files the game reads, newest first. VP9 video uses them (vp9video.inc) */
/* Every .webm the game has opened, most recent first. A game can open a video once
   (e.g. title videos at boot) and replay it later from memory, so the list must not
   forget files: a replayed video that cannot be matched to its file used to be shown
   as frames of whatever video played last. */
#define V9_FILES 64
static const char* g_v9_recent[V9_FILES];
void vp9_note_file(const char* path){
  if(g_v9_recent[0]==path) return;
  size_t n=strlen(path); if(n<5 || strcmp(path+n-5,".webm")) return;
  h_init(); pm_lock(&g_hlk);
  int k=0; for(;k<V9_FILES-1;k++) if(g_v9_recent[k]==path || !g_v9_recent[k]) break;
  for(;k>0;k--) g_v9_recent[k]=g_v9_recent[k-1];
  g_v9_recent[0]=path; pm_unlock(&g_hlk);
  if(getenv("SWR_TRACE_VP9")) LOG("vp9 note: game opened %s\n",path); }

static u64 iova_handle_addr(u32 h){ u64 a=nvmap_addr(h); if(!a) a=nvmap_addr_by_id(h); return a; }
/* still the buffer it was mapped for (a freed or reallocated handle is not) */
static int iom_valid(const IoMap* m){ return m->live && (m->refs || iova_handle_addr(m->h)==m->addr); }
static u32 iova_map(u32 h){
  u64 a=iova_handle_addr(h); if(!a) return 0;
  int slot=-1;
  for(int i=0;i<g_niom;i++){if(g_iom[i].live&&g_iom[i].h==h&&g_iom[i].addr==a){g_iom[i].refs++;return g_iom[i].iova;}
    if(slot<0&&!iom_valid(&g_iom[i]))slot=i;}
  extern u32 nvmap_size(u32 h);u32 sz=nvmap_size(h);if(!sz)sz=0x100000;
  if(slot<0){if(g_niom>=1024)return 0;slot=g_niom++;}
  /* Reclaim gaps instead of wrapping a monotonic 32-bit address counter. */
  u64 span=((u64)sz+0xffff)&~0xffffull,base=0x10000000ull;int again;
  do{again=0;for(int i=0;i<g_niom;i++)if(i!=slot&&iom_valid(&g_iom[i])){u64 lo=g_iom[i].iova,hi=lo+(((u64)g_iom[i].size+0xffff)&~0xffffull)+0x10000;
    if(base<hi&&base+span+0x10000>lo){base=hi;again=1;break;}}}while(again&&base+span<=0xffff0000ull);
  if(base+span>0xffff0000ull)return 0;
  g_iom[slot]=(IoMap){(u32)base,sz,a,h,1,1};return (u32)base;
}
static void iova_unmap(u32 h){
  for(int i=0;i<g_niom;i++)if(g_iom[i].refs&&g_iom[i].h==h){g_iom[i].refs--;return;}
}

/* 40-bit iova (engines get it >>8) -> host pointer */
static u8* iova_ptr(u64 iova, u64 len){
  for(int i=0;i<g_niom;i++){ IoMap* m=&g_iom[i]; if(iom_valid(m) && iova>=m->iova && len<=m->size && iova+len>=iova && iova+len<=(u64)m->iova+m->size) return GP(m->addr+(iova-m->iova)); }
  return NULL; }

/* Host-written video surfaces: renderer checks revisions instead of texture polling.
   host1x_ioctl serializes writers. Atomic fields keep renderer reads lock-free,
   avoiding a lock-order dependency between the renderer and video services. */
typedef struct { u64 cpu, size, gen; } VideoWrite;
static VideoWrite g_video_writes[16]; static u32 g_video_slot;
static u64 g_video_generation;
u64 video_write_generation(void){ return __atomic_load_n(&g_video_generation,__ATOMIC_ACQUIRE); }
static void video_written(u8* p,u64 size){
  u64 cpu=(u64)(uintptr_t)p; u32 k=0;
  for(;k<16;k++) if(__atomic_load_n(&g_video_writes[k].cpu,__ATOMIC_RELAXED)==cpu) break;
  if(k==16) k=g_video_slot++%16;
  u64 gen=video_write_generation()+1;
  __atomic_store_n(&g_video_writes[k].cpu,cpu,__ATOMIC_RELAXED);
  __atomic_store_n(&g_video_writes[k].size,size,__ATOMIC_RELAXED);
  __atomic_store_n(&g_video_writes[k].gen,gen,__ATOMIC_RELEASE);
  __atomic_store_n(&g_video_generation,gen,__ATOMIC_RELEASE);
}
u64 video_surface_generation(const u8* p,u64 size){
  if(!p || !size) return 0; u64 cpu=(u64)(uintptr_t)p, latest=0;
  for(u32 k=0;k<16;k++){
    u64 gen=__atomic_load_n(&g_video_writes[k].gen,__ATOMIC_ACQUIRE);
    u64 a=__atomic_load_n(&g_video_writes[k].cpu,__ATOMIC_RELAXED);
    u64 n=__atomic_load_n(&g_video_writes[k].size,__ATOMIC_RELAXED);
    if(gen && a<cpu+size && cpu<a+n && gen>latest) latest=gen;
  }
  return latest;
}

/* ------------------------------------------------------------ H.264: SPS/PPS rebuilt from nvdec_h264_pic_s */
typedef struct { u8* p; u32 cap, bytes; u32 acc; int nb; } BW;

static void bw_bit(BW* w, u32 b){ w->acc=(w->acc<<1)|(b&1); if(++w->nb==8){ if(w->bytes<w->cap) w->p[w->bytes++]=(u8)w->acc; w->acc=0; w->nb=0; } }

static void bw_u(BW* w, u32 v, int n){ for(int i=n-1;i>=0;i--) bw_bit(w,(v>>i)&1); }

static void bw_ue(BW* w, u32 v){ u32 x=v+1; int n=0; while((x>>n)>1) n++; for(int i=0;i<n;i++) bw_bit(w,0); bw_u(w,x,n+1); }

static void bw_se(BW* w, i32 v){ bw_ue(w, v>0? (u32)(2*v-1) : (u32)(-2*v)); }

static void bw_trailing(BW* w){ bw_bit(w,1); while(w->nb) bw_bit(w,0); }

/* start code + emulation prevention bytes */
static u32 nal_out(u8* dst, u32 cap, u8 hdr, const u8* rbsp, u32 n){
  u32 o=0; if(cap<5) return 0; dst[o++]=0; dst[o++]=0; dst[o++]=0; dst[o++]=1; dst[o++]=hdr; int z=0;
  for(u32 i=0;i<n && o+2<cap;i++){ if(z>=2 && rbsp[i]<=3){ dst[o++]=3; z=0; } dst[o++]=rbsp[i]; z= rbsp[i]==0? z+1 : 0; }
  return o; }

static u32 rd32(const u8* p, u32 off){ u32 v; memcpy(&v,p+off,4); return v; }

static i32 sext(u32 v, int bits){ return (i32)(v<<(32-bits))>>(32-bits); }
static const u8 zz4[16]={0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
static const u8 zz8[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void scaling_list(BW* w, const u8* raster, int n, const u8* zz){
  int last=8; for(int j=0;j<n;j++){ int v=raster[zz[j]]; int d=v-last; if(d>127) d-=256; if(d<-128) d+=256; bw_se(w,d); last=v; } }

static u32 build_headers(const u8* ps, u8* out, u32 cap){
  u8 rb[1024]; BW w; u32 o=0;
  u32 log2_poc=rd32(ps,0x58), dpoc0=rd32(ps,0x5C), fmo=rd32(ps,0x60), wmb=rd32(ps,0x64), hmb=rd32(ps,0x68);
  u32 ecm=rd32(ps,0x70), pop=rd32(ps,0x74), l0=rd32(ps,0x78), l1=rd32(ps,0x7C), dfc=rd32(ps,0x80), rpc=rd32(ps,0x84), t8=rd32(ps,0x88);
  u32 b0=rd32(ps,0xB0), b1=rd32(ps,0xB4), bl=rd32(ps,0x2D0);
  u32 mbaff=b0&1, d8=(b0>>1)&1, wp=(b0>>2)&1, cip=(b0>>3)&1, l2fn=(b0>>8)&15, cfi=(b0>>12)&3, poct=(b0>>14)&3;
  i32 qp26=sext((b0>>16)&63,6), cqo=sext((b0>>22)&31,5), cqo2=sext((b0>>27)&31,5);
  u32 wbi=b1&3, qpz=(bl>>1)&1;
  /* SPS, High profile so the PPS may use 8x8 transform / scaling lists */
  memset(&w,0,sizeof w); w.p=rb; w.cap=sizeof rb;
  bw_u(&w,100,8); bw_u(&w,0,8); bw_u(&w,51,8); bw_ue(&w,0);
  bw_ue(&w,cfi? cfi : 1); bw_ue(&w,0); bw_ue(&w,0); bw_u(&w,qpz,1); bw_u(&w,0,1);
  bw_ue(&w,l2fn); bw_ue(&w,poct);
  if(poct==0) bw_ue(&w,log2_poc);
  else if(poct==1){ bw_u(&w,dpoc0,1); bw_se(&w,0); bw_se(&w,0); bw_ue(&w,0); }
  bw_ue(&w,16); bw_u(&w,0,1);
  bw_ue(&w,wmb? wmb-1 : 0); bw_ue(&w, fmo? (hmb? hmb-1 : 0) : (hmb/2? hmb/2-1 : 0));
  bw_u(&w,fmo,1); if(!fmo) bw_u(&w,mbaff,1);
  bw_u(&w,d8,1); bw_u(&w,0,1); bw_u(&w,0,1); bw_trailing(&w);
  o+=nal_out(out+o,cap-o,0x67,rb,w.bytes);
  /* PPS, scaling matrices taken from the pic setup */
  memset(&w,0,sizeof w); w.p=rb; w.cap=sizeof rb;
  bw_ue(&w,0); bw_ue(&w,0); bw_u(&w,ecm,1); bw_u(&w,pop,1); bw_ue(&w,0);
  bw_ue(&w,l0); bw_ue(&w,l1); bw_u(&w,wp,1); bw_u(&w,wbi,2); bw_se(&w,qp26); bw_se(&w,0); bw_se(&w,cqo);
  bw_u(&w,dfc,1); bw_u(&w,cip,1); bw_u(&w,rpc,1);
  bw_u(&w,t8,1); bw_u(&w,1,1);
  for(int i=0;i<6;i++){ bw_u(&w,1,1); scaling_list(&w,ps+0x1C0+i*16,16,zz4); }
  if(t8) for(int i=0;i<2;i++){ bw_u(&w,1,1); scaling_list(&w,ps+0x220+i*64,64,zz8); }
  bw_se(&w,cqo2); bw_trailing(&w);
  o+=nal_out(out+o,cap-o,0x68,rb,w.bytes);
  return o; }

/* ------------------------------------------------------------ decoder and the last picture */
/* one decoder per NVDEC channel (file descriptor): a game that plays two videos at once (Metroid Prime 4 opens two
   channels) must not feed both streams into one decoder, whose reference pictures and parameter sets would then mix
   (gray, concealed pictures after the first frames). g_stream is the channel of the job being run */
#define H1_STREAMS 32
static FILE* g_regs_trace;   /* SWR_TRACE_NVDEC_REGS: nvdec_regs.txt, decode and VIC jobs in order */
static int g_stream;
#ifndef SWR_NO_H264
static ISVCDecoder* g_decs[H1_STREAMS];
#define g_dec g_decs[g_stream]
#endif
static int g_dec_fail;
static u8* g_fy; static u8* g_fuv; static u32 g_fw, g_fh, g_fcap, g_fuv_cap; static u32 g_vframes;
/* NVDEC current-picture surfaces are addressed by VIC's input plane offsets.
   Keep independent pictures instead of converting whichever stream decoded last. */
typedef struct {u64 luma,display,used,owner;u8*y,*uv;u32 w,h,cap,frame;} DecodedSurface;
static DecodedSurface decoded_surfaces[32];static u64 decoded_clock;
static int g_h264_filed;   /* H.264 pictures are filed by surface: VIC takes the exact one or none (vic_execute) */
static u64 g_save_luma;   /* H.264: the current picture's surface, from its picture setup (nvdec_execute) */
static void decoded_surface_save(u32*r,u64 owner,u32 frame,int cached){
  u64 luma= g_save_luma? g_save_luma : (u64)r[0x43c/4]<<8,display=(u64)r[0x428/4]<<8;
  /* H.264: 0x428 is the surface of the picture decoded before this one (being displayed meanwhile), not another name
     for this picture. Matching VIC requests against it returned the next picture for the previous one's surface:
     pictures shown in steps of 0, 1 and 2 (a repeat and a skip every third frame, Metroid Prime 4's intro judder) */
  if(g_save_luma) display=0;
  if(!luma)luma=display;if(!luma||!g_fw||!g_fh||!g_fy||!g_fuv)return;
  u8*sy=g_fy,*suv=g_fuv;u32 w=g_fw,h=g_fh;if(cached){DecodedSurface*prior=NULL;for(int i=0;i<32;i++)if(decoded_surfaces[i].used&&decoded_surfaces[i].owner==owner){if(decoded_surfaces[i].frame==frame){prior=&decoded_surfaces[i];break;}if(!prior||decoded_surfaces[i].used>prior->used)prior=&decoded_surfaces[i];}if(!prior)return;sy=prior->y;suv=prior->uv;w=prior->w;h=prior->h;}
  int k=-1;for(int i=0;i<32;i++)if(decoded_surfaces[i].luma==luma){k=i;break;}
  if(k<0){k=0;for(int i=1;i<32;i++)if(decoded_surfaces[i].used<decoded_surfaces[k].used)k=i;}
  DecodedSurface*d=&decoded_surfaces[k];u32 need=w*h;if(d->cap<need){u8*y=malloc(need),*uv=malloc(need/2+w+16);if(!y||!uv){free(y);free(uv);return;}free(d->y);free(d->uv);d->y=y;d->uv=uv;d->cap=need;}
  d->luma=luma;d->display=display;d->w=w;d->h=h;d->owner=owner;d->frame=frame;d->used=++decoded_clock;if(d->y!=sy)memcpy(d->y,sy,need);if(d->uv!=suv)memcpy(d->uv,suv,(size_t)w*((h+1)/2));
  if(getenv("SWR_TRACE_VIC")){static u64 seen[64];static u32 ns;u64 key=luma^((u64)g_fw<<32)^g_fh;int i;for(i=0;i<ns;i++)if(seen[i]==key)break;if(i==ns&&ns<64){seen[ns++]=key;LOG("nvdec surface: current=%llx display=%llx picture=%ux%u planes=%x,%x,%x,%x,%x,%x,%x,%x current16=%x picindex=%x\n",(unsigned long long)luma,(unsigned long long)display,g_fw,g_fh,r[0x430/4],r[0x434/4],r[0x438/4],r[0x43c/4],r[0x440/4],r[0x444/4],r[0x448/4],r[0x44c/4],r[0x470/4],r[0x40c/4]);}}
}
static DecodedSurface*decoded_surface_find(u64 luma){if(!luma)return NULL;for(int i=0;i<32;i++)if(decoded_surfaces[i].used&&(decoded_surfaces[i].luma==luma||decoded_surfaces[i].display==luma))return &decoded_surfaces[i];return NULL;}

static u8 g_last_hdrs[H1_STREAMS][512]; static u32 g_last_hdr_ns[H1_STREAMS];
#define g_last_hdr g_last_hdrs[g_stream]
#define g_last_hdr_n g_last_hdr_ns[g_stream]

#ifdef SWR_H264_FFMPEG
/* FFmpeg's H.264 decoder when the system has the libavcodec this runtime was built against (Linux): about twice as fast
   as OpenH264 on the game's 1080p60 videos (4.1 vs 8.5 ms a picture, 40 ms OpenH264 worst case), so the title screen's
   heavy background video keeps 60 FPS. Single-threaded with low delay: a picture for every job, as NVDEC gives (the
   game's streams have no B-frames). SWR_H264_DECODER=openh264 uses OpenH264 instead */
#include <dlfcn.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#define STR2_(x) #x
#define STR_(x) STR2_(x)
static struct { const AVCodec*(*find)(enum AVCodecID); AVCodecContext*(*alloc)(const AVCodec*); int(*open)(AVCodecContext*,const AVCodec*,AVDictionary**);
  AVPacket*(*pkt_alloc)(void); int(*send)(AVCodecContext*,const AVPacket*); int(*recv)(AVCodecContext*,AVFrame*); AVFrame*(*frame_alloc)(void);
  const AVCodec* codec; AVPacket* pkt; AVFrame* frame;
  /* hardware decoding (VA-API): the GPU's video decoder, a 1080p picture in 1-3 ms without the CPU. Software decoding
     needed about 20 ms for a keyframe, so a picture came late about once a second (more with the CPU busy, e.g. while
     recording the screen) and the previous one stayed on screen. SWR_H264_DECODER=ffmpeg-sw: software only */
  int(*hw_create)(AVBufferRef**,enum AVHWDeviceType,const char*,AVDictionary*,int); int(*hw_transfer)(AVFrame*,const AVFrame*,int);
  AVBufferRef*(*buf_ref)(const AVBufferRef*); void(*frame_unref)(AVFrame*); AVBufferRef* hwdev; AVFrame* sw; } ff;
static AVCodecContext* g_ffc[H1_STREAMS];
static int g_ff=-1;   /* -1 not tried, 0 unavailable, 1 in use */
static int ff_load(void){
  if(g_ff>=0) return g_ff; g_ff=0;
  const char* want=getenv("SWR_H264_DECODER"); if(want && strcmp(want,"ffmpeg")) return 0;
  const char* names[2]={"libavcodec.so." STR_(LIBAVCODEC_VERSION_MAJOR),"libavutil.so." STR_(LIBAVUTIL_VERSION_MAJOR)};
  void* hc=dlopen(names[0],RTLD_NOW|RTLD_LOCAL); void* hu= hc? dlopen(names[1],RTLD_NOW|RTLD_LOCAL) : NULL;
  if(!hc || !hu){ LOG("video: no %s (FFmpeg %d) on this system: OpenH264 instead\n",names[0],LIBAVCODEC_VERSION_MAJOR); return 0; }
  ff.find=dlsym(hc,"avcodec_find_decoder"); ff.alloc=dlsym(hc,"avcodec_alloc_context3"); ff.open=dlsym(hc,"avcodec_open2");
  ff.pkt_alloc=dlsym(hc,"av_packet_alloc"); ff.send=dlsym(hc,"avcodec_send_packet"); ff.recv=dlsym(hc,"avcodec_receive_frame"); ff.frame_alloc=dlsym(hu,"av_frame_alloc");
  if(!ff.find || !ff.alloc || !ff.open || !ff.pkt_alloc || !ff.send || !ff.recv || !ff.frame_alloc || !(ff.codec=ff.find(AV_CODEC_ID_H264))){ LOG("video: %s has no usable H.264 decoder: OpenH264 instead\n",names[0]); return 0; }
  ff.pkt=ff.pkt_alloc(); ff.frame=ff.frame_alloc(); if(!ff.pkt || !ff.frame) return 0;
  ff.hw_create=dlsym(hu,"av_hwdevice_ctx_create"); ff.hw_transfer=dlsym(hu,"av_hwframe_transfer_data"); ff.buf_ref=dlsym(hu,"av_buffer_ref"); ff.frame_unref=dlsym(hu,"av_frame_unref");
  if(!(want && !strcmp(want,"ffmpeg-sw")) && ff.hw_create && ff.hw_transfer && ff.buf_ref && ff.frame_unref && (ff.sw=ff.frame_alloc())){
    /* the primary GPU's render node (boot_vga), else the default one */
    char node[64]=""; for(int i=128;i<136 && !node[0];i++){ char p[96]; snprintf(p,sizeof p,"/sys/class/drm/renderD%d/device/boot_vga",i); FILE* f=fopen(p,"r");
      if(f){ int v=fgetc(f); fclose(f); if(v=='1') snprintf(node,sizeof node,"/dev/dri/renderD%d",i); } }
    if(ff.hw_create(&ff.hwdev,AV_HWDEVICE_TYPE_VAAPI,node[0]? node : NULL,NULL,0)<0) ff.hwdev=NULL; }
  LOG("video: H.264 decoder ready (FFmpeg %s, %s)\n",names[0],ff.hwdev? "decoded by the GPU's video decoder (VA-API)" : "NVDEC jobs are decoded on the CPU"); g_ff=1; return 1; }
static enum AVPixelFormat ff_get_format(AVCodecContext* c, const enum AVPixelFormat* f){ (void)c;
  const enum AVPixelFormat* p=f; for(;*p!=AV_PIX_FMT_NONE;p++) if(*p==AV_PIX_FMT_VAAPI) return *p;
  { static int said; if(!said){ said=1; LOG("video: this stream cannot be decoded by the GPU: decoded on the CPU\n"); } }
  return p>f? p[-1] : f[0]; }   /* the software format: last in the list */
static AVCodecContext* ff_ctx(void){
  if(g_ffc[g_stream]) return g_ffc[g_stream];
  AVCodecContext* c=ff.alloc(ff.codec); if(!c) return NULL;
  c->thread_count=1; c->flags|=AV_CODEC_FLAG_LOW_DELAY;
  if(ff.hwdev){ c->hw_device_ctx=ff.buf_ref(ff.hwdev); c->get_format=ff_get_format; }
  { u64 t0=plat_ns(); int r=ff.open(c,ff.codec,NULL); LOG("video: H.264 decoder opened in %.1f ms\n",(plat_ns()-t0)/1e6); if(r<0){ LOG("video: FFmpeg H.264 decoder could not be opened\n"); return NULL; } }
  return g_ffc[g_stream]=c; }
#endif

#if defined(_WIN32) && !defined(SWR_NO_H264)
static int mf_load(void); static int g_mf;
#endif
static int dec_open(void){
#ifdef SWR_H264_FFMPEG
  if(ff_load()) return 1;
#endif
#if defined(_WIN32) && !defined(SWR_NO_H264)
  if(g_mf!=0 && mf_load()) return 1;   /* Media Foundation (mfvideo.inc); 0 after it failed: OpenH264 */
#endif
#ifdef SWR_NO_H264
  if(!g_dec_fail){ g_dec_fail=1; LOG("video: this build has no H.264 decoder (the game's videos stay black)\n"); } return 0;
#else
  if(g_dec) return 1; if(g_dec_fail) return 0;
#ifdef SWR_H264_DLOPEN
  if(!h264_load()){ g_dec_fail=1; return 0; }
#endif
  if(WelsCreateDecoder(&g_dec) || !g_dec){ g_dec_fail=1; LOG("video: OpenH264 decoder could not be created\n"); return 0; }
  SDecodingParam p; memset(&p,0,sizeof p); p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC; p.eEcActiveIdc=ERROR_CON_SLICE_COPY;
  if((*g_dec)->Initialize(g_dec,&p)){ g_dec_fail=1; LOG("video: OpenH264 decoder initialize failed\n"); return 0; }
  LOG("video: H.264 decoder ready (OpenH264, NVDEC jobs are decoded on the CPU)\n"); return 1;
#endif
}
#ifndef SWR_NO_H264
static void keep_planes(const u8* py, const u8* pu, const u8* pv, int sy, int sc, u32 w, u32 h){
  if(!w || !h || w>4096 || h>4096) return;
  const u8* pl[3]={py,pu,pv};
  u32 need=w*h; if(g_fcap<need){ free(g_fy); free(g_fuv); g_fy=malloc(need); g_fuv=malloc(need/2+w); g_fuv_cap=need/2+w; g_fcap=need; }
  for(u32 y=0;y<h;y++) memcpy(g_fy+(u64)y*w,pl[0]+(u64)y*sy,w);
  for(u32 y=0;y<h/2;y++){ u8* d=g_fuv+(u64)y*w; const u8* u=pl[1]+(u64)y*sc; const u8* v=pl[2]+(u64)y*sc; for(u32 x=0;x<w/2;x++){ d[2*x]=u[x]; d[2*x+1]=v[x]; } }
  g_fw=w; g_fh=h; g_vframes++; }
/* NV12 (a picture from the GPU's decoder): the chroma is already interleaved as g_fuv keeps it */
static void keep_nv12(const u8* py, const u8* puv, int sy, int suv, u32 w, u32 h){
  if(!w || !h || w>4096 || h>4096) return;
  u32 need=w*h; if(g_fcap<need){ free(g_fy); free(g_fuv); g_fy=malloc(need); g_fuv=malloc(need/2+w); g_fuv_cap=need/2+w; g_fcap=need; }
  for(u32 y=0;y<h;y++) memcpy(g_fy+(u64)y*w,py+(u64)y*sy,w);
  for(u32 y=0;y<h/2;y++) memcpy(g_fuv+(u64)y*w,puv+(u64)y*suv,w);
  g_fw=w; g_fh=h; g_vframes++; }
static void keep_frame(unsigned char** pl, SBufferInfo* bi){
  keep_planes(pl[0],pl[1],pl[2],bi->UsrData.sSystemBuffer.iStride[0],bi->UsrData.sSystemBuffer.iStride[1],(u32)bi->UsrData.sSystemBuffer.iWidth,(u32)bi->UsrData.sSystemBuffer.iHeight); }
#endif
#if defined(_WIN32) && !defined(SWR_NO_H264)
#include "mfvideo.inc"
#endif

#ifdef SWR_HAVE_VP9
#define SWR_NATIVE_VIDEO_RETIRE 1
#include "vp9video.inc"
#endif

volatile unsigned long long g_video_last_ns;   /* last video decode (plat_ns): the shader pre-build stays off while a video plays */
static void nvdec_execute(u32* r){
  g_video_last_ns=plat_ns();
  { static int said; if(said<4){ said++; LOG("video: NVDEC job, control params %08x (codec %u)\n",r[0x400/4],r[0x400/4]&0xF); } }
#ifdef SWR_HAVE_VP9
  if(vp9_job(r,r[0x400/4]&0xF)) return;      /* a VP9 frame of a .webm the game read: decoded with libvpx */
#endif
  if(!dec_open()) return;
#ifndef SWR_NO_H264
  u64 ps_iova=(u64)r[0x404/4]<<8, bs_iova=(u64)r[0x408/4]<<8;
  u8* ps=iova_ptr(ps_iova,0x300); if(!ps){ static int said; if(said<4){ said++; LOG("video: NVDEC picture setup at iova %llx not mapped\n",(unsigned long long)ps_iova); } return; }
  u32 slen=rd32(ps,0x48); if(!slen || slen>(16u<<20)) return;
  u8* bs=iova_ptr(bs_iova,slen); if(!bs){ static int said; if(said<4){ said++; LOG("video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n",(unsigned long long)bs_iova,slen); } return; }
  u8 hdr[512]; u32 hn=build_headers(ps,hdr,sizeof hdr);
  static u8* buf; static u32 bcap; u32 need=hn+slen+8; if(bcap<need){ buf=realloc(buf,need); bcap=need; }
  u32 o=0;
  if(hn!=g_last_hdr_n || memcmp(hdr,g_last_hdr,hn)){ memcpy(buf,hdr,hn); o=hn; memcpy(g_last_hdr,hdr,hn); g_last_hdr_n=hn; }
  if(bs[0]==0 && bs[1]==0 && (bs[2]==1 || (bs[2]==0 && bs[3]==1))){ memcpy(buf+o,bs,slen); o+=slen; }
  else {
    /* mp4-style 4 byte BE lengths -> start codes; otherwise just prepend one */
    u32 p=0, ok=1; while(p+4<=slen){ u32 L=((u32)bs[p]<<24)|((u32)bs[p+1]<<16)|((u32)bs[p+2]<<8)|bs[p+3]; if(!L || (u64)p+4+L>slen || (bs[p+4]&0x80)){ ok=0; break; }   /* 64-bit: a huge L must not wrap past the check */ p+=4+L; }
    if(ok && p==slen){ static u8* t; static u32 tc; if(tc<slen+64){ t=realloc(t,slen+64); tc=slen+64; } (void)t;
      if(bcap<o+slen+slen/4+16){ bcap=o+slen+slen/4+16; buf=realloc(buf,bcap); }
      for(p=0;p<slen;){ u32 L=((u32)bs[p]<<24)|((u32)bs[p+1]<<16)|((u32)bs[p+2]<<8)|bs[p+3]; buf[o++]=0; buf[o++]=0; buf[o++]=0; buf[o++]=1; memcpy(buf+o,bs+p+4,L); o+=L; p+=4+L; } }
    else { buf[o++]=0; buf[o++]=0; buf[o++]=1; memcpy(buf+o,bs,slen); o+=slen; } }
  { /* SWR_DUMP_H264=file: the Annex-B stream exactly as the decoder gets it, one file per channel (file.<channel>), for
       offline decoder tests */
    static int dump=-1; static FILE* df[H1_STREAMS]; if(dump<0) dump=getenv("SWR_DUMP_H264")!=NULL;
    if(dump){ if(!df[g_stream]){ char p[600]; snprintf(p,sizeof p,"%s.%d",getenv("SWR_DUMP_H264"),g_stream); df[g_stream]=fopen(p,"wb"); }
      if(df[g_stream]){ fwrite(buf,1,o,df[g_stream]); fflush(df[g_stream]); } } }
  unsigned st=0; int out=0;
#if defined(_WIN32) && !defined(SWR_NO_H264)
  if(g_mf==1 && !mf_dec()){ g_mf=0; LOG("video: no Media Foundation H.264 decoder: OpenH264 instead\n"); if(!dec_open()) return; }
  if(g_mf==1) out=mf_decode(buf,o,&st); else
#endif
#ifdef SWR_H264_FFMPEG
  if(g_ff==1){ AVCodecContext* fc=ff_ctx(); if(!fc) return;
    ff.pkt->data=buf; ff.pkt->size=(int)o;
    if(ff.send(fc,ff.pkt)<0) st=4;   /* reported like OpenH264's dsBitstreamError */
    while(ff.recv(fc,ff.frame)==0){ AVFrame* f=ff.frame;
      if(f->format==AV_PIX_FMT_VAAPI){ if(ff.hw_transfer(ff.sw,f,0)<0){ st=4; continue; }
        AVFrame* s=ff.sw; if(s->format==AV_PIX_FMT_NV12) keep_nv12(s->data[0],s->data[1],s->linesize[0],s->linesize[1],(u32)f->width,(u32)f->height);
        else keep_planes(s->data[0],s->data[1],s->data[2],s->linesize[0],s->linesize[1],(u32)f->width,(u32)f->height);
        ff.frame_unref(ff.sw); }
      else keep_planes(f->data[0],f->data[1],f->data[2],f->linesize[0],f->linesize[1],(u32)f->width,(u32)f->height);
      out=1; }
  } else
#endif
  { unsigned char* pl[3]={0,0,0}; SBufferInfo bi; memset(&bi,0,sizeof bi);
    st=(unsigned)(*g_dec)->DecodeFrameNoDelay(g_dec,buf,(int)o,pl,&bi);
    if(bi.iBufferStatus==1 && pl[0]){ keep_frame(pl,&bi); out=1; } }
  { /* SWR_TRACE_NVDEC_REGS=n: the first n jobs' surface registers (picture index 0x40c, display 0x428, the 17 luma slots) */
    static int left=-1; if(left<0){ const char* e=getenv("SWR_TRACE_NVDEC_REGS"); left= e? atoi(e) : 0; }
    if(left>0){ left--; if(!g_regs_trace) g_regs_trace=fopen("nvdec_regs.txt","w"); FILE* tf=g_regs_trace;
      if(tf){ fprintf(tf,"DEC ch%d index %x display %x slots",g_stream,r[0x40c/4],r[0x428/4]); for(int k=0;k<17;k++) fprintf(tf," %x",r[0x430/4+k]);
        fprintf(tf," ps"); for(int k=0;k<0x300;k+=4) fprintf(tf," %x",rd32(ps,k)); fprintf(tf,"\n"); fflush(tf); } } }
  /* file the picture under the surface NVDEC wrote it to (0x43c), so VIC converts the one it asks for. Without this an
     H.264 VIC job fell back to the last picture decoded on any channel: Metroid Prime 4 decodes the first title-screen
     frames on a second channel while the intro plays, and the intro showed the title logo for a moment */
  if(out){ static char names[H1_STREAMS][16]; static u32 frames[H1_STREAMS]; if(!names[g_stream][0]) snprintf(names[g_stream],16,"h264 ch%d",g_stream);
    /* the current picture goes to PictureLumaOffset[CurrPicIdx] (methods 0x430 + 4*index): CurrPicIdx is bits 2-6 of
       word 0xb4 of the H.264 picture setup. The game rotates its pictures through several surfaces; filing every one
       under slot 3 (0x43c) put most pictures under the wrong surface, so VIC showed older pictures, the other video's
       (the title logo in the intro) or none (static) */
    u32 idx=(rd32(ps,0xb4)>>2)&31; if(idx>16) idx=3;
    g_save_luma=(u64)r[0x430/4+idx]<<8;
    decoded_surface_save(r,(u64)(uintptr_t)names[g_stream],++frames[g_stream],0); g_save_luma=0; g_h264_filed=1; }
  /* nvdec_status_s at method 0x424. players check it, an all-zero status means the frame failed */
  { u64 st_iova=(u64)r[0x424/4]<<8; u8* sp= st_iova? iova_ptr(st_iova,16) : NULL;
    if(sp){ u32 mbs=rd32(ps,0x64)*rd32(ps,0x68); u32 v[4]={ st? 0 : mbs, st? mbs : 0, 100000, st? 1 : 0 }; memcpy(sp,v,16); } }
  { static u32 njobs; njobs++; if(njobs%120==0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n",njobs,g_vframes); }
  { static int said; if(said<3 || (st && said<8)){ said++; LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder state %x, picture %s (%ux%u)\n",rd32(ps,0x64),rd32(ps,0x68),slen,st,out? "out" : "pending",g_fw,g_fh); } }
#endif
}
/* VIC output surface config, u64 at config(0x708)+0x20:
   0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8, 0x23 X8B8G8R8, 0x24 X8R8G8B8, 0x44 NV12), 11-14 bl kind, 15-18 block height log2,
   32-45 width-1, 46-59 height-1 */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);

static u8 clamp8(int v){ return v<0? 0 : v>255? 255 : (u8)v; }

static void vic_rgba_scalar(const u8*Y,const u8*UV,u8*out,u32 start,u32 w,int rgb_order){
  int ri=rgb_order?0:2,bi=rgb_order?2:0;
  for(u32 x=start;x<w;x++){int c=((int)Y[x]-16)*298,d=(int)UV[x&~1u]-128,e=(int)UV[(x&~1u)+1]-128;u8*o=out+x*4;
    o[ri]=clamp8((c+409*e+128)>>8);o[1]=clamp8((c-100*d-208*e+128)>>8);o[bi]=clamp8((c+516*d+128)>>8);o[3]=255;}
}
#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__) && !defined(__TINYC__)
#include <immintrin.h>
#define SWR_VIC_HAS_AVX2 1
__attribute__((target("avx2")))
static void vic_rgba_avx2(const u8*Y,const u8*UV,u8*out,u32 w,int rgb_order){
  const __m128i um=_mm_setr_epi8(0,0,2,2,4,4,6,6,-128,-128,-128,-128,-128,-128,-128,-128);
  const __m128i vm=_mm_setr_epi8(1,1,3,3,5,5,7,7,-128,-128,-128,-128,-128,-128,-128,-128);
  const __m256i zero=_mm256_setzero_si256(),max=_mm256_set1_epi32(255),bias=_mm256_set1_epi32(128);
  u32 x=0;
  for(;x+8<=w;x+=8){
    __m128i uv=_mm_loadl_epi64((const __m128i*)(UV+x));
    __m256i c=_mm256_mullo_epi32(_mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i*)(Y+x))),_mm256_set1_epi32(16)),_mm256_set1_epi32(298));
    __m256i d=_mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv,um)),bias);
    __m256i e=_mm256_sub_epi32(_mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv,vm)),bias);
    __m256i r=_mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(c,_mm256_mullo_epi32(e,_mm256_set1_epi32(409))),bias),8);
    __m256i g=_mm256_srai_epi32(_mm256_add_epi32(_mm256_sub_epi32(_mm256_sub_epi32(c,_mm256_mullo_epi32(d,_mm256_set1_epi32(100))),_mm256_mullo_epi32(e,_mm256_set1_epi32(208))),bias),8);
    __m256i b=_mm256_srai_epi32(_mm256_add_epi32(_mm256_add_epi32(c,_mm256_mullo_epi32(d,_mm256_set1_epi32(516))),bias),8);
    r=_mm256_min_epi32(_mm256_max_epi32(r,zero),max);g=_mm256_min_epi32(_mm256_max_epi32(g,zero),max);b=_mm256_min_epi32(_mm256_max_epi32(b,zero),max);
    __m256i lo=rgb_order?r:b,hi=rgb_order?b:r;
    __m256i pixels=_mm256_or_si256(_mm256_or_si256(lo,_mm256_slli_epi32(g,8)),_mm256_or_si256(_mm256_slli_epi32(hi,16),_mm256_set1_epi32((int)0xff000000u)));
    _mm256_storeu_si256((__m256i*)(out+x*4),pixels);
  }
  vic_rgba_scalar(Y,UV,out,x,w,rgb_order);
}
#endif
static void vic_rgba_row(const u8*Y,const u8*UV,u8*out,u32 w,int rgb_order){
#ifdef SWR_VIC_HAS_AVX2
  static int fast=-1;if(fast<0){extern int g_opt_video_avx2;fast=!getenv("SWR_NO_VIC_AVX2")&&(getenv("SWR_VIC_AVX2")||g_opt_video_avx2)&&__builtin_cpu_supports("avx2");if(fast)LOG("video: AVX2 exact integer RGBA conversion active\n");}
  if(fast){vic_rgba_avx2(Y,UV,out,w,rgb_order);return;}
#endif
  vic_rgba_scalar(Y,UV,out,0,w,rgb_order);
}

static void vic_put(u8* base, u64 off, const u8* src, u32 n, u64 lim){ if(off+n<=lim) memcpy(base+off,src,n); }

static void vic_block_row(u8*base,const u8*src,u32 bytes,u32 y,u32 pitch,u32 height,u32 bh,u64 lim){
  static int fast=-1;if(fast<0){extern int g_opt_video_block_copy;fast=!getenv("SWR_NO_VIC_BLOCK_COPY")&&(getenv("SWR_VIC_BLOCK_COPY")||g_opt_video_block_copy);}
  u32 x=0;
  if(fast){u64 row=bl_offset(0,y,0,pitch,height,bh,0),column_stride=512ull<<bh;
    for(;bytes-x>=64;x+=64){u64 off=row+(u64)(x>>6)*column_stride;
      vic_put(base,off,src+x,16,lim);vic_put(base,off+32,src+x+16,16,lim);
      vic_put(base,off+256,src+x+32,16,lim);vic_put(base,off+288,src+x+48,16,lim);
    }
  }
  for(;x<bytes;x+=16){u32 n=bytes-x<16?bytes-x:16;vic_put(base,bl_offset(x,y,0,pitch,height,bh,0),src+x,n,lim);}
}

/* VIC jobs split their rows over a few threads: a 1920x1080 picture took about 9 ms on one thread, and with the
   decode before it (5-10 ms) a 60 FPS video did not fit in its 16.7 ms, so frames were repeated or skipped (the intro's
   micro-stutter). Rows are independent: each writes its own bytes. The calling thread works too */
typedef void (*VicRowsFn)(void* ctx, u32 y0, u32 y1);
static PMutex g_vp_m; static PCond g_vp_go; static int g_vp_n=-1; static u64 g_vp_gen;
static VicRowsFn g_vp_fn; static void* g_vp_ctx; static u32 g_vp_rows; static volatile u32 g_vp_next, g_vp_done;
#define VP_CHUNK 32
static void vp_work(void){
  for(;;){ u32 y0=__atomic_fetch_add(&g_vp_next,VP_CHUNK,__ATOMIC_ACQ_REL); if(y0>=g_vp_rows) break;
    u32 y1= y0+VP_CHUNK<g_vp_rows? y0+VP_CHUNK : g_vp_rows; g_vp_fn(g_vp_ctx,y0,y1); __atomic_add_fetch(&g_vp_done,y1-y0,__ATOMIC_ACQ_REL); } }
static void vp_thread(void* arg){ (void)arg; plat_diag_role("video-rows"); u64 seen=0;
  for(;;){ pm_lock(&g_vp_m); while(g_vp_gen==seen) pc_wait(&g_vp_go,&g_vp_m,1000000000); seen=g_vp_gen; pm_unlock(&g_vp_m); vp_work(); } }
static void vic_rows(VicRowsFn fn, void* ctx, u32 rows){
  if(g_vp_n<0){ pm_init(&g_vp_m); pc_init(&g_vp_go); g_vp_n=0;
    if(!getenv("SWR_VIC_ONE_THREAD")){ int want=plat_ncpu()/3; if(want>7) want=7; for(int i=0;i<want;i++) if(!plat_thread(vp_thread,NULL,256*1024)) g_vp_n++; }
    LOG("video: VIC pictures converted on %d threads\n",g_vp_n+1); }
  if(!g_vp_n || rows<2*VP_CHUNK){ fn(ctx,0,rows); return; }
  pm_lock(&g_vp_m); g_vp_fn=fn; g_vp_ctx=ctx; g_vp_rows=rows; __atomic_store_n(&g_vp_done,0,__ATOMIC_RELEASE); __atomic_store_n(&g_vp_next,0,__ATOMIC_RELEASE); g_vp_gen++; pc_broadcast(&g_vp_go); pm_unlock(&g_vp_m);
  vp_work();
  while(__atomic_load_n(&g_vp_done,__ATOMIC_ACQUIRE)<rows) plat_yield(); }
typedef struct { const u8 *fy, *fuv; u32 fw, w, pitch, sh, bh, blk; int rgb_order; u8 *py, *pc; u64 lim, clim; } VicJob;
static void vic_rgba_rows(void* c, u32 y0, u32 y1){ VicJob* j=c; u8 row[8192*4];
  for(u32 y=y0;y<y1;y++){ const u8* Y=j->fy+(u64)y*j->fw; const u8* UV=j->fuv+(u64)(y/2)*j->fw;
    vic_rgba_row(Y,UV,row,j->w,j->rgb_order);
    if(!j->blk) vic_put(j->py,(u64)y*j->pitch,row,j->w*4,j->lim); else vic_block_row(j->py,row,j->w*4,y,j->pitch,j->sh,j->bh,j->lim); } }
static void vic_luma_rows(void* c, u32 y0, u32 y1){ VicJob* j=c;
  for(u32 y=y0;y<y1;y++){ const u8* s=j->fy+(u64)y*j->fw; if(!j->blk) vic_put(j->py,(u64)y*j->pitch,s,j->w,j->lim); else vic_block_row(j->py,s,j->w,y,j->pitch,j->sh,j->bh,j->lim); } }
static void vic_chroma_rows(void* c, u32 y0, u32 y1){ VicJob* j=c;
  for(u32 y=y0;y<y1;y++){ const u8* s=j->fuv+(u64)y*j->fw; if(!j->blk) vic_put(j->pc,(u64)y*j->pitch,s,j->w,j->clim); else vic_block_row(j->pc,s,j->w,y,j->pitch,(j->sh+1)/2,j->bh,j->clim); } }

/* a black picture (video-range Y 16, UV 128) for a VIC job whose picture isn't decoded yet: leaving the output
   surface as it was showed its uninitialised memory, a rainbow-static flash as the intro movie started */
static u8 *g_black_y, *g_black_uv; static u32 g_black_w, g_black_h;
static void black_frame(u32 w, u32 h){
  if(g_black_y && g_black_w==w && g_black_h==h) return;
  free(g_black_y); free(g_black_uv); g_black_y=malloc((size_t)w*h); g_black_uv=malloc((size_t)w*((h+1)/2)+w);
  if(g_black_y) memset(g_black_y,16,(size_t)w*h); if(g_black_uv) memset(g_black_uv,128,(size_t)w*((h+1)/2)+w); g_black_w=w; g_black_h=h; }
static void vic_execute(u32* r){
  int black=0;
  if(!g_vframes){ black_frame(1920,1088); if(!g_black_y || !g_black_uv) return; black=1; }
  u8*fy= black? g_black_y : g_fy,*fuv= black? g_black_uv : g_fuv;u32 fw= black? 1920 : g_fw,fh= black? 1088 : g_fh;
  DecodedSurface*picture=decoded_surface_find((u64)r[0x400/4]<<8);
  { static int left=-1; if(left<0){ const char* e=getenv("SWR_TRACE_NVDEC_REGS"); left= e? atoi(e) : 0; }
    if(left>0){ left--; if(!g_regs_trace) g_regs_trace=fopen("nvdec_regs.txt","w"); FILE* tf=g_regs_trace; if(tf){ fprintf(tf,"VIC %x frame %d owner %s\n",r[0x400/4],picture? (int)picture->frame : -1,picture&&picture->owner? (const char*)(uintptr_t)picture->owner : "-"); fflush(tf); } } }
  if(!picture){ static u32 miss; int strict=g_h264_filed;
    if(++miss<=4 || miss%600==0) LOG("video: VIC input surface %llx has no decoded picture (%u so far): %s\n",(unsigned long long)((u64)r[0x400/4]<<8),miss,strict? "output left as it was" : "nearest match used");
    /* with H.264 every picture is filed by its surface, so a miss is a surface not decoded yet (a video starting).
       The fallbacks below (newest picture of the same size, else the last one decoded on any channel) then showed a
       picture of another video: Metroid Prime 4's title screen flashed in its intro. They stay for VP9 */
    if(strict){ black_frame(fw? fw : 1920,fh? fh : 1088); if(!g_black_y || !g_black_uv) return; fy=g_black_y; fuv=g_black_uv; if(!fw){ fw=1920; fh=1088; } } }
  if(picture){fy=picture->y;fuv=picture->uv;fw=picture->w;fh=picture->h;}
  { static u32 nv; nv++; if(nv%120==0) LOG("video: %u VIC jobs so far\n",nv); }
  u64 ly=(u64)r[0x720/4]<<8, lc=(u64)r[0x724/4]<<8, cfg_iova=(u64)r[0x708/4]<<8;
  u8* cs=iova_ptr(cfg_iova,0x28); u64 cfg=0; if(cs) memcpy(&cfg,cs+0x20,8);
  if(!picture && fy!=g_black_y){u8*sc=iova_ptr(cfg_iova,0xe0);if(sc){u64 sf=0;memcpy(&sf,sc+0xd0,8);u32 iw=(u32)((sf>>32)&0x3fff)+1,ih=(u32)((sf>>46)&0x3fff)+1;for(int i=0;i<32;i++){DecodedSurface*d=&decoded_surfaces[i];if(d->used&&d->w==iw&&d->h==ih&&(!picture||d->used>picture->used))picture=d;}if(picture){fy=picture->y;fuv=picture->uv;fw=picture->w;fh=picture->h;}}}
  u32 fmt=(u32)(cfg&0x7F), blk=(u32)((cfg>>11)&15), bh=(u32)((cfg>>15)&15);
  u32 sw=(u32)((cfg>>32)&0x3FFF)+1, sh=(u32)((cfg>>46)&0x3FFF)+1;
  if(!cs){ fmt=0x44; sw=fw; sh=fh; }
  {static int checked;static FILE*csv;if(!checked){checked=1;const char*e=getenv("SWR_VIC_JOB_CSV");if(e&&*e){csv=fopen(e,"w");if(csv)fprintf(csv,"ns,input,selected_owner,selected_frame,selected_used,exact_owner,exact_frame,exact_used,width,height\n");}}
    if(csv){u64 input=(u64)r[0x400/4]<<8;DecodedSurface*exact=NULL;for(int i=0;i<32;i++)if(decoded_surfaces[i].used&&decoded_surfaces[i].luma==input)exact=&decoded_surfaces[i];
      fprintf(csv,"%llu,%llu,%s,%d,%llu,%s,%d,%llu,%u,%u\n",(unsigned long long)plat_ns(),(unsigned long long)input,picture&&picture->owner?(const char*)(uintptr_t)picture->owner:"",picture?(int)picture->frame:-1,(unsigned long long)(picture?picture->used:0),exact&&exact->owner?(const char*)(uintptr_t)exact->owner:"",exact?(int)exact->frame:-1,(unsigned long long)(exact?exact->used:0),fw,fh);fflush(csv);}}

  if(getenv("SWR_VIC_FMT")) fmt=(u32)strtoul(getenv("SWR_VIC_FMT"),0,0);
  u32 w=fw<sw? fw : sw, h=fh<sh? fh : sh;
  if(getenv("SWR_TRACE_VIC")){static u64 seen[128];static u32 ns;u8*dst=iova_ptr(ly,1);u64 key=ly^cfg;int k;for(k=0;k<ns;k++)if(seen[k]==key)break;
    if(k==ns&&ns<128){seen[ns++]=key;u8*all=iova_ptr(cfg_iova,0x610);u32 enabled=0;char slots[700]={0};int used=0;
      if(all)for(u32 slot=0;slot<8;slot++){u64 sc=0,sf=0;memcpy(&sc,all+0x90+slot*0xb0,8);memcpy(&sf,all+0xd0+slot*0xb0,8);if(sc&1){enabled++;used+=snprintf(slots+used,sizeof slots-used," slot%u fmt=%x input=%x",slot,(u32)(sf&127),r[0x400/4+slot*24]);}}
      LOG("vic trace: output guest=%llx iova=%llx fmt=%x %ux%u movie=%ux%u enabled=%u source_bound=%u%s\n",(unsigned long long)(dst?(u64)(dst-GP(0)):0),(unsigned long long)ly,fmt,sw,sh,fw,fh,enabled,picture!=NULL,slots);
    }}

  { static int said; if(said<3){ said++; LOG("video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u, picture %ux%u\n",(unsigned long long)cfg,fmt,blk? "block linear" : "pitch linear",1u<<bh,sw,sh,fw,fh); } }
  if(fmt==0x1F || fmt==0x20 || fmt==0x23 || fmt==0x24){
    u32 pitch=sw*4; u64 lim= blk? (u64)pitch*(((sh+(8u<<bh)-1)/(8u<<bh))*(8u<<bh)) : (u64)pitch*sh;
    u8* py=iova_ptr(ly,lim); if(!py){ py=iova_ptr(ly,(u64)pitch*h); lim=(u64)pitch*h; }
    if(!py){ static int said; if(said<4){ said++; LOG("video: VIC output at iova %llx not mapped\n",(unsigned long long)ly); } return; }
    if(w>8192) w=8192;
    VicJob j={fy,fuv,fw,w,pitch,sh,bh,blk,(fmt==0x1F || fmt==0x23),py,NULL,lim,0}; vic_rows(vic_rgba_rows,&j,h);
    { static int said; if(said<2){ said++; LOG("video: VIC wrote a %ux%u RGBA picture at iova %llx\n",w,h,(unsigned long long)ly); } }
    video_written(py,lim);
    return; }
  /* NV12. pitch linear rows are 256 aligned */
  u32 pitch= blk? sw : (sw+255)&~255u;
  u64 ylim=(u64)pitch*sh, clim=(u64)pitch*((sh+1)/2);
  if(blk){ u32 gy=8u<<bh; ylim=(u64)pitch*((sh+gy-1)/gy*gy); clim=(u64)pitch*(((sh+1)/2+gy-1)/gy*gy); }
  u8* py=iova_ptr(ly,ylim); u8* pc= lc? iova_ptr(lc,clim) : NULL;
  if(!py){ static int said; if(said<4){ said++; LOG("video: VIC output luma at iova %llx not mapped\n",(unsigned long long)ly); } return; }
  { VicJob j={fy,fuv,fw,w,pitch,sh,bh,blk,0,py,pc,ylim,clim}; vic_rows(vic_luma_rows,&j,h); if(pc) vic_rows(vic_chroma_rows,&j,h/2); }
  video_written(py,ylim); if(pc) video_written(pc,clim);
  { static int said; if(said<2){ said++; LOG("video: VIC wrote a %ux%u NV12 picture (luma iova %llx, chroma %llx, pitch %u)\n",w,h,(unsigned long long)ly,(unsigned long long)lc,pitch); } } }

/* ------------------------------------------------------------ host1x command stream */
typedef struct { int is_vic; u32 syncpt; u32 regs[0x1000/4]; u32 method; int stream; } H1Ch;

static void dev_method(H1Ch* ch, u32 m, u32 v){
  u32 off=m<<2; if(off<0x1000) ch->regs[off/4]=v;
  if(off==0x300){
    /* SWR_VIDEO_TIMING=1: average and worst time per NVDEC (decode) and VIC (convert) job, every 120 jobs of each */
    static int timing=-1; if(timing<0) timing=!getenv("SWR_NO_VIDEO_TIMING");
    u64 t0= timing? plat_ns() : 0;
    if(ch->is_vic) vic_execute(ch->regs); else { g_stream=ch->stream; nvdec_execute(ch->regs); }
    { /* SWR_VIDEO_SEQ=a,b: every job between a and b seconds after start: kind, start and end in ms */
      static int seq=-1; static double sa, sb; static u64 base; if(seq<0){ const char* e=getenv("SWR_VIDEO_SEQ"); seq= e && sscanf(e,"%lf,%lf",&sa,&sb)==2; base=plat_ns(); }
      if(seq){ double ts=(t0? t0 : plat_ns())/1e6-base/1e6, te=(plat_ns()-base)/1e6; if(ts>=sa*1000 && ts<=sb*1000) LOG("video seq: %s %.2f-%.2f\n",ch->is_vic? "VIC" : "DEC",ts,te); } }
    if(timing && ch->is_vic){ /* how evenly new video pictures arrive: a 60 FPS video wants one every 16.7 ms */
      static u64 last, isum, imx; static u32 in, ilate; u64 now=plat_ns();
      if(last && now-last<500000000ull){ u64 dt=now-last; isum+=dt; if(dt>imx) imx=dt; if(dt>20000000ull) ilate++;
        if(++in==120){ LOG("video timing: new picture every %.2f ms on average, %.2f ms worst, %u of 120 over 20 ms\n",isum/120e6,imx/1e6,ilate); isum=imx=0; in=ilate=0; } }
      last=now; }
    if(timing){ static u64 sum[2], mx[2]; static u32 n[2]; int k=ch->is_vic; u64 dt=plat_ns()-t0; sum[k]+=dt; if(dt>mx[k]) mx[k]=dt;
      static u32 per[H1_STREAMS]; if(!k) per[ch->stream]++;
      if(++n[k]==120){ char by[200]=""; int o=0; if(!k) for(int s=0;s<H1_STREAMS;s++) if(per[s]){ o+=snprintf(by+o,sizeof by-o," ch%d:%u",s,per[s]); per[s]=0; }
        LOG("video timing: %s %.2f ms average, %.2f ms worst (120 jobs)%s\n",k? "VIC convert" : "NVDEC decode",sum[k]/120e6,mx[k]/1e6,by); sum[k]=mx[k]=0; n[k]=0; } } } }

static void reg_write(H1Ch* ch, u32 cls, u32 off, u32 v){
  if(cls==0x1) return;                       /* host1x class itself, incrs are already counted from the submit */
  if(off==0x10) ch->method=v; else if(off==0x11) dev_method(ch,ch->method,v); }

static void run_words(H1Ch* ch, const u32* w, u32 n){
  u32 cls=ch->is_vic? 0x5D : 0xF0;
  for(u32 i=0;i<n;){
    u32 x=w[i++], op=x>>28;
    switch(op){
      case 0: { cls=(x>>6)&0x3FF; u32 off=(x>>16)&0xFFF, mask=x&0x3F; for(int b=0;b<6;b++) if(mask&(1u<<b)){ if(i>=n) return; reg_write(ch,cls,off+b,w[i++]); } break; }
      case 1: { u32 off=(x>>16)&0xFFF, c=x&0xFFFF; for(u32 k=0;k<c && i<n;k++) reg_write(ch,cls,off+k,w[i++]); break; }
      case 2: { u32 off=(x>>16)&0xFFF, c=x&0xFFFF; for(u32 k=0;k<c && i<n;k++) reg_write(ch,cls,off,w[i++]); break; }
      case 3: { u32 off=(x>>16)&0xFFF, mask=x&0xFFFF; for(int b=0;b<16;b++) if(mask&(1u<<b)){ if(i>=n) return; reg_write(ch,cls,off+b,w[i++]); } break; }
      case 4: reg_write(ch,cls,(x>>16)&0xFFF,x&0xFFFF); break;
      default: { static int said; if(said<6){ said++; LOG("video: host1x opcode %u not handled (word %08x)\n",op,x); } return; }
    } } }

/* ------------------------------------------------------------ channel ioctls */
/* a channel per opened /dev/nvhost-nvdec or -vic (chan = the file descriptor), each with its own registers and syncpoint */
static H1Ch g_ch[2][H1_STREAMS]; static u32 g_ch_syncpt_next=200;

u32 host1x_ioctl(int is_vic, u32 chan, u32 cmd, u8* d, u32 n){
  h_init(); pm_lock(&g_hlk);
  H1Ch* ch=&g_ch[is_vic? 1 : 0][chan%H1_STREAMS]; ch->is_vic=is_vic; ch->stream=(int)(chan%H1_STREAMS); if(!ch->syncpt) ch->syncpt=g_ch_syncpt_next++;
  u32 r=0, grp=(cmd>>8)&0xFF, nr=cmd&0xFF;
  if(grp==0x48){ /* SET_NVMAP_FD etc */ }
  else if(grp==0) switch(nr){
    case 0x01: { /* SUBMIT: counts, cmdbufs {memid, offset, words}, relocs, reloc shifts, syncpt incrs {id, n, ?x3}, fences {id, value} */
      u32 ncb=rd32(d,0), nrel=rd32(d,4), nsp=rd32(d,8), nfe=rd32(d,12); u64 o=16;   /* 64-bit: guest counts must not wrap past the size check */
      u32 cbo=(u32)o; o+=(u64)ncb*12+(u64)nrel*20; u32 spo=(u32)o; o+=(u64)nsp*20; u32 feo=(u32)o;
      if(o>n){ r=1; break; }
      for(u32 i=0;i<ncb;i++){ u32 mem=rd32(d,cbo+i*12), off=rd32(d,cbo+i*12+4), wc=rd32(d,cbo+i*12+8);
        u64 a=nvmap_addr(mem); if(!a) a=nvmap_addr_by_id(mem); if(!a || !wc || wc>0x100000) continue;
        run_words(ch,(const u32*)GP(a+off),wc); }
      for(u32 i=0;i<nsp;i++){ u32 id=rd32(d,spo+i*20), inc=rd32(d,spo+i*20+4); if(!id) id=ch->syncpt;
        for(u32 k=0;k<inc && k<64;k++) syncpt_incr(id);
        if(i<nfe && feo+i*4+4<=n){ u32 v=syncpt_read(id); memcpy(d+feo+i*4,&v,4); } }   /* fence thresholds, u32 each */
      { static int said; if(said<3){ said++; LOG("video: %s submit: %u command buffers, %u relocs, %u syncpoint increments, %u fences (%u bytes)\n",is_vic? "vic" : "nvdec",ncb,nrel,nsp,nfe,n); } }
      break; }
    case 0x02: { u32 v=ch->syncpt; memcpy(d+4,&v,4); break; }          /* GET_SYNCPOINT */
    case 0x03: { u32 v=0; memcpy(d+4,&v,4); break; }                    /* GET_WAITBASE */
    case 0x07: break;                                                   /* SET_SUBMIT_TIMEOUT */
    case 0x09: { u32 ne=rd32(d,0); for(u32 i=0;i<ne && 12+i*8+8<=n;i++){ u32 h=rd32(d,12+i*8); u32 io=iova_map(h); memcpy(d+12+i*8+4,&io,4); } break; }   /* MAP_BUFFER */
    case 0x0A: {u32 ne=rd32(d,0);for(u32 i=0;i<ne&&12+i*8+8<=n;i++)iova_unmap(rd32(d,12+i*8));break;} /* UNMAP_BUFFER */
    default: { static int said; if(said<8){ said++; LOG("video: %s ioctl %08x not handled\n",is_vic? "vic" : "nvdec",cmd); } break; }
  }
  else { static int said; if(said<8){ said++; LOG("video: %s ioctl %08x not handled\n",is_vic? "vic" : "nvdec",cmd); } }
  pm_unlock(&g_hlk); return r; }

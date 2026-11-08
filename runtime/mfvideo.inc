/* H.264 through Windows Media Foundation (host1x.c, Windows): Microsoft's decoder MFT on the GPU's video decoder (DXVA
   through Direct3D 11), else in software. OpenH264 on the CPU took 7-8 ms a picture and up to 69 ms on keyframes, so
   the intro movie (1080p60) came late about once in six pictures and the game's player skipped frames to stay with
   the music: the intro felt rushed and cut short. The GPU decodes a picture in about 1-3 ms.
   Low-latency mode gives a picture for every job, as NVDEC does (the game's streams have no B-frames).
   One decoder per channel (g_stream), like OpenH264's. SWR_H264_DECODER=openh264: OpenH264 instead. */
#define COBJMACROS
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d11.h>
#include <strmif.h>
#include <codecapi.h>

static const GUID mfv_CLSID_H264Dec={0x62CE7E72,0x4C71,0x4D20,{0xB1,0x5D,0x45,0x28,0x31,0xA8,0x7D,0x9D}};   /* CLSID_CMSH264DecoderMFT */
static const GUID mfv_LowLatency={0x9c27891a,0xed7a,0x40e1,{0x88,0xe8,0xb2,0x27,0x27,0xa0,0x24,0xee}};     /* CODECAPI_AVLowLatencyMode */
static const GUID mfv_IID_ICodecAPI={0x901db4c7,0x31ce,0x41a2,{0x85,0xdc,0x8f,0xa0,0xbf,0x41,0xb8,0xda}};
static const GUID mfv_D3D11Aware={0x206b4fc8,0xfcf9,0x4c51,{0xaf,0xe3,0x97,0x64,0x36,0x9e,0x33,0xa0}};     /* MF_SA_D3D11_AWARE */

typedef struct { IMFTransform* t; int dxva, provides, typed; u32 w, h, stride; ID3D11Texture2D* stg; u32 sw, sh; } MfDec;
static MfDec g_mfd[H1_STREAMS];
static int g_mf=-1;   /* -1 not tried, 0 unavailable, 1 in use */
static ID3D11Device* g_mf_dev; static ID3D11DeviceContext* g_mf_ctx; static IMFDXGIDeviceManager* g_mf_dm; static UINT g_mf_token;
static int g_mf_hw;   /* the D3D11 device for DXVA exists */

static void mf_com(void){ static __thread int done; if(!done){ done=1; CoInitializeEx(NULL,COINIT_MULTITHREADED); } }

static int mf_load(void){
  if(g_mf>=0) return g_mf; g_mf=0;
  const char* want=getenv("SWR_H264_DECODER"); if(want && strcmp(want,"mf") && strcmp(want,"mf-sw")) return 0;
  mf_com();
  if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){ LOG("video: Media Foundation could not start: OpenH264 instead\n"); return 0; }
  if(!(want && !strcmp(want,"mf-sw"))){
    static const D3D_FEATURE_LEVEL fl[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if(SUCCEEDED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,fl,2,D3D11_SDK_VERSION,&g_mf_dev,NULL,&g_mf_ctx))){
      ID3D10Multithread* mt=NULL; if(SUCCEEDED(ID3D11Device_QueryInterface(g_mf_dev,&IID_ID3D10Multithread,(void**)&mt)) && mt){ ID3D10Multithread_SetMultithreadProtected(mt,TRUE); ID3D10Multithread_Release(mt); }
      if(SUCCEEDED(MFCreateDXGIDeviceManager(&g_mf_token,&g_mf_dm)) && SUCCEEDED(IMFDXGIDeviceManager_ResetDevice(g_mf_dm,(IUnknown*)g_mf_dev,g_mf_token))) g_mf_hw=1; } }
  g_mf=1; LOG("video: H.264 decoder: Windows Media Foundation (%s)\n",g_mf_hw? "the GPU's video decoder through Direct3D 11 when the stream allows" : "software, no Direct3D 11 video device");
  return 1; }

/* NV12 output type; frame size and stride from it */
static int mf_set_output(MfDec* d){
  for(DWORD i=0;;i++){ IMFMediaType* mt=NULL; if(FAILED(IMFTransform_GetOutputAvailableType(d->t,0,i,&mt)) || !mt) return 0;
    GUID sub; int ok= SUCCEEDED(IMFMediaType_GetGUID(mt,&MF_MT_SUBTYPE,&sub)) && IsEqualGUID(&sub,&MFVideoFormat_NV12);
    if(ok && SUCCEEDED(IMFTransform_SetOutputType(d->t,0,mt,0))){
      UINT64 fs=0; IMFMediaType_GetUINT64(mt,&MF_MT_FRAME_SIZE,&fs); d->w=(u32)(fs>>32); d->h=(u32)fs;
      UINT32 st=0; d->stride= SUCCEEDED(IMFMediaType_GetUINT32(mt,&MF_MT_DEFAULT_STRIDE,&st)) && (i32)st>0? st : d->w;
      MFT_OUTPUT_STREAM_INFO si; memset(&si,0,sizeof si); IMFTransform_GetOutputStreamInfo(d->t,0,&si);
      d->provides= (si.dwFlags&(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES|MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))!=0;
      IMFMediaType_Release(mt); d->typed=1; return 1; }
    IMFMediaType_Release(mt); } }

static MfDec* mf_dec(void){
  MfDec* d=&g_mfd[g_stream]; if(d->t) return d;
  mf_com();
  if(FAILED(CoCreateInstance(&mfv_CLSID_H264Dec,NULL,CLSCTX_INPROC_SERVER,&IID_IMFTransform,(void**)&d->t)) || !d->t){ d->t=NULL; return NULL; }
  { ICodecAPI* ca=NULL; if(SUCCEEDED(IMFTransform_QueryInterface(d->t,&mfv_IID_ICodecAPI,(void**)&ca)) && ca){ VARIANT v; VariantInit(&v); v.vt=VT_UI4; v.ulVal=1; ICodecAPI_SetValue(ca,&mfv_LowLatency,&v); ICodecAPI_Release(ca); } }
  if(g_mf_hw){ IMFAttributes* at=NULL; UINT32 aware=0;
    if(SUCCEEDED(IMFTransform_GetAttributes(d->t,&at)) && at){ IMFAttributes_GetUINT32(at,&mfv_D3D11Aware,&aware); IMFAttributes_Release(at); }
    if(aware && SUCCEEDED(IMFTransform_ProcessMessage(d->t,MFT_MESSAGE_SET_D3D_MANAGER,(ULONG_PTR)g_mf_dm))) d->dxva=1; }
  IMFMediaType* in=NULL; MFCreateMediaType(&in);
  IMFMediaType_SetGUID(in,&MF_MT_MAJOR_TYPE,&MFMediaType_Video); IMFMediaType_SetGUID(in,&MF_MT_SUBTYPE,&MFVideoFormat_H264);
  HRESULT hr=IMFTransform_SetInputType(d->t,0,in,0); IMFMediaType_Release(in);
  if(FAILED(hr)){ LOG("video: Media Foundation would not take H.264 input (%08lx)\n",(unsigned long)hr); IMFTransform_Release(d->t); d->t=NULL; return NULL; }
  mf_set_output(d);
  IMFTransform_ProcessMessage(d->t,MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0); IMFTransform_ProcessMessage(d->t,MFT_MESSAGE_NOTIFY_START_OF_STREAM,0);
  LOG("video: Media Foundation H.264 decoder for channel %d (%s)\n",g_stream,d->dxva? "GPU" : "software");
  return d; }

/* one output picture into g_fy/g_fuv; 1 if one came out */
static int mf_take(MfDec* d, IMFSample* s){
  IMFMediaBuffer* mb=NULL; if(FAILED(IMFSample_GetBufferByIndex(s,0,&mb)) || !mb) return 0;
  int ok=0;
  IMFDXGIBuffer* db=NULL;
  if(d->dxva && SUCCEEDED(IMFMediaBuffer_QueryInterface(mb,&IID_IMFDXGIBuffer,(void**)&db)) && db){
    ID3D11Texture2D* tex=NULL; UINT sub=0;
    if(SUCCEEDED(IMFDXGIBuffer_GetResource(db,&IID_ID3D11Texture2D,(void**)&tex)) && tex && SUCCEEDED(IMFDXGIBuffer_GetSubresourceIndex(db,&sub))){
      D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(tex,&td);
      if(!d->stg || d->sw!=td.Width || d->sh!=td.Height){ if(d->stg) ID3D11Texture2D_Release(d->stg); d->stg=NULL;
        D3D11_TEXTURE2D_DESC sd=td; sd.MipLevels=1; sd.ArraySize=1; sd.Usage=D3D11_USAGE_STAGING; sd.BindFlags=0; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ; sd.MiscFlags=0; sd.SampleDesc.Count=1; sd.SampleDesc.Quality=0;
        if(SUCCEEDED(ID3D11Device_CreateTexture2D(g_mf_dev,&sd,NULL,&d->stg))){ d->sw=td.Width; d->sh=td.Height; } }
      if(d->stg){ ID3D11DeviceContext_CopySubresourceRegion(g_mf_ctx,(ID3D11Resource*)d->stg,0,0,0,0,(ID3D11Resource*)tex,sub,NULL);
        D3D11_MAPPED_SUBRESOURCE m; if(SUCCEEDED(ID3D11DeviceContext_Map(g_mf_ctx,(ID3D11Resource*)d->stg,0,D3D11_MAP_READ,0,&m))){
          u32 w= d->w? d->w : td.Width, h= d->h? d->h : td.Height;
          const u8* y=(const u8*)m.pData; const u8* uv=y+(size_t)m.RowPitch*td.Height;
          keep_nv12(y,uv,(int)m.RowPitch,(int)m.RowPitch,w,h); ok=1;
          ID3D11DeviceContext_Unmap(g_mf_ctx,(ID3D11Resource*)d->stg,0); } }
      ID3D11Texture2D_Release(tex); }
    else if(tex) ID3D11Texture2D_Release(tex);
    IMFDXGIBuffer_Release(db); }
  else {
    IMF2DBuffer* b2=NULL; BYTE* p=NULL; LONG pitch=0;
    if(SUCCEEDED(IMFMediaBuffer_QueryInterface(mb,&IID_IMF2DBuffer,(void**)&b2)) && b2 && SUCCEEDED(IMF2DBuffer_Lock2D(b2,&p,&pitch))){
      u32 hh=d->h; keep_nv12(p,p+(size_t)pitch*hh,(int)pitch,(int)pitch,d->w,d->h); ok=1; IMF2DBuffer_Unlock2D(b2); }
    else { DWORD len=0; if(SUCCEEDED(IMFMediaBuffer_Lock(mb,&p,NULL,&len))){ u32 st=d->stride; keep_nv12(p,p+(size_t)st*d->h,(int)st,(int)st,d->w,d->h); ok=1; IMFMediaBuffer_Unlock(mb); } }
    if(b2) IMF2DBuffer_Release(b2); }
  IMFMediaBuffer_Release(mb);
  return ok; }

/* outputs ready now: each one kept (the last wins, as with the other decoders); returns how many */
static int mf_drain(MfDec* d, unsigned* st){
  int n=0;
  for(int guard=0;guard<8;guard++){
    MFT_OUTPUT_DATA_BUFFER ob; memset(&ob,0,sizeof ob); DWORD status=0; IMFSample* own=NULL;
    if(!d->provides){ MFT_OUTPUT_STREAM_INFO si; memset(&si,0,sizeof si); IMFTransform_GetOutputStreamInfo(d->t,0,&si);
      IMFMediaBuffer* b=NULL; DWORD sz= si.cbSize? si.cbSize : d->stride*d->h*3/2+4096;
      if(FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(sz,&b))){ if(own) IMFSample_Release(own); return n; }
      IMFSample_AddBuffer(own,b); IMFMediaBuffer_Release(b); ob.pSample=own; }
    HRESULT hr=IMFTransform_ProcessOutput(d->t,0,1,&ob,&status);
    if(hr==MF_E_TRANSFORM_STREAM_CHANGE){ if(own) IMFSample_Release(own); if(ob.pEvents) IMFCollection_Release(ob.pEvents); mf_set_output(d); continue; }
    if(SUCCEEDED(hr) && ob.pSample){ if(mf_take(d,ob.pSample)) n++; }
    else if(hr!=MF_E_TRANSFORM_NEED_MORE_INPUT) *st=4;
    if(ob.pSample && ob.pSample!=own) IMFSample_Release(ob.pSample);
    if(own) IMFSample_Release(own);
    if(ob.pEvents) IMFCollection_Release(ob.pEvents);
    if(FAILED(hr)) break; }
  return n; }

/* the job's Annex-B packet; 1 when a picture came out (in g_fy/g_fuv) */
static int mf_decode(const u8* data, u32 n, unsigned* st){
  MfDec* d=mf_dec(); if(!d){ *st=4; return 0; }
  IMFSample* s=NULL; IMFMediaBuffer* b=NULL; BYTE* p=NULL;
  if(FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer(n,&b))){ if(s) IMFSample_Release(s); *st=4; return 0; }
  if(SUCCEEDED(IMFMediaBuffer_Lock(b,&p,NULL,NULL))){ memcpy(p,data,n); IMFMediaBuffer_Unlock(b); IMFMediaBuffer_SetCurrentLength(b,n); }
  IMFSample_AddBuffer(s,b); IMFMediaBuffer_Release(b);
  static __thread LONGLONG ts; IMFSample_SetSampleTime(s,ts); IMFSample_SetSampleDuration(s,166667); ts+=166667;
  int out=0;
  HRESULT hr=IMFTransform_ProcessInput(d->t,0,s,0);
  if(hr==MF_E_NOTACCEPTING){ out+=mf_drain(d,st); hr=IMFTransform_ProcessInput(d->t,0,s,0); }
  IMFSample_Release(s);
  if(FAILED(hr)){ *st=4; return out>0; }
  out+=mf_drain(d,st);
  return out>0; }

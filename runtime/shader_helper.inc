/* ---------------------------------------------------------------- shader helper process (NVIDIA, 2026-11-08)
   Included by backend_win32.c. While NVIDIA's driver compiles a shader it blocks the game's GL thread on unrelated
   calls: fences, "is it done?" checks, glCreateShader, 100-800 ms at a time (about 15 s in an hour of play, the door and
   map hitches). So new shaders are compiled in a second copy of this EXE with its own hidden GL context: the game writes
   the GLSL to shader_cache/jobs/<key>.job, the helper builds it and saves the program binary under the game's cache
   name, and the game loads that (gpu_gl.c sh_sep_ready, sps 6). Same GPU, driver and context type, so the binary
   loads; anything that goes wrong falls back to compiling in the game. SWR_NO_SHADER_HELPER=1: off */
typedef unsigned (APIENTRY *HP_CS)(unsigned); typedef void (APIENTRY *HP_SS)(unsigned,int,const char* const*,const int*);
typedef void (APIENTRY *HP_U)(unsigned); typedef unsigned (APIENTRY *HP_CP)(void); typedef void (APIENTRY *HP_AS)(unsigned,unsigned);
typedef void (APIENTRY *HP_PPI)(unsigned,unsigned,int); typedef void (APIENTRY *HP_GPIV)(unsigned,unsigned,int*);
typedef void (APIENTRY *HP_GPB)(unsigned,int,int*,unsigned*,void*); typedef void (APIENTRY *HP_GPIL)(unsigned,int,int*,char*);
typedef const unsigned char* (APIENTRY *HP_GS)(unsigned);
#define HELPER_COUNT 3
static HANDLE g_helper_proc2[HELPER_COUNT], g_helper_job;

static int hp_read(const char* path, unsigned char** out, size_t* n){
  FILE* f=fopen(path,"rb"); if(!f) return 0; fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  if(sz<=0 || sz>(16<<20)){ fclose(f); return 0; } unsigned char* b=malloc((size_t)sz+1); if(!b){ fclose(f); return 0; }
  size_t got=fread(b,1,(size_t)sz,f); fclose(f); if(got!=(size_t)sz){ free(b); return 0; } b[sz]=0; *out=b; *n=(size_t)sz; return 1; }

int shader_helper_main(int argc, char** argv){
  if(argc<4) return 2;
  const char* tag=argv[2]; DWORD ppid=(DWORD)strtoul(argv[3],NULL,10);
  HANDLE parent=OpenProcess(SYNCHRONIZE,FALSE,ppid); if(!parent) return 3;
  { char ed[MAX_PATH]; GetModuleFileNameA(NULL,ed,MAX_PATH); char* sl=strrchr(ed,'\\'); if(sl){ *sl=0; SetCurrentDirectoryA(ed); } }
  SetPriorityClass(GetCurrentProcess(),(argc>=6 && atoi(argv[4])==0)? NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);   /* helper 0 builds what a draw is waiting for */
  FILE* lg; { char lp[64]; snprintf(lp,sizeof lp,"shader_cache\\jobs\\helper%s.log",argc>=6? argv[4] : ""); lg=fopen(lp,"w"); }
  WNDCLASSA wc; memset(&wc,0,sizeof wc); wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="swr_shader_helper"; RegisterClassA(&wc);
  HWND w=CreateWindowA(wc.lpszClassName,"",WS_OVERLAPPEDWINDOW,0,0,16,16,NULL,NULL,wc.hInstance,NULL); HDC dc=GetDC(w);
  PIXELFORMATDESCRIPTOR pfd; memset(&pfd,0,sizeof pfd); pfd.nSize=sizeof pfd; pfd.nVersion=1; pfd.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
  pfd.iPixelType=PFD_TYPE_RGBA; pfd.cColorBits=32; pfd.cDepthBits=24; pfd.cStencilBits=8; pfd.iLayerType=PFD_MAIN_PLANE;
  SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd);
  HGLRC tmp=wglCreateContext(dc); if(!tmp || !wglMakeCurrent(dc,tmp)){ if(lg){ fprintf(lg,"no GL context\n"); fclose(lg); } return 4; }
  CCA cca=(CCA)wglGetProcAddress("wglCreateContextAttribsARB"); HGLRC ctx=NULL;
  static const int vers[4][2]={{4,6},{4,5},{4,4},{4,3}};
  for(int i=0;i<4 && !ctx && cca;i++){ int ca[]={0x2091,vers[i][0],0x2092,vers[i][1],0x9126,0x00000001,0}; ctx=cca(dc,NULL,ca); }
  if(!ctx || !wglMakeCurrent(dc,ctx)){ if(lg){ fprintf(lg,"no core context\n"); fclose(lg); } return 4; }
  wglDeleteContext(tmp);
  HMODULE gl=GetModuleHandleA("opengl32.dll");
  HP_GS gs=(HP_GS)GetProcAddress(gl,"glGetString");
#define HPF(t,v,n) t v=(t)wglGetProcAddress(n);
  HPF(HP_CS,cs,"glCreateShader") HPF(HP_SS,ss,"glShaderSource") HPF(HP_U,comp,"glCompileShader") HPF(HP_CP,cp,"glCreateProgram")
  HPF(HP_AS,as,"glAttachShader") HPF(HP_PPI,ppi,"glProgramParameteri") HPF(HP_U,link,"glLinkProgram") HPF(HP_GPIV,gpiv,"glGetProgramiv")
  HPF(HP_GPB,gpb,"glGetProgramBinary") HPF(HP_GPIL,gpil,"glGetProgramInfoLog") HPF(HP_U,dsh,"glDeleteShader") HPF(HP_U,dpr,"glDeleteProgram")
#undef HPF
  if(!gs||!cs||!ss||!comp||!cp||!as||!ppi||!link||!gpiv||!gpb||!gpil||!dsh||!dpr){ if(lg){ fprintf(lg,"missing GL functions\n"); fclose(lg); } return 5; }
  { /* same fingerprint as the game's cache names (gpu_gl.c pcache_init): otherwise its binaries would be for another driver */
    const char* rn=(const char*)gs(0x1F01), *vv=(const char*)gs(0x1F02); unsigned long long h=1469598103934665603ull;
    for(const char* q=rn;q&&*q;q++){ h^=(unsigned char)*q; h*=1099511628211ull; } for(const char* q=vv;q&&*q;q++){ h^=(unsigned char)*q; h*=1099511628211ull; }
    char mine[16]; snprintf(mine,sizeof mine,"%08x",(unsigned)(h^(h>>32)));
    if(strcmp(mine,tag)){ if(lg){ fprintf(lg,"driver fingerprint %s, the game has %s: not building\n",mine,tag); fclose(lg); } return 6; }
    if(lg){ fprintf(lg,"ready: %s / %s\n",rn? rn : "?",vv? vv : "?"); fflush(lg); } }
  /* not glMaxShaderCompilerThreads: tested here too (driver 617.42), no compile ever completes after it. More throughput
     comes from more helpers (argv[4] is this one's index) */
  int hp_i= argc>=6? atoi(argv[4]) : 0;
  typedef struct { unsigned p; char name[64]; unsigned long long key; DWORD t0; } Pend;
  Pend pend[16]; int np=0; unsigned built=0, failed=0;
  while(WaitForSingleObject(parent,0)==WAIT_TIMEOUT){
    for(int i=0;i<np;){ int done=0; gpiv(pend[i].p,0x91B1 /* GL_COMPLETION_STATUS_KHR */,&done);
      if(!done){ i++; continue; }
      int ok=0; gpiv(pend[i].p,0x8B82 /* GL_LINK_STATUS */,&ok);
      char jp[128]; snprintf(jp,sizeof jp,"shader_cache\\jobs\\%s",pend[i].name);
      if(ok){ int len=0; gpiv(pend[i].p,0x8741 /* GL_PROGRAM_BINARY_LENGTH */,&len);
        unsigned char* b= len>0? malloc((size_t)len+8) : NULL; int got=0; unsigned fmt=0;
        if(b) gpb(pend[i].p,len,&got,&fmt,b+8);
        if(b && got>0){ unsigned n=(unsigned)got; memcpy(b,&fmt,4); memcpy(b+4,&n,4);
          char tp[160], fp[160]; snprintf(tp,sizeof tp,"shader_cache\\%s_s%016llx.tmp",tag,pend[i].key); snprintf(fp,sizeof fp,"shader_cache\\%s_s%016llx.bin",tag,pend[i].key);
          FILE* o=fopen(tp,"wb"); if(o){ fwrite(b,1,(size_t)got+8,o); fclose(o); MoveFileExA(tp,fp,MOVEFILE_REPLACE_EXISTING); }
          built++; if(lg){ fprintf(lg,"built %016llx in %lu ms\n",pend[i].key,(unsigned long)(GetTickCount()-pend[i].t0)); fflush(lg); } }
        free(b); }
      else { char e[2048]; e[0]=0; gpil(pend[i].p,sizeof e,NULL,e); failed++;
        char ep[128]; snprintf(ep,sizeof ep,"shader_cache\\jobs\\%016llx.err",pend[i].key); FILE* o=fopen(ep,"w"); if(o){ fputs(e,o); fclose(o); }
        if(lg){ fprintf(lg,"failed %016llx\n",pend[i].key); fflush(lg); } }
      dpr(pend[i].p); DeleteFileA(jp); pend[i]=pend[--np]; }
    /* .job: a draw is waiting for it, always first. .pjob: found ahead of its first draw; not on helper 0, which stays free
       for draws. One at a time: the driver builds a process's shaders strictly in order, so more in flight only makes the
       next urgent one wait (measured 2026-11-08: finished 2.3 s apart in submission order, one slow shader held up the rest).
       A job is claimed by renaming it, so the helpers share one queue and a shader that takes minutes ties up only one */
    for(int pass=0;pass<(hp_i==0? 1 : 2) && np<1;pass++){
      WIN32_FIND_DATAA fd; HANDLE fh=FindFirstFileA(pass? "shader_cache\\jobs\\*.pjob" : "shader_cache\\jobs\\*.job",&fd);
      if(fh!=INVALID_HANDLE_VALUE){ do {
          unsigned long long jk=strtoull(fd.cFileName,NULL,16);
          char jp[128]; snprintf(jp,sizeof jp,"shader_cache\\jobs\\%s",fd.cFileName); unsigned char* b; size_t n;
          { char fp[160]; snprintf(fp,sizeof fp,"shader_cache\\%s_s%016llx.bin",tag,jk); if(GetFileAttributesA(fp)!=INVALID_FILE_ATTRIBUTES){ DeleteFileA(jp); continue; } }   /* already built */
          if(!hp_read(jp,&b,&n)) continue;
          /* "SWRJ", GL stage enum, GLSL length, GLSL, "SWRJOBOK": a job still being written has no trailer yet */
          unsigned st=0, sl=0; if(n<20 || memcmp(b,"SWRJ",4)){ free(b); DeleteFileA(jp); continue; }
          memcpy(&st,b+4,4); memcpy(&sl,b+8,4); if((size_t)sl+20!=n || memcmp(b+12+sl,"SWRJOBOK",8)){ free(b); continue; }
          char cn[64]; snprintf(cn,sizeof cn,"%016llx.c%d",jk,hp_i); char cp_[128]; snprintf(cp_,sizeof cp_,"shader_cache\\jobs\\%s",cn);
          if(!MoveFileA(jp,cp_)){ free(b); continue; }   /* another helper took it */
          b[12+sl]=0; const char* src=(const char*)b+12;
          unsigned sh=cs(st); ss(sh,1,&src,NULL); comp(sh); unsigned p=cp(); ppi(p,0x8258 /* SEPARABLE */,1); ppi(p,0x8257 /* BINARY_RETRIEVABLE */,1);
          as(p,sh); link(p); dsh(sh); free(b);
          Pend* q=&pend[np++]; q->p=p; snprintf(q->name,sizeof q->name,"%s",cn); q->key=jk; q->t0=GetTickCount(); break;
        } while(FindNextFileA(fh,&fd)); FindClose(fh); } }
    Sleep(np? 2 : 10); }
  if(lg){ fprintf(lg,"game closed: %u built, %u failed\n",built,failed); fclose(lg); }
  return 0; }

/* the game's side: start the helper (killed with the game through a job object) */
int backend_shader_helper_start(const char* tag){
  CreateDirectoryA("shader_cache",NULL); CreateDirectoryA("shader_cache\\jobs",NULL);
  { WIN32_FIND_DATAA fd; HANDLE fh=FindFirstFileA("shader_cache\\jobs\\*.*",&fd);   /* jobs and errors from an earlier run */
    if(fh!=INVALID_HANDLE_VALUE){ do { if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue; char p[200]; snprintf(p,sizeof p,"shader_cache\\jobs\\%s",fd.cFileName); DeleteFileA(p); } while(FindNextFileA(fh,&fd)); FindClose(fh); } }
  char exe[MAX_PATH]; GetModuleFileNameA(NULL,exe,MAX_PATH);
  g_helper_job=CreateJobObjectA(NULL,NULL);
  if(g_helper_job){ JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li,0,sizeof li); li.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_helper_job,JobObjectExtendedLimitInformation,&li,sizeof li); }
  for(int i=0;i<HELPER_COUNT;i++){ char cmd[MAX_PATH+128]; snprintf(cmd,sizeof cmd,"\"%s\" --shader-helper %s %lu %d %d",exe,tag,(unsigned long)GetCurrentProcessId(),i,HELPER_COUNT);
    STARTUPINFOA si; memset(&si,0,sizeof si); si.cb=sizeof si; PROCESS_INFORMATION pi; memset(&pi,0,sizeof pi);
    if(!CreateProcessA(NULL,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW|CREATE_SUSPENDED,NULL,NULL,&si,&pi)){
      LOG("shaders: the shader helper could not start (error %lu)\n",(unsigned long)GetLastError()); for(int k=0;k<i;k++){ TerminateProcess(g_helper_proc2[k],0); CloseHandle(g_helper_proc2[k]); g_helper_proc2[k]=NULL; } return 0; }
    if(g_helper_job) AssignProcessToJobObject(g_helper_job,pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); g_helper_proc2[i]=pi.hProcess; }
  LOG("shaders: new shaders are built by %d helper processes: the game's GL thread doesn't wait on the driver's compiler\n",HELPER_COUNT);
  return 1; }
int backend_shader_helper_alive(void){ for(int i=0;i<HELPER_COUNT;i++) if(g_helper_proc2[i] && WaitForSingleObject(g_helper_proc2[i],0)==WAIT_TIMEOUT) return 1; return 0; }
int backend_file_exists(const char* path){ DWORD a=GetFileAttributesA(path); return a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY); }

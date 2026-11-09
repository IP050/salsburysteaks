/* Discord Rich Presence through the Discord app's local pipe (\\.\pipe\discord-ipc-N), no SDK. Everything happens on one
   low-priority thread that only sleeps and talks to the pipe, so a slow or missing Discord can never hold up the game.
   Discord clears the presence by itself when the pipe closes at exit. discord_app_id in the ini is the application's ID
   from discord.com/developers/applications (its name is the "Playing ..." line); no ID, no thread. */
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef DISCORD_TEST
#include "rt.h"
#else
#define LOG printf
#endif

enum { OP_HANDSHAKE=0, OP_FRAME=1, OP_CLOSE=2, OP_PING=3, OP_PONG=4 };
static char g_app_id[32]; static time_t g_started; static int g_enabled=1;

void discord_ini(const char* k, const char* v){
  if(!strcmp(k,"discord")) g_enabled=atoi(v)!=0;
  else if(!strcmp(k,"discord_app_id")){   /* digits only: it goes into JSON as it is */
    size_t n=strlen(v); g_app_id[0]=0; if(n>=sizeof g_app_id) return;
    for(size_t i=0;i<n;i++) if(v[i]<'0'||v[i]>'9') return;
    memcpy(g_app_id,v,n+1); } }

static int pipe_send(HANDLE p, unsigned op, const char* json, unsigned len){
  unsigned hdr[2]={op,len}; DWORD w;
  return WriteFile(p,hdr,sizeof hdr,&w,NULL) && w==sizeof hdr && WriteFile(p,json,len,&w,NULL) && w==len; }

/* 1 = a frame was read, 0 = nothing waiting, -1 = the pipe is gone. Text longer than the buffer is read and dropped. */
static int pipe_recv(HANDLE p, unsigned* op, char* buf, unsigned cap){
  DWORD avail=0, r; if(!PeekNamedPipe(p,NULL,0,NULL,&avail,NULL)) return -1;
  if(avail<8) return 0;
  unsigned hdr[2]; if(!ReadFile(p,hdr,sizeof hdr,&r,NULL) || r!=sizeof hdr) return -1;
  *op=hdr[0]; unsigned left=hdr[1], got=0;
  while(left){ char junk[512]; unsigned room=got+1<cap? cap-got-1:0; char* to=room? buf+got:junk; unsigned want=room? room:sizeof junk; if(want>left) want=left;
    if(!ReadFile(p,to,want,&r,NULL) || !r) return -1;
    if(room) got+=r;
    left-=r; }
  buf[got]=0; return 1; }

static HANDLE pipe_open(void){
  for(int i=0;i<10;i++){ char name[64]; snprintf(name,sizeof name,"\\\\.\\pipe\\discord-ipc-%d",i);
    HANDLE p=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if(p!=INVALID_HANDLE_VALUE) return p; }
  return INVALID_HANDLE_VALUE; }

/* one connection, until the pipe closes. 1 = try again later, 0 = Discord refused the ID, stop */
static int presence_session(HANDLE p){
  char buf[8192]; unsigned op=0; int n;
  n=snprintf(buf,sizeof buf,"{\"v\":1,\"client_id\":\"%s\"}",g_app_id);
  if(!pipe_send(p,OP_HANDSHAKE,buf,n)) return 1;
  int waited=0, r;
  while(!(r=pipe_recv(p,&op,buf,sizeof buf))){ if(++waited>100) return 1; Sleep(100); }   /* its answer, up to 10 seconds */
  if(r<0) return 1;
  if(op==OP_CLOSE){ LOG("discord: refused: %s\n",buf); return strstr(buf,"4000")? 0:1; }
  if(!strstr(buf,"READY")){ LOG("discord: unexpected answer: %.200s\n",buf); return 1; }
  n=snprintf(buf,sizeof buf,
    "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"1\",\"args\":{\"pid\":%lu,\"activity\":{"
    "\"details\":\"Windows recomp\",\"timestamps\":{\"start\":%lld},"
    "\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Metroid Prime 4: Beyond\"}}}}",
    GetCurrentProcessId(),(long long)g_started);
  if(!pipe_send(p,OP_FRAME,buf,n)) return 1;
  int reported=0;
  for(;;){ r=pipe_recv(p,&op,buf,sizeof buf);
    if(r<0) return 1;
    if(!r){ Sleep(1000); continue; }
    if(op==OP_PING){ if(!pipe_send(p,OP_PONG,buf,(unsigned)strlen(buf))) return 1; }
    else if(op==OP_CLOSE){ LOG("discord: closed: %.200s\n",buf); return 1; }
    else if(!reported){ reported=1; if(strstr(buf,"\"evt\":\"ERROR\"")) LOG("discord: presence refused: %.300s\n",buf); else LOG("discord: presence set: %.300s\n",buf); } } }

static unsigned __stdcall presence_thread(void* a){ (void)a;
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
  for(;;){ HANDLE p=pipe_open();
    if(p!=INVALID_HANDLE_VALUE){ int again=presence_session(p); CloseHandle(p); if(!again) return 0; }
    Sleep(15000); } }   /* Discord closed or not started yet: keep looking, it may be opened later */

void discord_start(void){
  static int started; if(started || !g_enabled || !g_app_id[0]) return; started=1;
  g_started=time(NULL);
  uintptr_t h=_beginthreadex(NULL,64*1024,presence_thread,NULL,0,NULL); if(h) CloseHandle((HANDLE)h); }

#ifdef DISCORD_TEST
int main(int argc, char** argv){ setvbuf(stdout,NULL,_IONBF,0); discord_ini("discord_app_id",argc>1?argv[1]:"1");
  printf("id '%s'\n",g_app_id); g_started=time(NULL);
  HANDLE p=pipe_open(); if(p==INVALID_HANDLE_VALUE){ printf("no pipe\n"); return 1; }
  int again=presence_session(p); printf("session ended, again=%d\n",again); CloseHandle(p); return 0; }
#endif

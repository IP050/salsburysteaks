/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* executable entry point */
int swr_main(int argc, char** argv);
#if defined(_WIN32)
#include <windows.h>
#include <string.h>
int shader_helper_main(int argc, char** argv);   /* backend_win32.c (shader_helper.inc): compiles shaders for the game */

int main(int argc, char** argv){
  if(argc>=2 && !strcmp(argv[1],"--shader-helper")) return shader_helper_main(argc,argv);
  return swr_main(argc,argv); }
#else
int main(int argc, char** argv){ return swr_main(argc,argv); }
#endif

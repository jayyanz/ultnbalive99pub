#include <windows.h>
#include <stdio.h>
typedef void (__stdcall *S)(int);
static DWORD WINAPI th(LPVOID p){ return 0; }
int main(void){
  HMODULE h=LoadLibraryA("glide2x.dll"); ((S)GetProcAddress(h,"_grBufferSwap@4"))(1);
  printf("free=%d\n",FreeLibrary(h));
  HANDLE t=CreateThread(NULL,0,th,NULL,0,NULL); SetThreadPriority(t,1); WaitForSingleObject(t,INFINITE);
  h=LoadLibraryA("glide2x.dll"); ((S)GetProcAddress(h,"_grBufferSwap@4"))(1);
  t=CreateThread(NULL,0,th,NULL,0,NULL); WaitForSingleObject(t,INFINITE);
  printf("ok\n"); return 0;
}

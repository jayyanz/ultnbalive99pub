#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef void (__stdcall *V)(char*); typedef int (__stdcall *Q)(int*);
typedef int (__stdcall *W)(uint32_t,int,int,int,int,int,int); typedef void (__stdcall *S)(int);
static volatile int sink;
__declspec(noinline) void slow_loader(void){ Sleep(1500); }   /* simulated stall: blocking call */
__declspec(noinline) void busy_loader(void){ DWORD t=GetTickCount(); while(GetTickCount()-t<900) sink++; }
static DWORD WINAPI hog(LPVOID p){ DWORD t=GetTickCount(); while(GetTickCount()-t<(DWORD)(uintptr_t)p) sink++; return 0; }
int main(void){
  HMODULE h=LoadLibraryA("glide2x.dll"); if(!h){printf("load fail %lu\n",GetLastError());return 1;}
  char v[64]={0}; int hw[4]={0};
  ((V)GetProcAddress(h,"_grGlideGetVersion@4"))(v);
  int nb=((Q)GetProcAddress(h,"_grSstQueryBoards@4"))(hw);
  int r=((W)GetProcAddress(h,"_grSstWinOpen@28"))(1,2,3,4,5,6,7);
  S sw=(S)GetProcAddress(h,"_grBufferSwap@4");
  SetProcessAffinityMask(GetCurrentProcess(),1);              /* like the SingleProcAffinity shim */
  for(int i=0;i<650;i++){ sw(1); Sleep(i<300?1:5);
    if(i==400) slow_loader();
    if(i==500) busy_loader();
    if(i==600){ HANDLE t=CreateThread(NULL,0,hog,(LPVOID)1200,0,NULL); SetThreadPriority(t,THREAD_PRIORITY_TIME_CRITICAL); Sleep(10); WaitForSingleObject(t,INFINITE); }
  }
  printf("version=%s boards=%d hw0=%d winopen=%d slow=%p busy=%p hog=%p\n",v,nb,hw[0],r,(void*)slow_loader,(void*)busy_loader,(void*)hog);
  return !(nb==1&&hw[0]==1&&r==28&&!strcmp(v,"fake 2.43"));
}

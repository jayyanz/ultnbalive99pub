#include <windows.h>
#include <stdio.h>
typedef void (__stdcall *S)(int);
static DWORD WINAPI th(LPVOID p){ Sleep(200); return 0; }
int main(void){
  DWORD_PTR pm,sm; SetProcessAffinityMask(GetCurrentProcess(),1);   /* like the shim */
  HMODULE h=LoadLibraryA("glide2x.dll"); ((S)GetProcAddress(h,"_grBufferSwap@4"))(1);
  GetProcessAffinityMask(GetCurrentProcess(),&pm,&sm);
  HANDLE t=CreateThread(NULL,0,th,NULL,CREATE_SUSPENDED,NULL);
  DWORD_PTR prev=SetThreadAffinityMask(t,1); SetThreadAffinityMask(t,prev);
  ResumeThread(t); WaitForSingleObject(t,INFINITE);
  DWORD_PTR mainprev=SetThreadAffinityMask(GetCurrentThread(),pm);
  printf("process mask now 0x%lx (system 0x%lx); new game thread mask 0x%lx; main thread mask 0x%lx\n",(unsigned long)pm,(unsigned long)sm,(unsigned long)prev,(unsigned long)mainprev);
  return 0;
}

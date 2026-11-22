#include <windows.h>
#include <stdint.h>
/* fake real glide: a few functions with checkable results */
__declspec(dllexport) void __stdcall grGlideGetVersion(char *v){ lstrcpyA(v,"fake 2.43"); }
__declspec(dllexport) int __stdcall grSstQueryBoards(int *hw){ hw[0]=1; return 1; }
__declspec(dllexport) int __stdcall grSstWinOpen(uint32_t a,int b,int c,int d,int e,int f,int g){ return a+b+c+d+e+f+g; }
__declspec(dllexport) void __stdcall grBufferSwap(int i){ (void)i; }

/*
 * C++ build settings (0.7.15). Every source file is compiled as C++ with the
 * restricted subset in README.md ("C++ rules") and includes this first.
 * - CINTERFACE and COBJMACROS keep the C-style COM calls (IDXGIFactory1_Release(f)
 *   and so on) that the C code used, so no COM code changes.
 * - D3D11_NO_HELPERS leaves out the C++ helper classes in d3d11.h.
 * - Each file's own code sits inside NBA99_C_BEGIN / NBA99_C_END (extern "C"),
 *   so every function and global keeps its C name: the exports, the symbols
 *   the inline asm thunks call or read by name, and the names shared with the
 *   .def files stay exactly as they were in C.
 */
#ifndef NBA99_CXX_H
#define NBA99_CXX_H
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#ifdef __cplusplus
#define NBA99_C_BEGIN extern "C" {
#define NBA99_C_END }
#else
#define NBA99_C_BEGIN
#define NBA99_C_END
#endif
#endif

#ifndef OBC_IMGUI_BACKEND_H
#define OBC_IMGUI_BACKEND_H

#include "imgui_impl_sdl2.h"

/*
 * Which ImGui renderer backend the build uses.
 *
 * Desktop takes opengl2, the fixed function one, matching the GL 1.2 floor.
 * The browser cannot: imgui_impl_opengl2 needs glPushAttrib, glTexEnvi and
 * client side arrays, none of which exist in GLES 2.0, and the vendored files
 * stay unmodified so the shim in gl_compat.h does not reach them. opengl3
 * builds against GLES 2.0 as shipped and is the backend upstream supports on
 * Emscripten, so the browser uses it with a GLSL 1.00 header.
 *
 * Only the panel chrome goes through this. The 3D view is our own code either
 * way, and still targets 1.2.
 */

#if defined(__EMSCRIPTEN__)

#include "imgui_impl_opengl3.h"

#define obc_imgui_renderer_init()    ImGui_ImplOpenGL3_Init("#version 100")
#define obc_imgui_renderer_new_frame ImGui_ImplOpenGL3_NewFrame
#define obc_imgui_renderer_render    ImGui_ImplOpenGL3_RenderDrawData
#define obc_imgui_renderer_shutdown  ImGui_ImplOpenGL3_Shutdown

#else

#include "imgui_impl_opengl2.h"

#define obc_imgui_renderer_init()    ImGui_ImplOpenGL2_Init()
#define obc_imgui_renderer_new_frame ImGui_ImplOpenGL2_NewFrame
#define obc_imgui_renderer_render    ImGui_ImplOpenGL2_RenderDrawData
#define obc_imgui_renderer_shutdown  ImGui_ImplOpenGL2_Shutdown

#endif

#endif

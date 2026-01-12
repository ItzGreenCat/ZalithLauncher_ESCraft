#include <stddef.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include "br_loader.h"
#include "egl_loader.h"
#include <EGL/egl.h>

// 全局函数指针
EGLBoolean (*eglMakeCurrent_p) (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean (*eglDestroyContext_p) (EGLDisplay dpy, EGLContext ctx);
EGLBoolean (*eglDestroySurface_p) (EGLDisplay dpy, EGLSurface surface);
EGLBoolean (*eglTerminate_p) (EGLDisplay dpy);
EGLBoolean (*eglReleaseThread_p) (void);
EGLContext (*eglGetCurrentContext_p) (void);
EGLDisplay (*eglGetDisplay_p) (NativeDisplayType display);
EGLBoolean (*eglInitialize_p) (EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean (*eglChooseConfig_p) (EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config);
EGLBoolean (*eglGetConfigAttrib_p) (EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value);
EGLBoolean (*eglBindAPI_p) (EGLenum api);
EGLSurface (*eglCreatePbufferSurface_p) (EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list);
EGLSurface (*eglCreateWindowSurface_p) (EGLDisplay dpy, EGLConfig config, NativeWindowType window, const EGLint *attrib_list);
EGLBoolean (*eglSwapBuffers_p) (EGLDisplay dpy, EGLSurface draw);
EGLint (*eglGetError_p) (void);
EGLContext (*eglCreateContext_p) (EGLDisplay dpy, EGLConfig config, EGLContext share_list, const EGLint *attrib_list);
EGLBoolean (*eglSwapInterval_p) (EGLDisplay dpy, EGLint interval);
EGLSurface (*eglGetCurrentSurface_p) (EGLint readdraw);
EGLBoolean (*eglQuerySurface_p)(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint * value);
extern __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_p) (const char *procname);

// ============================================================================
// HOOKS
// ============================================================================
static EGLBoolean (*real_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
static EGLContext (*real_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = NULL;
static EGLBoolean (*real_eglBindAPI)(EGLenum) = NULL;

static EGLBoolean hook_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    EGLint new_attribs[64];
    int i = 0, j = 0;
    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];
            if (attr == EGL_RENDERABLE_TYPE) val = 0x0004; // ES2
            if (attr == EGL_DEPTH_SIZE && val > 16) val = 16;
            new_attribs[j++] = attr;
            new_attribs[j++] = val;
            i += 2;
        }
    }
    new_attribs[j] = EGL_NONE;
    return real_eglChooseConfig(dpy, new_attribs, configs, config_size, num_config);
}

static EGLContext hook_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_list, const EGLint *attrib_list) {
    EGLint new_attribs[64];
    int i = 0, j = 0, has_ver = 0;
    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];
            if (attr == 0x3098) has_ver = 1;
            new_attribs[j++] = attr;
            new_attribs[j++] = val;
            i += 2;
        }
    }
    if (!has_ver) { new_attribs[j++] = 0x3098; new_attribs[j++] = 3; }
    new_attribs[j] = EGL_NONE;
    EGLContext ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    if (ctx == EGL_NO_CONTEXT && !has_ver) {
         new_attribs[j-2] = 2;
         ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    }
    return ctx;
}

static EGLBoolean hook_eglBindAPI(EGLenum api) { return real_eglBindAPI(0x30A0); }

// ============================================================================
// 加载器入口
// ============================================================================
void dlsym_EGL() {
    printf("egl_loader: [ABSOLUTE GLOBAL] Loading SYSTEM EGL...\n");
    void* dl_handle = NULL;
    
    // [关键修改] 使用 RTLD_GLOBAL，让符号对 LWJGL 可见！
    int flags = RTLD_GLOBAL | RTLD_LAZY;

    dl_handle = dlopen("/system/lib64/libEGL.so", flags);
    if (!dl_handle) dl_handle = dlopen("/system/lib/libEGL.so", flags);
    if (!dl_handle) dl_handle = dlopen("/vendor/lib64/libEGL.so", flags);

    if (!dl_handle) {
        printf("egl_loader: [FATAL] System EGL not found!\n");
        abort();
    }

    // 同时预加载 GLESv2，确保核心函数也就位
    dlopen("/system/lib64/libGLESv2.so", flags);
    dlopen("/system/lib/libGLESv2.so", flags);

    real_eglChooseConfig = dlsym(dl_handle, "eglChooseConfig");
    real_eglCreateContext = dlsym(dl_handle, "eglCreateContext");
    real_eglBindAPI = dlsym(dl_handle, "eglBindAPI");

    #define LOAD(name) name##_p = dlsym(dl_handle, #name);
    LOAD(eglMakeCurrent); LOAD(eglDestroyContext); LOAD(eglDestroySurface);
    LOAD(eglTerminate); LOAD(eglReleaseThread); LOAD(eglGetCurrentContext);
    LOAD(eglGetDisplay); LOAD(eglInitialize); LOAD(eglGetConfigAttrib);
    LOAD(eglCreatePbufferSurface); LOAD(eglCreateWindowSurface); LOAD(eglSwapBuffers);
    LOAD(eglGetError); LOAD(eglSwapInterval); LOAD(eglGetCurrentSurface);
    LOAD(eglQuerySurface);
    
    eglGetProcAddress_p = dlsym(dl_handle, "eglGetProcAddress");
    eglChooseConfig_p = hook_eglChooseConfig;
    eglCreateContext_p = hook_eglCreateContext;
    eglBindAPI_p = hook_eglBindAPI;

    printf("egl_loader: System EGL loaded (GLOBAL mode).\n");
    if (eglBindAPI_p) eglBindAPI_p(0);
}

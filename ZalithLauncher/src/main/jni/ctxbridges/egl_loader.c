#include <stddef.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include "br_loader.h"
#include "egl_loader.h"
#include <EGL/egl.h>

// ============================================================================
// 全局函数指针定义
// ============================================================================
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

// [FIX] 改为 extern，因为 br_loader.c 已经定义了它
extern __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_p) (const char *procname);

// ============================================================================
// 系统库句柄
// ============================================================================
static void* sys_lib_egl = NULL;

// 保存原始的系统函数指针，用于在 Hook 中回调
static EGLBoolean (*real_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
static EGLContext (*real_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = NULL;
static EGLBoolean (*real_eglBindAPI)(EGLenum) = NULL;

// ============================================================================
// HOOK 函数：拦截并篡改参数
// ============================================================================

// Hook 1: 强制修改 Config 属性
static EGLBoolean hook_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    printf("egl_loader: [HOOK] eglChooseConfig intercepted.\n");

    EGLint new_attribs[64];
    int i = 0;
    int j = 0;

    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];

            // 强制改为 ES2 (兼容 ES3)
            if (attr == EGL_RENDERABLE_TYPE) {
                val = 0x0004; 
            }
            // 强制深度降级
            if (attr == EGL_DEPTH_SIZE) {
                if (val > 16) val = 16;
            }

            new_attribs[j++] = attr;
            new_attribs[j++] = val;
            i += 2;
        }
    }
    new_attribs[j] = EGL_NONE;

    return real_eglChooseConfig(dpy, new_attribs, configs, config_size, num_config);
}

// Hook 2: 强制指定 Version 3
static EGLContext hook_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_list, const EGLint *attrib_list) {
    printf("egl_loader: [HOOK] eglCreateContext intercepted.\n");

    EGLint new_attribs[64];
    int i = 0;
    int j = 0;
    int has_version = 0;

    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];
            if (attr == 0x3098) has_version = 1;
            new_attribs[j++] = attr;
            new_attribs[j++] = val;
            i += 2;
        }
    }

    if (!has_version) {
        new_attribs[j++] = 0x3098; 
        new_attribs[j++] = 3;      
    }
    new_attribs[j] = EGL_NONE;

    EGLContext ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    
    // 回退机制
    if (ctx == EGL_NO_CONTEXT && !has_version) {
         new_attribs[j-2] = 2;
         ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    }
    
    return ctx;
}

// Hook 3: 强制 Bind GLES
static EGLBoolean hook_eglBindAPI(EGLenum api) {
    return real_eglBindAPI(0x30A0);
}

// ============================================================================
// 核心加载函数
// ============================================================================
void dlsym_EGL() {
    printf("egl_loader: [NUCLEAR] Loading SYSTEM libEGL.so directly...\n");

    sys_lib_egl = dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    if (!sys_lib_egl) sys_lib_egl = dlopen("/system/lib64/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    if (!sys_lib_egl) sys_lib_egl = dlopen("/system/lib/libEGL.so", RTLD_LOCAL | RTLD_LAZY);

    if (!sys_lib_egl) {
        printf("egl_loader: [FATAL] Failed to load libEGL.so!\n");
        abort();
    }

    real_eglChooseConfig = dlsym(sys_lib_egl, "eglChooseConfig");
    real_eglCreateContext = dlsym(sys_lib_egl, "eglCreateContext");
    real_eglBindAPI = dlsym(sys_lib_egl, "eglBindAPI");

    eglMakeCurrent_p = dlsym(sys_lib_egl, "eglMakeCurrent");
    eglDestroyContext_p = dlsym(sys_lib_egl, "eglDestroyContext");
    eglDestroySurface_p = dlsym(sys_lib_egl, "eglDestroySurface");
    eglTerminate_p = dlsym(sys_lib_egl, "eglTerminate");
    eglReleaseThread_p = dlsym(sys_lib_egl, "eglReleaseThread");
    eglGetCurrentContext_p = dlsym(sys_lib_egl, "eglGetCurrentContext");
    eglGetDisplay_p = dlsym(sys_lib_egl, "eglGetDisplay");
    eglInitialize_p = dlsym(sys_lib_egl, "eglInitialize");
    
    // 应用 HOOK
    eglChooseConfig_p = hook_eglChooseConfig;
    eglCreateContext_p = hook_eglCreateContext;
    eglBindAPI_p = hook_eglBindAPI;

    eglGetConfigAttrib_p = dlsym(sys_lib_egl, "eglGetConfigAttrib");
    eglCreatePbufferSurface_p = dlsym(sys_lib_egl, "eglCreatePbufferSurface");
    eglCreateWindowSurface_p = dlsym(sys_lib_egl, "eglCreateWindowSurface");
    eglSwapBuffers_p = dlsym(sys_lib_egl, "eglSwapBuffers");
    eglGetError_p = dlsym(sys_lib_egl, "eglGetError");
    eglSwapInterval_p = dlsym(sys_lib_egl, "eglSwapInterval");
    eglGetCurrentSurface_p = dlsym(sys_lib_egl, "eglGetCurrentSurface");
    eglQuerySurface_p = dlsym(sys_lib_egl, "eglQuerySurface");
    
    // 这里只赋值，不定义，定义在 br_loader.c
    eglGetProcAddress_p = dlsym(sys_lib_egl, "eglGetProcAddress");

    printf("egl_loader: System EGL loaded and HOOKED successfully.\n");
    
    if (eglBindAPI_p) eglBindAPI_p(0);
}

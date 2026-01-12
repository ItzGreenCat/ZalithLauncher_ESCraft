//
// Created by maks on 21.09.2022.
// Modified for System GLES Support
//
#include <stddef.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h> // Added for printf
#include "br_loader.h"
#include "egl_loader.h"
#include <EGL/egl.h> // Ensure we have EGL definitions

// Global function pointers
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

// -------------------------------------------------------------------------
// [HOOK] 自定义 eglBindAPI，强制 GLES
// -------------------------------------------------------------------------
static EGLBoolean (*real_eglBindAPI)(EGLenum api) = NULL;

static EGLBoolean my_hooked_eglBindAPI(EGLenum api) {
    // 无论调用者想绑什么 (比如 EGL_OPENGL_API)，强制绑 GLES
    // 0x30A0 = EGL_OPENGL_ES_API
    printf("egl_loader: [HOOK] eglBindAPI intercepted. Forcing EGL_OPENGL_ES_API (0x30A0)\n");
    if (real_eglBindAPI) {
        return real_eglBindAPI(0x30A0);
    }
    return EGL_FALSE;
}

// -------------------------------------------------------------------------
// 加载逻辑
// -------------------------------------------------------------------------
void dlsym_EGL() {
    void* dl_handle = NULL;
    
    // [MOD] 1. 优先检查是否启用了系统 GLES 模式
    // 我们约定：如果环境变量 POJAV_RENDERER=system_gles，则强制加载系统库
    const char* renderer = getenv("POJAV_RENDERER");
    int use_system_gles = (renderer && strcmp(renderer, "system_gles") == 0);

    if (use_system_gles) {
        printf("egl_loader: System GLES Mode detected. Loading system libEGL.so...\n");
        // 尝试加载系统库
        dl_handle = dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY);
        if (!dl_handle) dl_handle = dlopen("/system/lib64/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
        if (!dl_handle) dl_handle = dlopen("/system/lib/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
        
        if (!dl_handle) {
            printf("egl_loader: [FATAL] Failed to load system libEGL.so: %s\n", dlerror());
            abort();
        }
    } else {
        // 原版加载逻辑 (gl4es / ANGLE)
        char* eglName = NULL;
        char* gles = getenv("LIBGL_GLES");

        if (gles && !strncmp(gles, "libGLESv2_angle.so", 18)) {
            eglName = "libEGL_angle.so";
        } else {
            eglName = getenv("POJAVEXEC_EGL");
        }

        if (eglName) dl_handle = dlopen(eglName, RTLD_LOCAL | RTLD_LAZY);
        if (dl_handle == NULL) dl_handle = dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    }

    if (dl_handle == NULL) abort();

    // 加载函数指针
    // 注意：系统库通常用 dlsym 就能拿到，GLGetProcAddress 内部通常也是 dlsym
    #define LOAD_EGL(name) name##_p = (typeof(name##_p))dlsym(dl_handle, #name); if(!name##_p) name##_p = (typeof(name##_p))dlsym(RTLD_DEFAULT, #name);

    LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglCreateWindowSurface);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglGetConfigAttrib);
    LOAD_EGL(eglGetCurrentContext);
    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglGetError);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglSwapBuffers);
    LOAD_EGL(eglReleaseThread);
    LOAD_EGL(eglSwapInterval);
    LOAD_EGL(eglTerminate);
    LOAD_EGL(eglGetCurrentSurface);
    LOAD_EGL(eglQuerySurface);

    // [MOD] 2. 如果是系统模式，应用 API Hook
    if (use_system_gles && eglBindAPI_p) {
        printf("egl_loader: Applying eglBindAPI Hook...\n");
        real_eglBindAPI = eglBindAPI_p;
        eglBindAPI_p = my_hooked_eglBindAPI;
        
        // 立即调用一次，确保环境已经切换
        my_hooked_eglBindAPI(0); 
    }
}

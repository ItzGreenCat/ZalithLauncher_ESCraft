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
__eglMustCastToProperFunctionPointerType (*eglGetProcAddress_p) (const char *procname);

// -------------------------------------------------------------------------
// Hooks: 强制修正参数，防止系统 EGL 拒绝
// -------------------------------------------------------------------------
static EGLBoolean (*real_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
static EGLContext (*real_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = NULL;
static EGLBoolean (*real_eglBindAPI)(EGLenum) = NULL;

static EGLBoolean hook_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config) {
    // 强制修改 Renderable Type 为 ES2 (兼容性最强)
    // 强制修改 Depth 为 16 (防止 24位 导致匹配失败)
    EGLint new_attribs[64];
    int i = 0, j = 0;
    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];
            if (attr == EGL_RENDERABLE_TYPE) val = 0x0004; // EGL_OPENGL_ES2_BIT
            if (attr == EGL_DEPTH_SIZE) if (val > 16) val = 16;
            new_attribs[j++] = attr;
            new_attribs[j++] = val;
            i += 2;
        }
    }
    new_attribs[j] = EGL_NONE;
    return real_eglChooseConfig(dpy, new_attribs, configs, config_size, num_config);
}

static EGLContext hook_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_list, const EGLint *attrib_list) {
    // 强制注入 Client Version 3
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
    if (!has_ver) {
        new_attribs[j++] = 0x3098; // EGL_CONTEXT_CLIENT_VERSION
        new_attribs[j++] = 3;
    }
    new_attribs[j] = EGL_NONE;
    
    // 尝试创建 3.0
    EGLContext ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    
    // 失败则回退 2.0
    if (ctx == EGL_NO_CONTEXT && !has_ver) {
        new_attribs[j-1] = 2;
        ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    }
    return ctx;
}

static EGLBoolean hook_eglBindAPI(EGLenum api) {
    // 强制绑定 ES
    return real_eglBindAPI(0x30A0); // EGL_OPENGL_ES_API
}

// -------------------------------------------------------------------------
// 加载器入口
// -------------------------------------------------------------------------
void dlsym_EGL() {
    printf("egl_loader: [FORCE MODE] Ignoring everything, loading SYSTEM libEGL.so...\n");
    
    // 1. 硬编码加载系统库
    void* dl_handle = dlopen("libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    if (!dl_handle) dl_handle = dlopen("/system/lib64/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    if (!dl_handle) dl_handle = dlopen("/system/lib/libEGL.so", RTLD_LOCAL | RTLD_LAZY);

    if (!dl_handle) {
        printf("egl_loader: [FATAL] Cannot load system EGL!\n");
        abort();
    }

    // 2. 加载原始函数
    real_eglChooseConfig = dlsym(dl_handle, "eglChooseConfig");
    real_eglCreateContext = dlsym(dl_handle, "eglCreateContext");
    real_eglBindAPI = dlsym(dl_handle, "eglBindAPI");

    // 3. 填充全局指针 (使用 Hook)
    eglMakeCurrent_p = dlsym(dl_handle, "eglMakeCurrent");
    eglDestroyContext_p = dlsym(dl_handle, "eglDestroyContext");
    eglDestroySurface_p = dlsym(dl_handle, "eglDestroySurface");
    eglTerminate_p = dlsym(dl_handle, "eglTerminate");
    eglReleaseThread_p = dlsym(dl_handle, "eglReleaseThread");
    eglGetCurrentContext_p = dlsym(dl_handle, "eglGetCurrentContext");
    eglGetDisplay_p = dlsym(dl_handle, "eglGetDisplay");
    eglInitialize_p = dlsym(dl_handle, "eglInitialize");
    
    // 应用 HOOKS
    eglChooseConfig_p = hook_eglChooseConfig;
    eglCreateContext_p = hook_eglCreateContext;
    eglBindAPI_p = hook_eglBindAPI;

    eglGetConfigAttrib_p = dlsym(dl_handle, "eglGetConfigAttrib");
    eglCreatePbufferSurface_p = dlsym(dl_handle, "eglCreatePbufferSurface");
    eglCreateWindowSurface_p = dlsym(dl_handle, "eglCreateWindowSurface");
    eglSwapBuffers_p = dlsym(dl_handle, "eglSwapBuffers");
    eglGetError_p = dlsym(dl_handle, "eglGetError");
    eglSwapInterval_p = dlsym(dl_handle, "eglSwapInterval");
    eglGetCurrentSurface_p = dlsym(dl_handle, "eglGetCurrentSurface");
    eglQuerySurface_p = dlsym(dl_handle, "eglQuerySurface");
    eglGetProcAddress_p = dlsym(dl_handle, "eglGetProcAddress");

    // 立即绑定一次 API
    if (eglBindAPI_p) eglBindAPI_p(0);
}

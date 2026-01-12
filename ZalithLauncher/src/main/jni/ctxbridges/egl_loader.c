#include <stddef.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include "br_loader.h"
#include "egl_loader.h"
#include <EGL/egl.h>

// ============================================================================
// 全局函数指针
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

// 使用 extern 避免 duplicate symbol 错误
extern __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_p) (const char *procname);

// ============================================================================
// HOOKS: 强制修正 Minecraft 的 Desktop GL 请求为 GLES
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
            
            // 强制 ES2/ES3 兼容位
            if (attr == EGL_RENDERABLE_TYPE) val = 0x0004; 
            // 强制 16位 深度 (Android 系统 EGL 的痛点，设为 24 容易崩)
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
    // 强制注入版本 3
    if (!has_ver) { new_attribs[j++] = 0x3098; new_attribs[j++] = 3; }
    new_attribs[j] = EGL_NONE;
    
    EGLContext ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    // 失败回退 2
    if (ctx == EGL_NO_CONTEXT && !has_ver) {
         new_attribs[j-2] = 2;
         ctx = real_eglCreateContext(dpy, config, share_list, new_attribs);
    }
    return ctx;
}

static EGLBoolean hook_eglBindAPI(EGLenum api) {
    return real_eglBindAPI(0x30A0); // 强制绑定 EGL_OPENGL_ES_API
}

// ============================================================================
// 加载器入口：绝对路径硬编码
// ============================================================================
void dlsym_EGL() {
    printf("egl_loader: [ABSOLUTE FORCE] Loading SYSTEM libEGL.so...\n");

    void* dl_handle = NULL;
    
    // 1. 尝试 64位 系统路径
    dl_handle = dlopen("/system/lib64/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    
    // 2. 尝试 32位 系统路径
    if (!dl_handle) dl_handle = dlopen("/system/lib/libEGL.so", RTLD_LOCAL | RTLD_LAZY);
    
    // 3. 尝试 Vendor 路径 (部分机型)
    if (!dl_handle) dl_handle = dlopen("/vendor/lib64/libEGL.so", RTLD_LOCAL | RTLD_LAZY);

    if (!dl_handle) {
        printf("egl_loader: [FATAL] System libEGL.so NOT FOUND in /system or /vendor!\n");
        // 我们不回退，直接让它死，这样你就知道系统库真的没加载到
        abort(); 
    }

    // 加载原始指针
    real_eglChooseConfig = dlsym(dl_handle, "eglChooseConfig");
    real_eglCreateContext = dlsym(dl_handle, "eglCreateContext");
    real_eglBindAPI = dlsym(dl_handle, "eglBindAPI");

    // 填充全局指针
    #define LOAD(name) name##_p = dlsym(dl_handle, #name);
    LOAD(eglMakeCurrent);
    LOAD(eglDestroyContext);
    LOAD(eglDestroySurface);
    LOAD(eglTerminate);
    LOAD(eglReleaseThread);
    LOAD(eglGetCurrentContext);
    LOAD(eglGetDisplay);
    LOAD(eglInitialize);
    LOAD(eglGetConfigAttrib);
    LOAD(eglCreatePbufferSurface);
    LOAD(eglCreateWindowSurface);
    LOAD(eglSwapBuffers);
    LOAD(eglGetError);
    LOAD(eglSwapInterval);
    LOAD(eglGetCurrentSurface);
    LOAD(eglQuerySurface);
    
    // 这里只赋值，不定义(br_loader.c定义了)
    eglGetProcAddress_p = dlsym(dl_handle, "eglGetProcAddress");

    // 应用 Hooks
    eglChooseConfig_p = hook_eglChooseConfig;
    eglCreateContext_p = hook_eglCreateContext;
    eglBindAPI_p = hook_eglBindAPI;

    printf("egl_loader: System EGL loaded and hooks applied.\n");
    if (eglBindAPI_p) eglBindAPI_p(0);
}

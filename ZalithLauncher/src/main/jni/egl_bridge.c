#include <jni.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <android/dlext.h>
#include <time.h>

// 必须包含环境头文件
#include <environ/environ.h>
#include <EGL/egl.h>

// -------------------------------------------------------------
// Macros & Config
// -------------------------------------------------------------
#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

// 定义渲染器枚举 (防止头文件缺失)
#ifndef RENDERER_GL4ES
#define RENDERER_GL4ES 0
#define RENDERER_VULKAN 1
#define RENDERER_VIRGL 2
#define RENDERER_VK_ZINK 3
#endif

// -------------------------------------------------------------
// 自定义函数指针 (完全独立，不依赖外部)
// -------------------------------------------------------------
static void* lib_egl_handle = NULL;

// 定义我们需要用到的 EGL 函数指针
static EGLDisplay (*sys_eglGetDisplay)(EGLNativeDisplayType) = NULL;
static EGLBoolean (*sys_eglInitialize)(EGLDisplay, EGLint*, EGLint*) = NULL;
static EGLBoolean (*sys_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
static EGLContext (*sys_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = NULL;
static EGLSurface (*sys_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = NULL;
static EGLBoolean (*sys_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = NULL;
static EGLBoolean (*sys_eglSwapBuffers)(EGLDisplay, EGLSurface) = NULL;
static EGLBoolean (*sys_eglSwapInterval)(EGLDisplay, EGLint) = NULL;
static EGLBoolean (*sys_eglBindAPI)(EGLenum) = NULL;
static EGLint     (*sys_eglGetError)(void) = NULL;
static EGLBoolean (*sys_eglDestroySurface)(EGLDisplay, EGLSurface) = NULL;
static EGLBoolean (*sys_eglDestroyContext)(EGLDisplay, EGLContext) = NULL;
static EGLBoolean (*sys_eglTerminate)(EGLDisplay) = NULL;

// 全局状态保存
static EGLDisplay g_Display = EGL_NO_DISPLAY;
static EGLSurface g_Surface = EGL_NO_SURFACE;
static EGLContext g_Context = EGL_NO_CONTEXT;
static EGLConfig  g_Config  = NULL;

// -------------------------------------------------------------
// 1. 加载系统 EGL 驱动
// -------------------------------------------------------------
static int load_system_egl_functions() {
    printf("EGLBridge: Loading System EGL library...\n");
    
    // 尝试加载系统库
    lib_egl_handle = dlopen("libEGL.so", RTLD_LAZY);
    if (!lib_egl_handle) {
        // 尝试 64位 绝对路径
        lib_egl_handle = dlopen("/system/lib64/libEGL.so", RTLD_LAZY);
    }
    if (!lib_egl_handle) {
        // 尝试 32位 绝对路径
        lib_egl_handle = dlopen("/system/lib/libEGL.so", RTLD_LAZY);
    }
    
    if (!lib_egl_handle) {
        printf("EGLBridge: [FATAL] Could not dlopen libEGL.so! Error: %s\n", dlerror());
        return 0;
    }

    // 获取函数地址
    sys_eglGetDisplay = dlsym(lib_egl_handle, "eglGetDisplay");
    sys_eglInitialize = dlsym(lib_egl_handle, "eglInitialize");
    sys_eglChooseConfig = dlsym(lib_egl_handle, "eglChooseConfig");
    sys_eglCreateContext = dlsym(lib_egl_handle, "eglCreateContext");
    sys_eglCreateWindowSurface = dlsym(lib_egl_handle, "eglCreateWindowSurface");
    sys_eglMakeCurrent = dlsym(lib_egl_handle, "eglMakeCurrent");
    sys_eglSwapBuffers = dlsym(lib_egl_handle, "eglSwapBuffers");
    sys_eglSwapInterval = dlsym(lib_egl_handle, "eglSwapInterval");
    sys_eglBindAPI = dlsym(lib_egl_handle, "eglBindAPI");
    sys_eglGetError = dlsym(lib_egl_handle, "eglGetError");
    sys_eglDestroySurface = dlsym(lib_egl_handle, "eglDestroySurface");
    sys_eglDestroyContext = dlsym(lib_egl_handle, "eglDestroyContext");
    sys_eglTerminate = dlsym(lib_egl_handle, "eglTerminate");

    if (!sys_eglGetDisplay || !sys_eglCreateContext) {
        printf("EGLBridge: [FATAL] Could not dlsym essential EGL functions!\n");
        return 0;
    }
    
    printf("EGLBridge: System EGL Loaded Successfully.\n");
    return 1;
}

// -------------------------------------------------------------
// 2. 初始化 EGL
// -------------------------------------------------------------
int pojavInitOpenGL() {
    // 1. 加载函数
    if (!load_system_egl_functions()) return -1;

    // 2. 获取 Display
    g_Display = sys_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_Display == EGL_NO_DISPLAY) {
        printf("EGLBridge: eglGetDisplay failed: 0x%x\n", sys_eglGetError());
        return -1;
    }

    // 3. 初始化 EGL
    if (!sys_eglInitialize(g_Display, NULL, NULL)) {
        printf("EGLBridge: eglInitialize failed: 0x%x\n", sys_eglGetError());
        return -1;
    }

    // 4. 强制绑定 API (GLES)
    if (sys_eglBindAPI) {
        // 0x30A0 = EGL_OPENGL_ES_API
        if (sys_eglBindAPI(0x30A0)) {
            printf("EGLBridge: Bound API to GLES.\n");
        } else {
            printf("EGLBridge: Failed to bind API to GLES.\n");
        }
    }

    printf("EGLBridge: EGL Initialized (Display: %p)\n", g_Display);
    
    // 设置内部状态，防止其他地方空指针
    pojav_environ->config_renderer = RENDERER_GL4ES; 

    return 0;
}

EXTERNAL_API int pojavInit() {
    if (!pojav_environ->pojavWindow) {
        printf("EGLBridge: [ERROR] pojavWindow is NULL.\n");
        return 0;
    }

    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    // 强制 RGBA 8888
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    
    if (pojavInitOpenGL() != 0) {
        return 0; 
    }
    return 1; 
}

// -------------------------------------------------------------
// 3. 创建 Context (手动强制配置)
// -------------------------------------------------------------
EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    (void)contextSrc; // 忽略传入参数，我们自己管
    printf("EGLBridge: pojavCreateContext (Custom Implementation)\n");

    if (g_Display == EGL_NO_DISPLAY) {
        printf("EGLBridge: Display not initialized!\n");
        return NULL;
    }

    // ----------------------------------------------
    // 核心：手写 GLES 配置
    // ----------------------------------------------
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, 0x0004, // EGL_OPENGL_ES2_BIT (兼容 ES3)
        EGL_BLUE_SIZE,       8,
        EGL_GREEN_SIZE,      8,
        EGL_RED_SIZE,        8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      16,     // 16位深度 (兼容性关键)
        EGL_STENCIL_SIZE,    0,
        EGL_NONE
    };

    EGLint num_configs;
    if (!sys_eglChooseConfig(g_Display, attribs, &g_Config, 1, &num_configs) || num_configs == 0) {
        printf("EGLBridge: [FATAL] eglChooseConfig failed or no config found! Error: 0x%x\n", sys_eglGetError());
        return NULL;
    }

    // ----------------------------------------------
    // 核心：创建 GLES 3 上下文
    // ----------------------------------------------
    const EGLint ctx_attribs[] = {
        0x3098, 3, // EGL_CONTEXT_CLIENT_VERSION = 3
        EGL_NONE
    };

    g_Context = sys_eglCreateContext(g_Display, g_Config, EGL_NO_CONTEXT, ctx_attribs);
    if (g_Context == EGL_NO_CONTEXT) {
        printf("EGLBridge: [WARN] Failed to create GLES 3 context (0x%x), retrying GLES 2...\n", sys_eglGetError());
        
        const EGLint ctx_attribs_2[] = { 0x3098, 2, EGL_NONE };
        g_Context = sys_eglCreateContext(g_Display, g_Config, EGL_NO_CONTEXT, ctx_attribs_2);
    }

    if (g_Context == EGL_NO_CONTEXT) {
        printf("EGLBridge: [FATAL] Failed to create any Context.\n");
        return NULL;
    }

    printf("EGLBridge: Context Created Successfully: %p\n", g_Context);

    // ----------------------------------------------
    // 核心：创建 Surface
    // ----------------------------------------------
    g_Surface = sys_eglCreateWindowSurface(g_Display, g_Config, (EGLNativeWindowType)pojav_environ->pojavWindow, NULL);
    if (g_Surface == EGL_NO_SURFACE) {
        printf("EGLBridge: [FATAL] eglCreateWindowSurface failed: 0x%x\n", sys_eglGetError());
        return NULL;
    }

    // 自动 MakeCurrent
    if (!sys_eglMakeCurrent(g_Display, g_Surface, g_Surface, g_Context)) {
         printf("EGLBridge: [FATAL] eglMakeCurrent failed: 0x%x\n", sys_eglGetError());
    }

    // 返回一个非空指针 (GLFW 需要)
    // 通常这里应该返回包含上下文信息的结构体，但在 Pojav 的设计里
    // 如果不使用 br_loader，返回 Context 指针本身通常也能蒙混过关
    // 或者我们只要确保 g_Context 是全局可访问的即可
    return (void*)g_Context; 
}

// -------------------------------------------------------------
// 4. 其他必须实现的接口
// -------------------------------------------------------------

EXTERNAL_API void pojavSwapBuffers() {
    // 简单的 FPS 计算
    static int frameCount = 0;
    static time_t lastTime = 0;
    frameCount++;
    time_t currentTime = time(NULL);
    if (currentTime != lastTime) { lastTime = currentTime; frameCount = 0; }

    if (sys_eglSwapBuffers && g_Display && g_Surface) {
        sys_eglSwapBuffers(g_Display, g_Surface);
    }
}

EXTERNAL_API void pojavMakeCurrent(void* window) {
    (void)window;
    if (sys_eglMakeCurrent && g_Display && g_Surface && g_Context) {
        sys_eglMakeCurrent(g_Display, g_Surface, g_Surface, g_Context);
    }
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    if (sys_eglSwapInterval && g_Display) {
        sys_eglSwapInterval(g_Display, interval);
    }
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    // 忽略所有 Hint，我们已经在 pojavCreateContext 里硬编码了最佳配置
}

EXTERNAL_API void* pojavGetCurrentContext() {
    return (void*)g_Context;
}

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating...\n");
    if (sys_eglMakeCurrent) sys_eglMakeCurrent(g_Display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (sys_eglDestroySurface) sys_eglDestroySurface(g_Display, g_Surface);
    if (sys_eglDestroyContext) sys_eglDestroyContext(g_Display, g_Context);
    if (sys_eglTerminate) sys_eglTerminate(g_Display);
}

// JNI & Vulkan Utils
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
}

EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) {
    return 0; // 简化
}

// Vulkan Loader (Keep minimal)
void load_vulkan() {
    void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
    if (vPtr) {
        char envval[64];
        sprintf(envval, "%"PRIxPTR, (uintptr_t)vPtr);
        setenv("VULKAN_PTR", envval, 1);
    }
}

EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
     if(getenv("VULKAN_PTR") == NULL) load_vulkan();
     const char* ptr = getenv("VULKAN_PTR");
     return ptr ? (jlong)strtoul(ptr, NULL, 0x10) : 0;
}

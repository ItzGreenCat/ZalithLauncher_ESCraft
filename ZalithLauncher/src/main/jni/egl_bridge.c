#include <jni.h>
#include <assert.h>
#include <dlfcn.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GL/osmesa.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"

#ifdef GLES_TEST
#include <GLES2/gl2.h>
#endif

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <string.h>

// [FIX] 必须包含此头文件，否则出现 pojave_environ 未定义错误
#include <environ/environ.h>

#include <android/dlext.h>
#include <time.h>
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

#define GLFW_CLIENT_API 0x22001
/* Consider GLFW_NO_API as Vulkan API */
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
// [ADD] 新增 GLES API 定义
#define GLFW_OPENGL_ES_API 0x30002

// This means that the function is an external API and that it will be used
#define EXTERNAL_API __attribute__((visibility("default"), used))
// This means that you are forced to have this function/variable for ABI compatibility
#define ABI_COMPAT __attribute__((unused))

// [FIX] 显式初始化以防链接错误
EXTERNAL_API EGLConfig config = NULL;
EXTERNAL_API struct PotatoBridge potatoBridge;

// [ADD] 系统库句柄，用于提取函数指针
static void* sys_gles_handle = NULL;
static void* sys_egl_handle = NULL;

void* loadTurnipVulkan();
void calculateFPS();

// -------------------------------------------------------------
// [ADD] 核心补丁：实现函数地址获取器
// LWJGL 必须通过这个函数才能拿到 GLES 的函数地址
// -------------------------------------------------------------
EXTERNAL_API void* pojavGetProcAddress(const char* procname) {
    void* addr = NULL;
    
    // 1. 先尝试从 GLESv2 系统库找 (glGetString, glDrawArrays 等)
    if (sys_gles_handle) {
        addr = dlsym(sys_gles_handle, procname);
    }
    
    // 2. 如果没找到，去 EGL 系统库找
    if (!addr && sys_egl_handle) {
        addr = dlsym(sys_egl_handle, procname);
    }
    
    return addr;
}

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");

    switch (pojav_environ->config_renderer) {
        case RENDERER_GL4ES: {
            eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
            eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
            eglTerminate_p(potatoBridge.eglDisplay);
            eglReleaseThread_p();

            potatoBridge.eglContext = EGL_NO_CONTEXT;
            potatoBridge.eglDisplay = EGL_NO_DISPLAY;
            potatoBridge.eglSurface = EGL_NO_SURFACE;
        } break;

            //case RENDERER_VIRGL:
        case RENDERER_VK_ZINK: {
            // Nothing to do here
        } break;
    }
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}

JNIEXPORT void JNICALL
Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) {
        ANativeWindow_release(pojav_environ->pojavWindow);
    }
}

EXTERNAL_API void* pojavGetCurrentContext() {
    if (pojav_environ->config_renderer == RENDERER_VIRGL)
        return virglGetCurrentContext();

    return br_get_current();
}

static void set_vulkan_ptr(void* ptr) {
    char envval[64];
    sprintf(envval, "%"PRIxPTR, (uintptr_t)ptr);
    setenv("VULKAN_PTR", envval, 1);
}

void load_vulkan() {
    // [MOD] 简单处理，既然我们专注 GLES，这里防止崩溃即可
    // 如果不需要 Vulkan，可以留空或者只做基本的 dlopen
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         if (vPtr) {
             set_vulkan_ptr(vPtr);
         }
    }
}

// -------------------------------------------------------------
// [MOD] 初始化逻辑：强制 System GLES
// -------------------------------------------------------------
int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    // [ADD] 1. 加载系统库句柄 (为了 pojavGetProcAddress)
    // 直接用文件名，让 Android Linker 自动去 /system/lib64 找
    sys_gles_handle = dlopen("libGLESv2.so", RTLD_LAZY);
    sys_egl_handle = dlopen("libEGL.so", RTLD_LAZY);
    
    if (!sys_gles_handle) printf("EGLBridge: Warning - Failed to dlopen libGLESv2.so\n");

    // [ADD] 2. 强制设置环境变量，欺骗 set_gl_bridge_tbl
    // 这告诉官方的加载器：不要加载 libgl4es.so，去加载系统的 libEGL.so
    setenv("LIBGL_EGL", "libEGL.so", 1);
    setenv("LIBGL_GLES", "libGLESv2.so", 1);
    setenv("LIBGL_ES", "libGLESv2.so", 1);
    
    // 清除可能导致干扰的 Mesa 变量
    unsetenv("GALLIUM_DRIVER");
    
    printf("EGLBridge: Forced System GLES Mode. LIBGL_EGL=libEGL.so\n");

    load_vulkan();

    // [MOD] 3. 无条件进入 GL4ES 路径 (但因为环境变量改了，实际上跑的是 System GLES)
    // 这样我们就能复用 br_init 的代码，不用自己写 EGL 初始化逻辑
    pojav_environ->config_renderer = RENDERER_GL4ES;
    set_gl_bridge_tbl();

    // [ADD] 4. 强制绑定 API (防止 EGL 默认 Desktop GL)
    // 动态查找 eglBindAPI 并调用
    if (sys_egl_handle) {
        void (*ptr_eglBindAPI)(EGLenum) = dlsym(sys_egl_handle, "eglBindAPI");
        if (!ptr_eglBindAPI) ptr_eglBindAPI = dlsym(RTLD_DEFAULT, "eglBindAPI");
        
        if (ptr_eglBindAPI) {
            printf("EGLBridge: Calling eglBindAPI(EGL_OPENGL_ES_API)\n");
            ptr_eglBindAPI(0x30A0); // 0x30A0 = EGL_OPENGL_ES_API
        }
    }

    // 5. 调用官方的初始化
    if (br_init()) br_setup_window();

    return 0;
}

EXTERNAL_API int pojavInit() {
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    
    // [MOD] 强制 RGBA 8888，最通用的 GLES 格式
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow,
                                     pojav_environ->savedWidth,
                                     pojav_environ->savedHeight,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    pojavInitOpenGL();
    return 1;
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            break;
        case GLFW_OPENGL_API:
            // 允许 Desktop 请求，但实际上底下已经是 GLES 环境了
            break;
        case GLFW_OPENGL_ES_API:
            // [MOD] 关键修改：不要 abort()，允许 GLES 请求通过
            printf("EGLBridge: Accepted GLFW_OPENGL_ES_API hint.\n");
            break;
        default:
            printf("GLFW: Unimplemented API 0x%x\n", value);
            // abort(); // [MOD] 为了稳健，不要崩溃，只是打印日志
    }
}

EXTERNAL_API void pojavSwapBuffers() {
    calculateFPS();

    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES)
    {
        br_swap_buffers();
    }

    if (pojav_environ->config_renderer == RENDERER_VIRGL)
    {
        virglSwapBuffers();
    }
}

EXTERNAL_API void pojavMakeCurrent(void* window) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES)
    {
        br_make_current((basic_render_window_t*)window);
    }

    if (pojav_environ->config_renderer == RENDERER_VIRGL)
    {
        virglMakeCurrent(window);
    }
}

EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    if (pojav_environ->config_renderer == RENDERER_VULKAN)
        return (void *) pojav_environ->pojavWindow;

    if (pojav_environ->config_renderer == RENDERER_VIRGL)
        return virglCreateContext(contextSrc);

    return br_init_context((basic_render_window_t*)contextSrc);
}

// [MOD] 简单的 Vulkan stub，满足链接需求
EXTERNAL_API void* maybe_load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) load_vulkan();
    const char* ptr = getenv("VULKAN_PTR");
    return ptr ? (void*) strtoul(ptr, NULL, 0x10) : NULL;
}

static int frameCount = 0;
static int fps = 0;
static time_t lastTime = 0;

void calculateFPS() {
    frameCount++;
    time_t currentTime = time(NULL);

    if (currentTime != lastTime) {
        lastTime = currentTime;
        fps = frameCount;
        frameCount = 0;
    }
}

EXTERNAL_API JNIEXPORT jint JNICALL
Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) {
    return fps;
}

EXTERNAL_API JNIEXPORT jlong JNICALL
Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    return (jlong) maybe_load_vulkan();
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES)
    {
        br_swap_interval(interval);
    }

    if (pojav_environ->config_renderer == RENDERER_VIRGL)
    {
        virglSwapInterval(interval);
    }
}

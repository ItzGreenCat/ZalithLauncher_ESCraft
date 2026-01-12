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
#include <environ/environ.h>

#include <EGL/egl.h>
#include "ctxbridges/egl_loader.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/renderer_config.h"
#include "ctxbridges/virgl_bridge.h"
#include "driver_helper/nsbypass.h"
#include "utils.h"
#include "ctxbridges/bridge_tbl.h" // 这里面应该声明了 bridge_tbl
#include "ctxbridges/osm_bridge.h"

// -------------------------------------------------------------
// Macros
// -------------------------------------------------------------
#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

// 初始化全局变量
EXTERNAL_API EGLConfig config = NULL;
struct PotatoBridge potatoBridge;
void* loadTurnipVulkan(); 
void calculateFPS();

// -------------------------------------------------------------
// HOOKS: 拦截并修改 EGL 调用
// -------------------------------------------------------------

// 保存原始函数指针
static EGLBoolean (*real_eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = NULL;
static EGLContext (*real_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = NULL;

// 1. 劫持 eglChooseConfig：强制修改属性为 GLES 兼容
EGLBoolean my_hooked_eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list, EGLConfig* configs, EGLint config_size, EGLint* num_config) {
    printf("EGLBridge: [HOOK] eglChooseConfig intercepted! Fixing attributes...\n");
    
    // 构造一个新的属性列表，最大支持 64 个属性
    EGLint my_attribs[64];
    int i = 0; // 原列表索引
    int j = 0; // 新列表索引
    
    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];
            
            // [MAGIC FIX 1] 强制 Renderable Type 为 ES2/ES3
            if (attr == EGL_RENDERABLE_TYPE) {
                // 不管原来请求的是什么(比如 Desktop GL)，直接改成 ES3 | ES2
                printf("EGLBridge: [HOOK] Patching EGL_RENDERABLE_TYPE: 0x%x -> ES2_BIT\n", val);
                val = 0x0004; // EGL_OPENGL_ES2_BIT (兼容性最好)
                // val = 0x0040; // EGL_OPENGL_ES3_BIT_KHR (也可以尝试)
            }
            
            // [MAGIC FIX 2] 强制 Depth 为 16位
            if (attr == EGL_DEPTH_SIZE) {
                if (val == 24) {
                    printf("EGLBridge: [HOOK] Downgrading Depth 24 -> 16 for compatibility.\n");
                    val = 16;
                }
            }
            
            my_attribs[j++] = attr;
            my_attribs[j++] = val;
            i += 2;
        }
    }
    my_attribs[j] = EGL_NONE;
    
    // 调用真正的系统函数
    if (real_eglChooseConfig) {
        return real_eglChooseConfig(dpy, my_attribs, configs, config_size, num_config);
    }
    return EGL_FALSE;
}

// 2. 劫持 eglCreateContext：强制指定 Client Version
EGLContext my_hooked_eglCreateContext(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint* attrib_list) {
    printf("EGLBridge: [HOOK] eglCreateContext intercepted!\n");
    
    EGLint my_attribs[64];
    int i = 0;
    int j = 0;
    bool has_version = false;

    if (attrib_list) {
        while (attrib_list[i] != EGL_NONE && j < 60) {
            EGLint attr = attrib_list[i];
            EGLint val = attrib_list[i+1];
            
            // 检查是否已经指定了版本
            if (attr == 0x3098) { // EGL_CONTEXT_CLIENT_VERSION
                has_version = true;
                printf("EGLBridge: [HOOK] Context version already requested: %d\n", val);
            }
            
            my_attribs[j++] = attr;
            my_attribs[j++] = val;
            i += 2;
        }
    }
    
    // [MAGIC FIX 3] 如果没指定版本，强制指定为 3.0
    if (!has_version) {
        printf("EGLBridge: [HOOK] Injecting EGL_CONTEXT_CLIENT_VERSION = 3\n");
        my_attribs[j++] = 0x3098; // EGL_CONTEXT_CLIENT_VERSION
        my_attribs[j++] = 3;      // Version 3
    }
    my_attribs[j] = EGL_NONE;

    if (real_eglCreateContext) {
        EGLContext ctx = real_eglCreateContext(dpy, config, share_context, my_attribs);
        if (ctx == EGL_NO_CONTEXT) {
             printf("EGLBridge: [HOOK] Context creation failed (Error: 0x%x). Retrying with Version 2...\n");
             // 如果 3.0 失败，尝试 2.0
             if (!has_version) {
                 my_attribs[j-1] = 2; 
                 ctx = real_eglCreateContext(dpy, config, share_context, my_attribs);
             }
        }
        return ctx;
    }
    return EGL_NO_CONTEXT;
}

// -------------------------------------------------------------
// Core Logic
// -------------------------------------------------------------

static void force_system_gles_drivers() {
    printf("EGLBridge: [INFO] Configuring for System GLES Drivers...\n");

    const char* lib_egl = "libEGL.so";
    const char* lib_gles = "libGLESv2.so";

    setenv("LIBGL_EGL", lib_egl, 1);
    setenv("LIBGL_GLES", lib_gles, 1);
    setenv("LIBGL_ES", lib_gles, 1);
    
    unsetenv("GALLIUM_DRIVER"); 
    unsetenv("MESA_LOADER_DRIVER_OVERRIDE");

    // 加载系统库符号到 bridge_tbl
    set_gl_bridge_tbl();
    
    // ==========================================
    // [CRITICAL] 应用 Hooks 到 bridge_tbl
    // ==========================================
    // 假设 bridge_tbl 是全局可见的 (Pojav 标准设计)
    // 如果编译报错说 bridge_tbl 未定义，请告诉我，我再换一种写法
    if (bridge_tbl.eglChooseConfig) {
        real_eglChooseConfig = bridge_tbl.eglChooseConfig; // 保存原指针
        bridge_tbl.eglChooseConfig = my_hooked_eglChooseConfig; // 替换为 Hook
        printf("EGLBridge: [SUCCESS] Hooked eglChooseConfig\n");
    }
    
    if (bridge_tbl.eglCreateContext) {
        real_eglCreateContext = bridge_tbl.eglCreateContext;
        bridge_tbl.eglCreateContext = my_hooked_eglCreateContext;
        printf("EGLBridge: [SUCCESS] Hooked eglCreateContext\n");
    }
}

static void force_bind_gles_api() {
    typedef EGLBoolean (*eglBindAPI_t)(EGLenum);
    eglBindAPI_t ptr_eglBindAPI = (eglBindAPI_t)dlsym(RTLD_DEFAULT, "eglBindAPI");
    
    if (!ptr_eglBindAPI) {
        void* handle = dlopen("libEGL.so", RTLD_LAZY);
        if (handle) ptr_eglBindAPI = (eglBindAPI_t)dlsym(handle, "eglBindAPI");
    }

    if (ptr_eglBindAPI) {
        printf("EGLBridge: FORCE calling eglBindAPI(EGL_OPENGL_ES_API)...\n");
        if (ptr_eglBindAPI(0x30A0)) {
            printf("EGLBridge: API Bind Success.\n");
        } else {
            printf("EGLBridge: API Bind Failed.\n");
        }
    }
}

void load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) {
         void* vPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
         if (vPtr) {
             char envval[64];
             sprintf(envval, "%"PRIxPTR, (uintptr_t)vPtr);
             setenv("VULKAN_PTR", envval, 1);
         }
    }
}

EXTERNAL_API void* maybe_load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) load_vulkan();
    const char* ptrStr = getenv("VULKAN_PTR");
    if (ptrStr == NULL) return NULL;
    return (void*) strtoul(ptrStr, NULL, 0x10);
}

// -------------------------------------------------------------
// Initialization
// -------------------------------------------------------------

int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    maybe_load_vulkan();

    pojav_environ->config_renderer = RENDERER_GL4ES;
    
    // 1. 设置驱动并应用 Hook
    force_system_gles_drivers();
    
    // 2. 初始化 EGL
    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: [FATAL] br_init() failed!\n");
        return -1;
    }

    // 3. 绑定 API
    force_bind_gles_api();

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
    
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, 
                                     pojav_environ->savedWidth, 
                                     pojav_environ->savedHeight, 
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    
    if (pojavInitOpenGL() != 0) {
        return 0; 
    }
    return 1; 
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    // 忽略所有 Hint，因为我们已经通过 Hook 强制接管了
}

EXTERNAL_API void* pojavCreateContext(void* contextSrc) {
    printf("EGLBridge: pojavCreateContext called...\n");
    void* ctx = br_init_context((basic_render_window_t*)contextSrc);
    if (!ctx) {
        printf("EGLBridge: [FATAL] pojavCreateContext returned NULL. Hook failed?\n");
    }
    return ctx;
}

// 标准函数
EXTERNAL_API void pojavTerminate() {
    if (potatoBridge.eglDisplay) {
        eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (potatoBridge.eglSurface) eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
        if (potatoBridge.eglContext) eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
        eglTerminate_p(potatoBridge.eglDisplay);
        eglReleaseThread_p();
    }
    memset(&potatoBridge, 0, sizeof(potatoBridge));
}
EXTERNAL_API void pojavSwapBuffers() { calculateFPS(); br_swap_buffers(); }
EXTERNAL_API void pojavMakeCurrent(void* window) { br_make_current((basic_render_window_t*)window); }
EXTERNAL_API void pojavSwapInterval(int interval) { br_swap_interval(interval); }
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}
JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
}
EXTERNAL_API void* pojavGetCurrentContext() { return br_get_current(); }
static int frameCount = 0; static int fps = 0; static time_t lastTime = 0;
void calculateFPS() {
    frameCount++; time_t currentTime = time(NULL);
    if (currentTime != lastTime) { lastTime = currentTime; fps = frameCount; frameCount = 0; }
}
EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) { return fps; }
EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    return (jlong) maybe_load_vulkan();
}

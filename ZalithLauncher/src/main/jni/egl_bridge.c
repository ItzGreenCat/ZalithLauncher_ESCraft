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
// 确保包含这个，否则 pojav_environ 报错
#include <environ/environ.h>
#include <android/dlext.h>
#include <time.h>
#include "utils.h"
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"

#define GLFW_CLIENT_API 0x22001
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001
// [MOD] 添加 GLES API 定义
#define GLFW_OPENGL_ES_API 0x30002

#define EXTERNAL_API __attribute__((visibility("default"), used))
#define ABI_COMPAT __attribute__((unused))

// [MOD] 初始化全局变量以防链接错误
EXTERNAL_API EGLConfig config = NULL;
struct PotatoBridge potatoBridge;

void* loadTurnipVulkan();
void calculateFPS();

// =============================================================
// Helper: 强制绑定 GLES API
// =============================================================
static void force_gles_binding() {
    // 尝试获取 eglBindAPI 地址
    // 注意：这里假设库已经通过 set_gl_bridge_tbl 加载
    void (*ptr_eglBindAPI)(EGLenum) = dlsym(RTLD_DEFAULT, "eglBindAPI");
    
    // 如果找不到，尝试手动 dlopen 系统库
    if (!ptr_eglBindAPI) {
        void* handle = dlopen("libEGL.so", RTLD_LAZY);
        if (handle) ptr_eglBindAPI = dlsym(handle, "eglBindAPI");
    }

    if (ptr_eglBindAPI) {
        printf("EGLBridge: Force calling eglBindAPI(EGL_OPENGL_ES_API)...\n");
        // EGL_OPENGL_ES_API = 0x30A0
        if (ptr_eglBindAPI(0x30A0)) {
            printf("EGLBridge: API Bind Success!\n");
        } else {
            printf("EGLBridge: API Bind Failed!\n");
        }
    } else {
        printf("EGLBridge: Warning: eglBindAPI symbol not found.\n");
    }
}

// ... (pojavTerminate, JNIEXPORTs 保持不变) ...
EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");
    switch (pojav_environ->config_renderer) {
        case RENDERER_GL4ES: {
            if (potatoBridge.eglDisplay) {
                eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (potatoBridge.eglSurface) eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
                if (potatoBridge.eglContext) eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
                eglTerminate_p(potatoBridge.eglDisplay);
                eglReleaseThread_p();
            }
            potatoBridge.eglContext = EGL_NO_CONTEXT;
            potatoBridge.eglDisplay = EGL_NO_DISPLAY;
            potatoBridge.eglSurface = EGL_NO_SURFACE;
        } break;
        case RENDERER_VK_ZINK: { } break;
    }
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_setupBridgeWindow(JNIEnv* env, ABI_COMPAT jclass clazz, jobject surface) {
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (br_setup_window) br_setup_window();
}

JNIEXPORT void JNICALL Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass clazz) {
    if (pojav_environ->pojavWindow) ANativeWindow_release(pojav_environ->pojavWindow);
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
    // ... (保留原版 Vulkan 加载逻辑) ...
    const char* zinkPreferSystemDriver = getenv("POJAV_ZINK_PREFER_SYSTEM_DRIVER");
    int deviceApiLevel = android_get_device_api_level();
    if (zinkPreferSystemDriver == NULL && deviceApiLevel >= 28) {
#ifdef ADRENO_POSSIBLE
        void* result = loadTurnipVulkan();
        if (result != NULL) {
            printf("AdrenoSupp: Loaded Turnip, loader address: %p\n", result);
            set_vulkan_ptr(result);
            return;
        }
#endif
    }
    printf("OSMDroid: Loading Vulkan regularly...\n");
    void* vulkanPtr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
    printf("OSMDroid: Loaded Vulkan, ptr=%p\n", vulkanPtr);
    set_vulkan_ptr(vulkanPtr);
}

// -------------------------------------------------------------
// [MOD] 核心初始化逻辑修改
// -------------------------------------------------------------
int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    const char *renderer = getenv("POJAV_RENDERER");
    
    // [MOD] 默认回退到 system_gles
    if (renderer == NULL || renderer[0] == '\0') renderer = "system_gles";

    load_vulkan();

    // [MOD] 新增系统驱动模式
    if (!strcmp(renderer, "system_gles"))
    {
        printf("EGLBridge: >>> Using SYSTEM GLES Mode <<<\n");
        pojav_environ->config_renderer = RENDERER_GL4ES;

        // 1. 设置系统库路径 (使用相对路径让 Linker 自动查找)
        setenv("LIBGL_EGL", "libEGL.so", 1);
        setenv("LIBGL_GLES", "libGLESv2.so", 1);
        setenv("LIBGL_ES", "libGLESv2.so", 1);
        
        // 2. 加载符号表
        set_gl_bridge_tbl();
        
        // 3. 强制绑定 API (关键步骤)
        force_gles_binding();
    }
    else if (!strncmp("opengles", renderer, 8))
    {
        pojav_environ->config_renderer = RENDERER_GL4ES;
        set_gl_bridge_tbl();
    }
    // ... (其他原版分支保持不变) ...
    else if (!strcmp(renderer, "custom_gallium")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        set_osm_bridge_tbl();
    }
    else if (!strcmp(renderer, "vulkan_zink")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        load_vulkan();
        setenv("GALLIUM_DRIVER", "zink", 1);
        set_osm_bridge_tbl();
    }
    else if (!strcmp(renderer, "gallium_freedreno")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 1);
        setenv("GALLIUM_DRIVER", "freedreno", 1);
        set_osm_bridge_tbl();
    }
    else if (!strcmp(renderer, "gallium_panfrost")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        setenv("GALLIUM_DRIVER", "panfrost", 1);
        setenv("MESA_DISK_CACHE_SINGLE_FILE", "1", 1);
        set_osm_bridge_tbl();
    }
    else if (!strcmp(renderer, "gallium_virgl")) {
        pojav_environ->config_renderer = RENDERER_VIRGL;
        setenv("GALLIUM_DRIVER", "virpipe", 1);
        // ... (原版 VirGL 逻辑) ...
        loadSymbolsVirGL();
        virglInit();
        return 0;
    }

    if (br_init()) {
        br_setup_window();
    } else {
        printf("EGLBridge: br_init failed!\n");
    }

    return 0;
}

EXTERNAL_API int pojavInit() {
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    // [NOTE] 使用通用 RGBA8888 格式
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow,pojav_environ->savedWidth,pojav_environ->savedHeight,AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    pojavInitOpenGL();
    return 1;
}

// -------------------------------------------------------------
// [MOD] 修复 Window Hint 崩溃
// -------------------------------------------------------------
EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            break;
        case GLFW_OPENGL_API:
            // 原版这里是空，意味着允许 Desktop GL
            break;
        case GLFW_OPENGL_ES_API:
            // [MOD] 允许 ES 请求，不要 abort()
            printf("EGLBridge: Allowing GLES API request.\n");
            break;
        default:
            printf("GLFW: Unimplemented API 0x%x\n", value);
            // abort(); // [MOD] 建议注释掉 abort，防止误杀
    }
}

EXTERNAL_API void pojavSwapBuffers() {
    calculateFPS();
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK || pojav_environ->config_renderer == RENDERER_GL4ES) {
        br_swap_buffers();
    }
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
        virglSwapBuffers();
    }
}

EXTERNAL_API void pojavMakeCurrent(void* window) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK || pojav_environ->config_renderer == RENDERER_GL4ES) {
        br_make_current((basic_render_window_t*)window);
    }
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
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

// ... (Maybe load vulkan & FPS 逻辑保持不变) ...
void* maybe_load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) load_vulkan();
    const char* ptrStr = getenv("VULKAN_PTR");
    if(!ptrStr) return NULL; 
    return (void*) strtoul(ptrStr, NULL, 0x10);
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

EXTERNAL_API JNIEXPORT jint JNICALL Java_org_lwjgl_glfw_CallbackBridge_getCurrentFps(JNIEnv *env, jclass clazz) {
    return fps;
}

// [MOD] 强制导出 maybe_load_vulkan 以防链接错误
EXTERNAL_API JNIEXPORT jlong JNICALL Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    printf("EGLBridge: LWJGL-side Vulkan loader requested the Vulkan handle\n");
    return (jlong) maybe_load_vulkan();
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK || pojav_environ->config_renderer == RENDERER_GL4ES) {
        br_swap_interval(interval);
    }
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
        virglSwapInterval(interval);
    }
}

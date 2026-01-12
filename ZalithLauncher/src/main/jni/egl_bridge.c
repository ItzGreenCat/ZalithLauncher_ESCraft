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
#define EXTERNAL_API __attribute__((used))
// This means that you are forced to have this function/variable for ABI compatibility
#define ABI_COMPAT __attribute__((unused))

EGLConfig config;
struct PotatoBridge potatoBridge;

void* loadTurnipVulkan();
void calculateFPS();

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
    ANativeWindow_release(pojav_environ->pojavWindow);
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
    const char* zinkPreferSystemDriver = getenv("POJAV_ZINK_PREFER_SYSTEM_DRIVER");
    int deviceApiLevel = android_get_device_api_level();
    if (zinkPreferSystemDriver == NULL && deviceApiLevel >= 28) {
#ifdef ADRENO_POSSIBLE
        void* result = loadTurnipVulkan();
        if (result != NULL)
        {
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

int pojavInitOpenGL() {
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (forceVsync && !strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    // 获取渲染器配置
    const char *renderer = getenv("POJAV_RENDERER");
    
    // [MODIFIED] 如果未设置渲染器，默认回退到 system_gles (原生GLES模式)
    if (renderer == NULL || renderer[0] == '\0') {
        renderer = "system_gles";
    }

    load_vulkan();

    // -------------------------------------------------------------
    // [ADD] 纯净系统 GLES 模式 (Qualcomm/Mali/PowerVR)
    // -------------------------------------------------------------
    if (!strcmp(renderer, "system_gles"))
    {
        printf("EGLBridge: Selecting SYSTEM GLES driver mode...\n");
        
        // 我们复用 GL4ES 的逻辑路径，因为本质都是基于 EGL 的上下文管理
        pojav_environ->config_renderer = RENDERER_GL4ES;

        // 强制设置环境变量指向 Android 系统库 (arm64 路径)
        // 这一步告诉底层的 egl_loader.c 去哪里 dlopen
        setenv("LIBGL_EGL", "/system/lib64/libEGL.so", 1);
        setenv("LIBGL_GLES", "/system/lib64/libGLESv2.so", 1);
        setenv("LIBGL_ES", "/system/lib64/libGLESv2.so", 1); // 部分 loader 可能检查这个
        
        // 加载函数指针表
        // 注意：这里我们使用 set_gl_bridge_tbl，它会去 dlsym 上面指定的系统库
        set_gl_bridge_tbl();
        
        printf("EGLBridge: System drivers configured (/system/lib64).\n");
    }
    // -------------------------------------------------------------
    // 传统的 gl4es 模式 (用于翻译 Desktop GL)
    else if (!strncmp("opengles", renderer, 8))
    {
        pojav_environ->config_renderer = RENDERER_GL4ES;
        // 注意：这里通常依赖 Java 层设置 LIBGL_EGL 指向 libgl4es.so
        set_gl_bridge_tbl();
    }

    else if (!strcmp(renderer, "custom_gallium"))
    {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        set_osm_bridge_tbl();
    }

    else if (!strcmp(renderer, "vulkan_zink"))
    {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        load_vulkan();
        setenv("GALLIUM_DRIVER", "zink", 1);
        set_osm_bridge_tbl();
    }

    else if (!strcmp(renderer, "gallium_freedreno"))
    {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 1);
        setenv("GALLIUM_DRIVER", "freedreno", 1);
        set_osm_bridge_tbl();
    }

    else if (!strcmp(renderer, "gallium_panfrost"))
    {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        setenv("GALLIUM_DRIVER", "panfrost", 1);
        setenv("MESA_DISK_CACHE_SINGLE_FILE", "1", 1);
        set_osm_bridge_tbl();
    }

    else if (!strcmp(renderer, "gallium_virgl"))
    {
        pojav_environ->config_renderer = RENDERER_VIRGL;
        setenv("GALLIUM_DRIVER", "virpipe", 1);
        setenv("OSMESA_NO_FLUSH_FRONTBUFFER", "1", false);
        setenv("MESA_GL_VERSION_OVERRIDE", "4.3", 1);
        setenv("MESA_GLSL_VERSION_OVERRIDE", "430", 1);
        if (getenv("OSMESA_NO_FLUSH_FRONTBUFFER") && !strcmp(getenv("OSMESA_NO_FLUSH_FRONTBUFFER"), "1"))
            printf("VirGL: OSMesa buffer flush is DISABLED!\n");
        loadSymbolsVirGL();
        virglInit();
        return 0;
    }

    if (br_init()) br_setup_window();

    return 0;
}

EXTERNAL_API int pojavInit() {
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow,pojav_environ->savedWidth,pojav_environ->savedHeight,AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    pojavInitOpenGL();
    return 1;
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            /* Nothing to do: initialization is handled in Java-side */
            // pojavInitVulkan();
            break;
        case GLFW_OPENGL_API:
        case GLFW_OPENGL_ES_API: // [MODIFIED] 允许 ES API 请求
            /* * LWJGL 如果请求 GLES，会发送 0x30002。
             * 我们在这里捕获它，并让它继续走 EGL 初始化流程。
             * 上下文版本由 br_init_context 里的 eglCreateContext 属性决定。
             */
            break;
        default:
            printf("GLFW: Unimplemented API 0x%x\n", value);
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

void* maybe_load_vulkan() {
    if(getenv("VULKAN_PTR") == NULL) load_vulkan();
    return (void*) strtoul(getenv("VULKAN_PTR"), NULL, 0x10);
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
    printf("EGLBridge: LWJGL-side Vulkan loader requested the Vulkan handle\n");
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

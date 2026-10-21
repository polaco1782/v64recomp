#include "renderer.h"
#include "input.h"
#include "hle/rt64_application.h"
#include "hle/rt64_workload_queue.h"
#include "hle/rt64_present_queue.h"
#include "rhi/rt64_render_hooks.h"
#include "imgui.h"
#include "textures.h"
#include "cheat_menu.h"
#ifdef RECOMP_DEBUG
#include "debug_overlay.h"
#endif
#include "../texture_scan/scanner.h"
#include "../texture_scan/capture.h"
#define Status int
#include <X11/Xutil.h>
#undef Status
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <stdexcept>
#include <unistd.h>

struct RecompRenderer {
    Runtime *rt;
    uint32_t code=0,data=0,list=0;
    RecompGraphicsHook graphics_hook=nullptr;
    RecompMicrocodeHook microcode_hook=nullptr;
    bool custom_gbi=false;
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<RT64::Application> app;
    std::unique_ptr<RecompCheatMenu> cheat_menu;
    SDL_EventFilter previous_event_filter=nullptr;
    void *previous_event_userdata=nullptr;
#ifdef RECOMP_DEBUG
    std::unique_ptr<RecompDebugOverlay> debug_overlay;
    RecompDebugGeometryStats debug_geometry;
    uint64_t debug_workload_id=0;
#endif
    std::array<uint8_t,64> header{};
    std::array<uint8_t,4096> dmem{},imem{};
    uint32_t mi=0, dp_start=0,dp_end=0,dp_current=0,dp_status=0,dp_clock=0,dp_busy=0,dp_pipe=0,dp_tmem=0;
    RecompViRegisters vi{};
    RecompRendererStats stats{};
    FILE *frame_trace=nullptr;
    std::atomic<uint64_t> display_frames{0}, first_display_ns{0}, last_display_ns{0};
    RecompTextureFiles textureFiles;
    std::unique_ptr<RecompTextures::Scanner> textureScan;
    std::unique_ptr<RecompTextures::CaptureWriter> textureCapture;
    uint64_t token=0;
    bool sp_seen=false,dp_seen=false;
    bool ready=false;
    SDL_Window *window=nullptr;
    std::unique_ptr<RecompInput,decltype(&recomp_input_destroy)> input{nullptr,recomp_input_destroy};
    explicit RecompRenderer(Runtime *runtime):rt(runtime) {}
    void check_owner() {
        if(owner!=std::this_thread::get_id())recomp_fail("RT64 callback on a non-owner thread",0);
    }
    void wait_workers() {
        app->workloadQueue->waitForWorkloadId(app->state->workloadId);
        app->presentQueue->waitForPresentId(app->state->presentId);
    }
    void wait_texture_reload() {
        wait_workers();
        // A workload ID is signaled after its first rendered frame. Remaining
        // interpolated frames can still hold the old texture snapshot. Match
        // RT64's resource-reload barrier before rebuilding the replacement map.
        app->workloadQueue->waitForIdle();
        app->presentQueue->waitForIdle();
    }
    void sync_vi() { (void)recomp_vi_registers(rt,&vi); }
};
static RecompRenderer *active_renderer;
static uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void trace_frame(RecompRenderer *s,const char *event,uint64_t value=0) {
    if(s->frame_trace && std::fprintf(s->frame_trace,"%llu,%s,%llu\n",
        static_cast<unsigned long long>(monotonic_ns()),event,static_cast<unsigned long long>(value))<0)
        recomp_fail("cannot write renderer frame trace",0);
}
static void display_draw(plume::RenderCommandList *list,plume::RenderFramebuffer *) {
    RecompRenderer *s=active_renderer;
    uint64_t now=monotonic_ns();
    if(s->display_frames.load()==0)s->first_display_ns=now;
    s->last_display_ns=now;s->display_frames++;
    trace_frame(s,"display");
#ifdef RECOMP_DEBUG
    if(s->debug_overlay)s->debug_overlay->draw(list,s->display_frames.load(),now);
#endif
    if(s->cheat_menu)s->cheat_menu->draw(list);
}
static int menu_event_filter(void *opaque,SDL_Event *event) {
    auto *s=static_cast<RecompRenderer *>(opaque);
    // F12 must reach the shared menu even when RT64's inspector captures keys.
    // While open, route UI events to the owner instead of the inspector.
    if((event->type==SDL_KEYDOWN || event->type==SDL_KEYUP) && event->key.keysym.scancode==SDL_SCANCODE_F12)return 1;
    if(s->cheat_menu && s->cheat_menu->open())return 1;
    return s->previous_event_filter?s->previous_event_filter(s->previous_event_userdata,event):1;
}
static void interrupts() {
    RecompRenderer *s=active_renderer;
    if(!s)recomp_fail("RT64 interrupt without renderer",0);
    s->check_owner();
    uint32_t bits=s->mi&0x21u;s->mi&=~bits;
    if(!bits)return;
    if(!s->token)recomp_fail("RT64 interrupt without task token",0);
    if(bits&0x20) {
        if(s->dp_seen)
            recomp_fail("invalid RT64 DP receipt",0);
        s->dp_seen=true;s->stats.dp_callbacks++;
#ifdef RECOMP_DEBUG
        // Full sync has flushed every split DL into this submitted workload.
        // The parser owns the ring cursor; these descriptors are read-only to
        // the rendering workers. Ignore repeats while the inspector is paused.
        const auto &workload=s->app->workloadQueue->workloads[s->app->state->lastWorkloadIndex];
        if(workload.workloadId!=s->debug_workload_id) {
            s->debug_geometry=recomp_debug_geometry(workload);
            s->debug_workload_id=workload.workloadId;
        }
#endif
    }
    if(bits&1) {
        if(s->sp_seen)
            recomp_fail("invalid RT64 SP receipt",0);
        s->sp_seen=true;s->stats.sp_callbacks++;
    }
}

static void process_graphics(RecompRenderer *s) {
    uint64_t start=monotonic_ns();
    trace_frame(s,"graphics_begin",s->code);
    bool custom=s->microcode_hook && s->microcode_hook(s->app->interpreter.get(),s->rt->rdram_size,s->code,s->data);
    if(!custom && s->custom_gbi) {
        s->app->interpreter->UCode.textAddress=UINT32_MAX;
        s->app->interpreter->UCode.dataAddress=UINT32_MAX;
    }
    if(custom && (!s->app->interpreter->hleGBI || s->app->interpreter->UCode.textAddress!=s->code
                   || s->app->interpreter->UCode.dataAddress!=s->data))
        recomp_fail("custom graphics microcode binding is incomplete",s->code);
    s->custom_gbi=custom;
    // Quake alternates F3DEX.NoN and F3DLP.Rej. Let RT64 select the cached
    // decoder and its variant flags; it prints diagnostics if lookup fails.
    s->app->interpreter->loadUCodeGBI(s->code,s->data,true);
    if(!s->app->interpreter->hleGBI)recomp_fail("RT64 did not recognize the graphics microcode",s->code);
    if(s->graphics_hook)s->graphics_hook(s->app->interpreter.get(),s->rt->rdram_size,&s->stats.rdp_triangles);
    if(s->textureCapture)s->textureCapture->beginTask();
    s->app->processDisplayLists(s->rt->rdram,s->list,0,true);
    s->textureFiles.collect(*s->app->textureCache);
    s->stats.submit_ns+=monotonic_ns()-start;
    trace_frame(s,"graphics_end",s->dp_seen);
}
static void process_present(RecompRenderer *s) {
    uint64_t start=monotonic_ns();
    trace_frame(s,"present_begin",s->vi.origin);
    // Capture live framebuffer bytes before resuming the guest. RT64's render
    // and present queues consume their own storage after this returns.
    s->app->screenApiProfiler.logAndRestart();
    s->app->state->updateScreen(s->app->core.decodeVI(),false);
    s->stats.presentations++;s->stats.present_ns+=monotonic_ns()-start;
    trace_frame(s,"present_end");
}

RecompRenderer *recomp_renderer_create_named(Runtime *rt,const char *data_path,const char *window_title) {
    if(active_renderer || !rt->rom || rt->rdram_size>8*1024*1024 || !data_path)return nullptr;
    if(!XInitThreads())return nullptr;
    SDL_SetHint(SDL_HINT_VIDEODRIVER,"x11");
    if(SDL_InitSubSystem(SDL_INIT_VIDEO|SDL_INIT_EVENTS)!=0) {
        std::fprintf(stderr,"RT64 requires an X11 desktop: %s\n",SDL_GetError());return nullptr;
    }
    auto s=std::make_unique<RecompRenderer>(rt);
    s->window=SDL_CreateWindow(window_title?window_title:"Native N64 recompilation",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
                               1280,720,SDL_WINDOW_RESIZABLE);
    SDL_SysWMinfo wm{};SDL_VERSION(&wm.version);
    if(!s->window || !SDL_GetWindowWMInfo(s->window,&wm) || wm.subsystem!=SDL_SYSWM_X11) {
        std::fprintf(stderr,"RT64 X11 window unavailable: %s\n",SDL_GetError());
        if(s->window)SDL_DestroyWindow(s->window);
        SDL_Quit();return nullptr;
    }
    for(unsigned n=0;n<64;n++)s->header[n^3]=rt->rom[n];
    RT64::Application::Core core{};
    core.window.display=wm.info.x11.display;core.window.window=wm.info.x11.window;
    core.HEADER=s->header.data();core.RDRAM=rt->rdram;core.DMEM=s->dmem.data();core.IMEM=s->imem.data();
    core.MI_INTR_REG=&s->mi;core.DPC_START_REG=&s->dp_start;core.DPC_END_REG=&s->dp_end;
    core.DPC_CURRENT_REG=&s->dp_current;core.DPC_STATUS_REG=&s->dp_status;core.DPC_CLOCK_REG=&s->dp_clock;
    core.DPC_BUFBUSY_REG=&s->dp_busy;core.DPC_PIPEBUSY_REG=&s->dp_pipe;core.DPC_TMEM_REG=&s->dp_tmem;
    core.VI_STATUS_REG=&s->vi.status;core.VI_ORIGIN_REG=&s->vi.origin;core.VI_WIDTH_REG=&s->vi.width;
    core.VI_INTR_REG=&s->vi.intr;core.VI_V_CURRENT_LINE_REG=&s->vi.current_line;core.VI_TIMING_REG=&s->vi.burst;
    core.VI_V_SYNC_REG=&s->vi.v_sync;core.VI_H_SYNC_REG=&s->vi.h_sync;core.VI_LEAP_REG=&s->vi.leap;
    core.VI_H_START_REG=&s->vi.h_start;core.VI_V_START_REG=&s->vi.v_start;core.VI_V_BURST_REG=&s->vi.v_burst;
    core.VI_X_SCALE_REG=&s->vi.x_scale;core.VI_Y_SCALE_REG=&s->vi.y_scale;core.checkInterrupts=interrupts;
    RT64::ApplicationConfiguration config;
    config.appId="n64-native-recomp";config.detectDataPath=false;config.useConfigurationFile=false;config.dataPath=data_path;
    active_renderer=s.get();
    RT64::SetRenderHooks(nullptr,display_draw,nullptr);
    try {
        s->app=std::make_unique<RT64::Application>(core,config);
#ifdef RECOMP_DEBUG
        s->app->userConfig.developerMode=true;
#else
        s->app->userConfig.developerMode=false;
#endif
        s->app->userConfig.graphicsAPI=RT64::UserConfiguration::GraphicsAPI::Vulkan;
        s->app->userConfig.resolution=RT64::UserConfiguration::Resolution::Manual;
        s->app->userConfig.resolutionMultiplier=4.0;
        s->app->userConfig.antialiasing=RT64::UserConfiguration::Antialiasing::MSAA4X;
        s->app->userConfig.threePointFiltering=false;
        s->app->userConfig.aspectRatio=RT64::UserConfiguration::AspectRatio::Expand;
        s->app->userConfig.extAspectRatio=RT64::UserConfiguration::AspectRatio::Expand;
        s->app->userConfig.refreshRate=RT64::UserConfiguration::RefreshRate::Manual;
        s->app->userConfig.refreshRateTarget=60;
        s->app->enhancementConfig.presentation.mode=RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        s->app->enhancementConfig.textureLOD.scale=true;
        if(s->app->setup(0)!=RT64::Application::SetupResult::Success) {
            std::fprintf(stderr,"RT64 setup failed: %s\n",RT64::GlobalLastError.c_str());
            s->app.reset();RT64::SetRenderHooks(nullptr,nullptr,nullptr);
            active_renderer=nullptr;SDL_DestroyWindow(s->window);SDL_Quit();return nullptr;
        }
        s->ready=true;
        if(const char *path=std::getenv("RECOMP_FRAME_TRACE");path && *path) {
            s->frame_trace=std::fopen(path,"wx");
            if(!s->frame_trace)recomp_fail("cannot create renderer frame trace",0);
            std::fprintf(s->frame_trace,"timestamp_ns,event,value\n");
        }
        // The app clock paces this serialized adapter. Waiting again on the
        // compositor can block guest VI/audio work while acquiring an image.
        s->app->swapChain->setVsyncEnabled(false);
        s->app->presentQueue->presentWaitEnabled=false;
        s->app->appWindow->sdlWindow=s->window;
#ifdef RECOMP_DEBUG
        s->debug_overlay=std::make_unique<RecompDebugOverlay>(*s->app);
        s->app->appWindow->sdlCheckFilterInstallation();
#endif
        s->cheat_menu=std::make_unique<RecompCheatMenu>(*s->app,rt,data_path);
        SDL_GetEventFilter(&s->previous_event_filter,&s->previous_event_userdata);
        SDL_SetEventFilter(menu_event_filter,s.get());
        s->textureFiles.configure(*s->app->textureCache);
        if (const char *source = std::getenv("RECOMP_TEXTURE_SCAN"); source && *source) {
            const char *raw = std::getenv("RECOMP_TEXTURE_SCAN_RAW");
            const char *replacements = std::getenv("RECOMP_TEXTURE_REPLACEMENTS");
            if (!raw || !*raw || !replacements || !*replacements)
                throw std::runtime_error("texture scan paths are not configured");
            std::string title(reinterpret_cast<const char *>(rt->rom + 32), 20);
            while (!title.empty() && (title.back() == ' ' || title.back() == '\0')) title.pop_back();
            s->textureScan=std::make_unique<RecompTextures::Scanner>(RecompTextures::ScanConfig{source,raw,replacements,title});
            s->textureCapture=std::make_unique<RecompTextures::CaptureWriter>(raw);
            s->app->state->textureManager.captureTexture=[capture=s->textureCapture.get(),s=s.get()](
                    uint64_t hash,RT64::State *state,const RT64::LoadTile &tile,uint16_t width,uint16_t height,uint32_t tlut) {
                const auto &load=state->rdp->rice.lastLoadOpByTMEM[tile.tmem];
                const auto &palette=state->rdp->rice.lastLoadOpByTMEM[256+(tile.siz==G_IM_SIZ_4b?tile.palette*16:0)];
                return capture->capture(hash,reinterpret_cast<const uint8_t *>(state->rdp->TMEM),state->RDRAM,
                                        s->rt->rdram_size,tile,load,palette,width,height,tlut);
            };
            s->textureScan->start();
            std::fprintf(stderr,"Texture scan: %s; pack output %s/textures.rtz; matching every %u seconds.\n",
                         source,replacements,RecompTextures::ScanIntervalSeconds);
        }
#ifdef RECOMP_DEBUG
        if (const char *path = std::getenv("RECOMP_TEXTURE_RAW_DUMP"); path && *path && !s->textureScan) {
            std::filesystem::create_directories(path);
            s->app->state->dumpingTexturesDirectory=path;
            std::fprintf(stderr,"Texture raw dump: %s\n",path);
        }
#endif
        s->input.reset(recomp_input_create());
        return s.release();
    } catch(const std::exception &e) {
        std::fprintf(stderr,"RT64 setup: %s\n",e.what());
        SDL_SetEventFilter(nullptr,nullptr);
        s->cheat_menu.reset();
#ifdef RECOMP_DEBUG
        s->debug_overlay.reset();
#endif
        s->app.reset();RT64::SetRenderHooks(nullptr,nullptr,nullptr);active_renderer=nullptr;
        if(s->frame_trace)std::fclose(s->frame_trace);
        SDL_DestroyWindow(s->window);SDL_Quit();return nullptr;
    }
}
void recomp_renderer_set_frame_rate(RecompRenderer *s,uint32_t target_fps) {
    s->check_owner();
    if(s->token || s->stats.graphics_tasks || s->stats.presentations || (target_fps && (target_fps<10 || target_fps>1000)))
        recomp_fail("configure renderer frame rate before submitting tasks",target_fps);
    s->app->userConfig.refreshRate=target_fps?RT64::UserConfiguration::RefreshRate::Manual:RT64::UserConfiguration::RefreshRate::Original;
    if(target_fps)s->app->userConfig.refreshRateTarget=static_cast<int>(target_fps);
    s->app->updateUserConfig(false);
    // Start presenting interpolated frames with the completed display list.
    // Waiting for the guest's buffered VI swap delays the interpolation window.
    s->app->enhancementConfig.presentation.mode=target_fps?RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly:RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering;
    s->app->updateEnhancementConfig();
}
void recomp_renderer_set_graphics_hook(RecompRenderer *s,RecompGraphicsHook hook) {
    s->check_owner();s->graphics_hook=hook;
}
void recomp_renderer_set_microcode_hook(RecompRenderer *s,RecompMicrocodeHook hook) {
    s->check_owner();s->microcode_hook=hook;
}
void recomp_renderer_destroy(RecompRenderer *s) {
    if(!s)return;
    s->check_owner();
    s->app->state->textureManager.captureTexture={};
    s->textureCapture.reset();
    if(s->textureScan)s->textureScan->stop();
    SDL_SetEventFilter(nullptr,nullptr);
    s->wait_texture_reload();
    s->cheat_menu.reset();
#ifdef RECOMP_DEBUG
    s->debug_overlay.reset();
#endif
    if(s->ready) { s->wait_workers();s->app->end(); }
    if(std::getenv("RECOMP_AUDIO_TRACE"))std::fprintf(stderr,
        "Renderer timing: graphics_tasks=%llu graphics_job_ms=%.3f presentations=%llu present_job_ms=%.3f display_frames=%llu\n",
        static_cast<unsigned long long>(s->stats.graphics_tasks),s->stats.submit_ns/1000000.0,
        static_cast<unsigned long long>(s->stats.presentations),s->stats.present_ns/1000000.0,
        static_cast<unsigned long long>(s->display_frames.load()));
#ifdef RECOMP_DEBUG
    std::fprintf(stderr,"Textures: %llu PNGs written, %llu replacements used.\n",
                 static_cast<unsigned long long>(s->textureFiles.pngsWritten),
                 static_cast<unsigned long long>(s->textureFiles.replacedCount()));
#endif
    SDL_Window *window=s->window;
    s->app.reset();RT64::SetRenderHooks(nullptr,nullptr,nullptr);active_renderer=nullptr;
    if(s->frame_trace && std::fclose(s->frame_trace))recomp_fail("cannot close renderer frame trace",0);
    s->input.reset();
    if(window)SDL_DestroyWindow(window);
    SDL_Quit();delete s;
}
bool recomp_renderer_submit(RecompRenderer *s,const RecompTask *task) {
    s->check_owner();
    if(!s->ready || s->token || task->resumed || task->words[0]!=1
       || (task->words[1]&~(RECOMP_TASK_DP_WAIT | RECOMP_TASK_LOADABLE | RECOMP_TASK_USER_FLAGS))
       || !task->words[4] || task->words[5]>0x2000
       || !task->words[6] || !task->words[7] || task->words[7]>0x2000
       || task->words[13]%8 || !task->words[13])return false;
    s->sync_vi();
    s->code=recomp_ram_address(s->rt,task->words[4],task->words[5]?task->words[5]:0x1000);
    s->data=recomp_ram_address(s->rt,task->words[6],task->words[7]);
    s->list=recomp_ram_address(s->rt,task->words[12],task->words[13]);
    s->token=task->token;s->sp_seen=s->dp_seen=false;
    trace_frame(s,"graphics_submit",s->code);
    /* The task hook parks the guest in recomp_on_host. RT64 captures geometry,
     * textures and readbacks before returning; its GPU queues stay asynchronous. */
    process_graphics(s);
    trace_frame(s,"graphics_receipt");
    if(!s->sp_seen || !recomp_task_complete(s->rt,task->token,RECOMP_SP_DONE))
        recomp_fail("graphics task missing SP receipt",task->guest_address);
    if(s->dp_seen) {
        if(!recomp_task_complete(s->rt,task->token,RECOMP_DP_DONE))
            recomp_fail("invalid graphics task DP receipt",task->guest_address);
    } else {
        if(!recomp_task_finish_without_dp(s->rt,task->token))
            recomp_fail("invalid graphics task split-list receipt",task->guest_address);
        s->stats.graphics_tasks_without_dp++;
    }
    s->token=0;s->stats.graphics_tasks++;
    return true;
}
bool recomp_renderer_present(RecompRenderer *s) {
    s->check_owner();
    s->sync_vi();SDL_PumpEvents();
    trace_frame(s,"host_present",s->vi.origin);
#ifdef RECOMP_DEBUG
    s->debug_overlay->update(s->rt,{s->debug_geometry,s->stats.graphics_tasks,s->stats.submit_ns});
    s->app->appWindow->sdlCheckFilterInstallation();
#endif
    process_present(s);
    return true;
}
bool recomp_renderer_input(RecompRenderer *s,RecompPad *pad) {
    s->check_owner();SDL_Event event;
    if(s->textureScan && s->textureScan->takeReload()) {
        s->wait_texture_reload();s->textureFiles.refresh(*s->app->textureCache);
        std::fprintf(stderr,"Texture scan: RT64 replacements refreshed.\n");
    }
    while(SDL_PollEvent(&event)) {
        if(event.type==SDL_QUIT)return false;
        s->cheat_menu->event(event);
        if(s->cheat_menu->open()) {
            const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
            // RT64 draws its inspector after the external render hook. Close
            // it so it cannot cover the menu or retain keyboard/mouse capture.
            if(s->app->presentQueue->inspector)
                s->app->processDeveloperShortcut(RT64::Application::DeveloperShortcut::Inspector);
        }
        if(s->cheat_menu->captures_input())continue;
        if(event.type==SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode==SDL_SCANCODE_F5) {
            s->wait_texture_reload();s->textureFiles.refreshAndToggle(*s->app->textureCache);
        }
#ifdef RECOMP_DEBUG
        if(event.type==SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode==SDL_SCANCODE_F6) {
            s->wait_workers();
            if(s->app->device->getCapabilities().wireframe) {
                auto &renderer=*s->app->workloadQueue->framebufferRenderer;
                bool enabled=!renderer.wireframe.load();
                if(enabled)s->app->rasterShaderCache->shaderUber->createWireframePipelines();
                renderer.wireframe=enabled;
                std::fprintf(stderr,"Render mode: %s\n",enabled?"wireframe":"textured");
            } else {
                std::fprintf(stderr,"Wireframe rendering is unavailable on this graphics device.\n");
            }
        }
#endif
    }
    s->cheat_menu->update();
    *pad={};pad->connected=true;
    if(s->cheat_menu->captures_input())return true;
    if(!(SDL_GetWindowFlags(s->window)&SDL_WINDOW_INPUT_FOCUS))return true;
#ifdef RECOMP_DEBUG
    {
        const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
        // SDL's keyboard state includes keys consumed by the ImGui event filter.
        if(s->app->presentQueue->inspector && ImGui::GetIO().WantCaptureKeyboard)return true;
    }
#endif
    const Uint8 *keys=SDL_GetKeyboardState(nullptr);
    if(keys[SDL_SCANCODE_ESCAPE])return false;
    const struct { SDL_Scancode key;uint16_t button; } bindings[]={
        {SDL_SCANCODE_A,0x8000},{SDL_SCANCODE_X,0x4000},{SDL_SCANCODE_Z,0x2000},{SDL_SCANCODE_S,0x1000},
        {SDL_SCANCODE_T,0x0800},{SDL_SCANCODE_G,0x0400},{SDL_SCANCODE_F,0x0200},{SDL_SCANCODE_H,0x0100},
        {SDL_SCANCODE_C,0x0020},{SDL_SCANCODE_V,0x0010},{SDL_SCANCODE_I,0x0008},{SDL_SCANCODE_K,0x0004},
        {SDL_SCANCODE_J,0x0002},{SDL_SCANCODE_L,0x0001}};
    for(const auto &binding:bindings)if(keys[binding.key])pad->buttons|=binding.button;
    pad->stick_x=(keys[SDL_SCANCODE_RIGHT]-keys[SDL_SCANCODE_LEFT])*80;
    pad->stick_y=(keys[SDL_SCANCODE_UP]-keys[SDL_SCANCODE_DOWN])*80;
    if(pad->stick_x && pad->stick_y) { pad->stick_x=pad->stick_x*7/10;pad->stick_y=pad->stick_y*7/10; }
    recomp_input_poll(s->input.get(),pad);
    return true;
}
#ifdef RECOMP_DEBUG
bool recomp_renderer_capture(RecompRenderer *s,const char *path) {
    s->check_owner();s->wait_workers();
    SDL_SysWMinfo info{};SDL_VERSION(&info.version);
    if(!SDL_GetWindowWMInfo(s->app->appWindow->sdlWindow,&info) || info.subsystem!=SDL_SYSWM_X11)return false;
    XWindowAttributes attrs{};Display *display=info.info.x11.display;Window window=info.info.x11.window;
    if(!XGetWindowAttributes(display,window,&attrs))return false;
    XImage *shot=XGetImage(display,window,0,0,attrs.width,attrs.height,AllPlanes,ZPixmap);if(!shot)return false;
    FILE *file=std::fopen(path,"wb");if(!file) { XDestroyImage(shot);return false; }
    std::fprintf(file,"P6\n%d %d\n255\n",attrs.width,attrs.height);
    auto component=[](unsigned long pixel,unsigned long mask) {
        unsigned shift=0;while(mask && !(mask&1)) { mask>>=1;shift++; }
        return mask ? static_cast<int>(((pixel>>shift)&mask)*255/mask) : 0;
    };
    for(int y=0;y<attrs.height;y++)for(int x=0;x<attrs.width;x++) {
        unsigned long pixel=XGetPixel(shot,x,y);
        std::fputc(component(pixel,shot->red_mask),file);std::fputc(component(pixel,shot->green_mask),file);
        std::fputc(component(pixel,shot->blue_mask),file);
    }
    bool okay=!std::ferror(file);if(std::fclose(file))okay=false;XDestroyImage(shot);return okay;
}
#endif
RecompRendererStats recomp_renderer_stats(RecompRenderer *s) {
    s->check_owner();s->stats.textures_dumped=s->textureFiles.pngsWritten;
    s->stats.textures_replaced=s->textureFiles.replacedCount();
    s->stats.replacement_files=s->textureFiles.replacementFiles;
    uint64_t first=s->first_display_ns.load(),last=s->last_display_ns.load();
    s->stats.display_frames=s->display_frames.load();
    s->stats.display_elapsed_ns=last>=first?last-first:0;
    return s->stats;
}

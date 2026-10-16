#include "renderer.h"
#include "hle/rt64_application.h"
#include "hle/rt64_workload_queue.h"
#include "hle/rt64_present_queue.h"
#include "rhi/rt64_render_hooks.h"
#include "imgui.h"
#include "textures.h"
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
#include <vector>
#include <unistd.h>

struct RecompRenderer {
    Runtime *rt;
    RecompWorker *worker=nullptr;
    std::vector<uint8_t> before;
    RecompTask pending{};
    bool busy=false, presenting=false, registered=false;
    uint32_t code=0,data=0,list=0;
    std::thread::id processing;
    RecompGraphicsHook graphics_hook=nullptr;
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<RT64::Application> app;
    std::vector<uint8_t> ram=std::vector<uint8_t>(8*1024*1024);
    std::array<uint8_t,64> header{};
    std::array<uint8_t,4096> dmem{},imem{};
    uint32_t mi=0, dp_start=0,dp_end=0,dp_current=0,dp_status=0,dp_clock=0,dp_busy=0,dp_pipe=0,dp_tmem=0;
    RecompViRegisters vi{};
    RecompRendererStats stats{};
    std::atomic<uint64_t> display_frames{0}, first_display_ns{0}, last_display_ns{0};
    RecompTextureFiles textureFiles;
    std::unique_ptr<RecompTextures::Scanner> textureScan;
    std::unique_ptr<RecompTextures::CaptureWriter> textureCapture;
    uint64_t token=0;
    bool sp_seen=false,dp_seen=false;
    bool ready=false;
    SDL_Window *window=nullptr;
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
static void display_draw(plume::RenderCommandList *,plume::RenderFramebuffer *) {
    RecompRenderer *s=active_renderer;
    uint64_t now=monotonic_ns();
    if(s->display_frames.load()==0)s->first_display_ns=now;
    s->last_display_ns=now;s->display_frames++;
}
static void interrupts() {
    RecompRenderer *s=active_renderer;
    if(!s)recomp_fail("RT64 interrupt without renderer",0);
    if(s->processing!=std::this_thread::get_id())recomp_fail("RT64 interrupt outside processing thread",0);
    uint32_t bits=s->mi&0x21u;s->mi&=~bits;
    if(!bits)return;
    if(!s->token)recomp_fail("RT64 interrupt without task token",0);
    if(bits&0x20) {
        if(s->dp_seen)
            recomp_fail("invalid RT64 DP receipt",0);
        s->dp_seen=true;s->stats.dp_callbacks++;
    }
    if(bits&1) {
        if(s->sp_seen)
            recomp_fail("invalid RT64 SP receipt",0);
        s->sp_seen=true;s->stats.sp_callbacks++;
    }
}

static void service_renderer(Runtime *rt,void *opaque,bool wait) {
    RecompRenderer *s=static_cast<RecompRenderer *>(opaque);
    s->check_owner();
    if(!s->busy || (!wait && !recomp_worker_ready(s->worker)))return;
    s->busy=false;
    recomp_worker_finish(s->worker);
    if(s->presenting) { s->presenting=false;return; }
    recomp_worker_merge(rt,s->before.data(),s->ram.data());
    if(!s->sp_seen || !recomp_task_complete(rt,s->pending.token,RECOMP_SP_DONE))
        recomp_fail("graphics worker missing SP receipt",s->pending.guest_address);
    if(s->dp_seen) {
        if(!recomp_task_complete(rt,s->pending.token,RECOMP_DP_DONE))
            recomp_fail("invalid graphics worker DP receipt",s->pending.guest_address);
    } else {
        if(!recomp_task_finish_without_dp(rt,s->pending.token))
            recomp_fail("invalid graphics worker split-list receipt",s->pending.guest_address);
        s->stats.graphics_tasks_without_dp++;
    }
    s->token=0;s->stats.graphics_tasks++;
}
static void process_graphics(void *opaque) {
    RecompRenderer *s=static_cast<RecompRenderer *>(opaque);
    s->processing=std::this_thread::get_id();
    uint64_t start=monotonic_ns();
    if(s->app->interpreter->UCode.textAddress!=s->code || s->app->interpreter->UCode.dataAddress!=s->data)
        s->app->interpreter->gbiManager.deduceGBIInformation(s->ram.data(),s->code,s->data);
    s->app->interpreter->loadUCodeGBI(s->code,s->data,true);
    if(!s->app->interpreter->hleGBI)recomp_fail("RT64 did not recognize the graphics microcode",s->code);
    if(s->graphics_hook)s->graphics_hook(s->app->interpreter.get(),s->before.size(),&s->stats.rdp_triangles);
    if(s->textureCapture)s->textureCapture->beginTask();
    s->app->processDisplayLists(s->ram.data(),s->list,0,true);
    s->wait_workers();
    s->textureFiles.collect(*s->app->textureCache);
    s->stats.submit_ns+=monotonic_ns()-start;
}
static void process_present(void *opaque) {
    RecompRenderer *s=static_cast<RecompRenderer *>(opaque);
    s->processing=std::this_thread::get_id();
    uint64_t start=monotonic_ns();
    // SDL filter/window access stays on the host. The worker owns RT64 state.
    s->app->screenApiProfiler.logAndRestart();
    s->app->state->updateScreen(s->app->core.decodeVI(),false);
    s->wait_workers();
    s->stats.presentations++;s->stats.present_ns+=monotonic_ns()-start;
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
    core.HEADER=s->header.data();core.RDRAM=s->ram.data();core.DMEM=s->dmem.data();core.IMEM=s->imem.data();
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
        s->app->userConfig.refreshRate=RT64::UserConfiguration::RefreshRate::Original;
        s->app->enhancementConfig.textureLOD.scale=true;
        if(s->app->setup(0)!=RT64::Application::SetupResult::Success) {
            std::fprintf(stderr,"RT64 setup failed: %s\n",RT64::GlobalLastError.c_str());
            s->app.reset();RT64::SetRenderHooks(nullptr,nullptr,nullptr);
            active_renderer=nullptr;SDL_DestroyWindow(s->window);SDL_Quit();return nullptr;
        }
        s->ready=true;
        // The app clock paces this serialized adapter. Waiting again on the
        // compositor can block guest VI/audio work while acquiring an image.
        s->app->swapChain->setVsyncEnabled(false);
        s->app->presentQueue->presentWaitEnabled=false;
        s->app->appWindow->sdlWindow=s->window;
#ifdef RECOMP_DEBUG
        s->app->appWindow->sdlCheckFilterInstallation();
#endif
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
                                        s->ram.size(),tile,load,palette,width,height,tlut);
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
        s->before.resize(rt->rdram_size);
        s->worker=recomp_worker_create("n64-video");
        if(s->worker && rt->tasks)s->registered=recomp_task_service_add(rt,service_renderer,s.get());
        if(!s->worker || (rt->tasks && !s->registered)) {
            recomp_worker_destroy(s->worker);s->worker=nullptr;
            throw std::runtime_error("graphics worker initialization failed");
        }
        return s.release();
    } catch(const std::exception &e) {
        std::fprintf(stderr,"RT64 setup: %s\n",e.what());
        SDL_SetEventFilter(nullptr,nullptr);
        s->app.reset();RT64::SetRenderHooks(nullptr,nullptr,nullptr);active_renderer=nullptr;
        SDL_DestroyWindow(s->window);SDL_Quit();return nullptr;
    }
}
void recomp_renderer_set_frame_rate(RecompRenderer *s,uint32_t target_fps) {
    s->check_owner();
    if(s->busy || s->stats.graphics_tasks || s->stats.presentations || (target_fps && (target_fps<10 || target_fps>1000)))
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
    s->check_owner();service_renderer(s->rt,s,true);s->graphics_hook=hook;
}
void recomp_renderer_destroy(RecompRenderer *s) {
    if(!s)return;
    s->check_owner();service_renderer(s->rt,s,true);
    recomp_task_service_remove(s->rt,s);
    recomp_worker_destroy(s->worker);
    s->app->state->textureManager.captureTexture={};
    s->textureCapture.reset();
    if(s->textureScan)s->textureScan->stop();
    SDL_SetEventFilter(nullptr,nullptr);
    if(s->ready) { s->wait_workers();s->app->end(); }
#ifdef RECOMP_DEBUG
    std::fprintf(stderr,"Textures: %llu PNGs written, %llu replacements used.\n",
                 static_cast<unsigned long long>(s->textureFiles.pngsWritten),
                 static_cast<unsigned long long>(s->textureFiles.replacedCount()));
#endif
    SDL_Window *window=s->window;
    s->app.reset();RT64::SetRenderHooks(nullptr,nullptr,nullptr);active_renderer=nullptr;
    if(window)SDL_DestroyWindow(window);
    SDL_Quit();delete s;
}
bool recomp_renderer_submit(RecompRenderer *s,const RecompTask *task) {
    s->check_owner();service_renderer(s->rt,s,true);
    if(!s->registered) {
        s->registered=recomp_task_service_add(s->rt,service_renderer,s);
        if(!s->registered)return false;
    }
    if(!s->ready || s->token || task->resumed || task->words[0]!=1
       || (task->words[1]&~(RECOMP_TASK_DP_WAIT | RECOMP_TASK_LOADABLE | RECOMP_TASK_USER_FLAGS))
       || !task->words[4] || task->words[5]>0x2000
       || !task->words[6] || !task->words[7] || task->words[7]>0x2000
       || task->words[13]%8 || !task->words[13])return false;
    s->sync_vi();
    std::memcpy(s->before.data(),s->rt->rdram,s->rt->rdram_size);
    std::memcpy(s->ram.data(),s->before.data(),s->rt->rdram_size);
    s->code=recomp_ram_address(s->rt,task->words[4],task->words[5]?task->words[5]:0x1000);
    s->data=recomp_ram_address(s->rt,task->words[6],task->words[7]);
    s->list=recomp_ram_address(s->rt,task->words[12],task->words[13]);
    s->pending=*task;s->token=task->token;s->sp_seen=s->dp_seen=false;
    s->busy=recomp_worker_submit(s->worker,process_graphics,s);
    return s->busy;
}
bool recomp_renderer_present(RecompRenderer *s) {
    s->check_owner();service_renderer(s->rt,s,true);
    s->sync_vi();SDL_PumpEvents();
#ifdef RECOMP_DEBUG
    s->app->appWindow->sdlCheckFilterInstallation();
#endif
    s->presenting=true;s->busy=recomp_worker_submit(s->worker,process_present,s);
    return s->busy;
}
bool recomp_renderer_input(RecompRenderer *s,RecompPad *pad) {
    s->check_owner();service_renderer(s->rt,s,true);SDL_Event event;
    if(s->textureScan && s->textureScan->takeReload()) {
        s->wait_texture_reload();s->textureFiles.refresh(*s->app->textureCache);
        std::fprintf(stderr,"Texture scan: RT64 replacements refreshed.\n");
    }
    while(SDL_PollEvent(&event)) {
        if(event.type==SDL_QUIT)return false;
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
    *pad={};pad->connected=true;
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
    return true;
}
#ifdef RECOMP_DEBUG
bool recomp_renderer_capture(RecompRenderer *s,const char *path) {
    s->check_owner();service_renderer(s->rt,s,true);s->wait_workers();
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
    s->check_owner();service_renderer(s->rt,s,true);s->stats.textures_dumped=s->textureFiles.pngsWritten;
    s->stats.textures_replaced=s->textureFiles.replacedCount();
    s->stats.replacement_files=s->textureFiles.replacementFiles;
    uint64_t first=s->first_display_ns.load(),last=s->last_display_ns.load();
    s->stats.display_frames=s->display_frames.load();
    s->stats.display_elapsed_ns=last>=first?last-first:0;
    return s->stats;
}

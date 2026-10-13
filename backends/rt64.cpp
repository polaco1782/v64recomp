#include "renderer.h"
#include "hle/rt64_application.h"
#include "hle/rt64_workload_queue.h"
#include "hle/rt64_present_queue.h"
#include "textures.h"
#define Status int
#include <X11/Xutil.h>
#undef Status
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

struct RecompRenderer {
    Runtime *rt;
    RecompGraphicsHook graphics_hook=nullptr;
    std::thread::id owner=std::this_thread::get_id();
    std::unique_ptr<RT64::Application> app;
    std::vector<uint8_t> ram=std::vector<uint8_t>(8*1024*1024);
    std::array<uint8_t,64> header{};
    std::array<uint8_t,4096> dmem{},imem{};
    uint32_t mi=0, dp_start=0,dp_end=0,dp_current=0,dp_status=0,dp_clock=0,dp_busy=0,dp_pipe=0,dp_tmem=0;
    RecompViRegisters vi{};
    RecompRendererStats stats{};
    RecompTextureFiles textureFiles;
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
    void sync_vi() { (void)recomp_vi_registers(rt,&vi); }
};
static RecompRenderer *active_renderer;
static uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void interrupts() {
    RecompRenderer *s=active_renderer;
    if(!s)recomp_fail("RT64 interrupt without renderer",0);
    s->check_owner();uint32_t bits=s->mi&0x21u;s->mi&=~bits;
    if(!bits)return;
    if(!s->token)recomp_fail("RT64 interrupt without task token",0);
    if(bits&0x20) {
        if(s->dp_seen || !recomp_task_complete(s->rt,s->token,RECOMP_DP_DONE))
            recomp_fail("invalid RT64 DP receipt",0);
        s->dp_seen=true;s->stats.dp_callbacks++;
    }
    if(bits&1) {
        if(s->sp_seen || !recomp_task_complete(s->rt,s->token,RECOMP_SP_DONE))
            recomp_fail("invalid RT64 SP receipt",0);
        s->sp_seen=true;s->stats.sp_callbacks++;
    }
}

RecompRenderer *recomp_renderer_create_named(Runtime *rt,const char *data_path,const char *window_title) {
    if(active_renderer || !rt->rom || rt->rdram_size>8*1024*1024 || !data_path)return nullptr;
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
    try {
        s->app=std::make_unique<RT64::Application>(core,config);
        s->app->userConfig.graphicsAPI=RT64::UserConfiguration::GraphicsAPI::Vulkan;
        s->app->userConfig.resolution=RT64::UserConfiguration::Resolution::Manual;
        s->app->userConfig.resolutionMultiplier=4.0;
        s->app->userConfig.antialiasing=RT64::UserConfiguration::Antialiasing::MSAA4X;
        s->app->userConfig.threePointFiltering=false;
        s->app->userConfig.refreshRate=RT64::UserConfiguration::RefreshRate::Original;
        if(s->app->setup(0)!=RT64::Application::SetupResult::Success) {
            std::fprintf(stderr,"RT64 setup failed: %s\n",RT64::GlobalLastError.c_str());
            active_renderer=nullptr;s->app.reset();SDL_DestroyWindow(s->window);SDL_Quit();return nullptr;
        }
        s->ready=true;
        // The app clock paces this serialized adapter. Waiting again on the
        // compositor can block guest VI/audio work while acquiring an image.
        s->app->swapChain->setVsyncEnabled(false);
        s->app->presentQueue->presentWaitEnabled=false;
        s->app->appWindow->sdlWindow=s->window;
        s->textureFiles.configure(*s->app->textureCache);
        return s.release();
    } catch(const std::exception &e) {
        std::fprintf(stderr,"RT64 setup: %s\n",e.what());active_renderer=nullptr;
        s->app.reset();SDL_DestroyWindow(s->window);SDL_Quit();return nullptr;
    }
}
void recomp_renderer_set_graphics_hook(RecompRenderer *s,RecompGraphicsHook hook) {
    s->check_owner();s->graphics_hook=hook;
}
void recomp_renderer_destroy(RecompRenderer *s) {
    if(!s)return;
    s->check_owner();
    SDL_SetEventFilter(nullptr,nullptr);
    if(s->ready) { s->wait_workers();s->app->end(); }
    std::fprintf(stderr,"Textures: %llu PNGs written, %llu replacements used.\n",
                 static_cast<unsigned long long>(s->textureFiles.pngsWritten),
                 static_cast<unsigned long long>(s->textureFiles.replacedCount()));
    active_renderer=nullptr;
    SDL_Window *window=s->window;
    s->app.reset();if(window)SDL_DestroyWindow(window);SDL_Quit();delete s;
}
bool recomp_renderer_submit(RecompRenderer *s,const RecompTask *task) {
    uint64_t start=monotonic_ns();
    s->check_owner();
    if(!s->ready || s->token || task->resumed || task->words[0]!=1 || (task->words[1]&~2u)
       || !task->words[4] || task->words[5]>0x2000
       || !task->words[6] || !task->words[7] || task->words[7]>0x2000
       || task->words[13]%8 || !task->words[13])return false;
    s->wait_workers();s->sync_vi();
    std::memcpy(s->ram.data(),s->rt->rdram,s->rt->rdram_size);
    uint32_t code=recomp_ram_address(s->rt,task->words[4],task->words[5]?task->words[5]:0x1000);
    uint32_t data=recomp_ram_address(s->rt,task->words[6],task->words[7]);
    uint32_t list=recomp_ram_address(s->rt,task->words[12],task->words[13]);
    s->token=task->token;s->sp_seen=s->dp_seen=false;
    if(s->app->interpreter->UCode.textAddress!=code || s->app->interpreter->UCode.dataAddress!=data)
        s->app->interpreter->gbiManager.deduceGBIInformation(s->ram.data(),code,data);
    s->app->interpreter->loadUCodeGBI(code,data,true);
    if(!s->app->interpreter->hleGBI)recomp_fail("RT64 did not recognize the graphics microcode",code);
    if(s->graphics_hook)s->graphics_hook(s->app->interpreter.get(),s->rt->rdram_size,&s->stats.rdp_triangles);
    s->app->processDisplayLists(s->ram.data(),list,0,true);
    s->wait_workers();
    s->textureFiles.collect(*s->app->textureCache);
    // fullSync renders/copies native framebuffers before its DP callback. The
    // guest coroutine is still inside submit, so copying back cannot overwrite guest work.
    std::memcpy(s->rt->rdram,s->ram.data(),s->rt->rdram_size);
    bool complete=s->sp_seen && s->dp_seen;s->token=0;
    if(complete)s->stats.graphics_tasks++;
    s->stats.submit_ns+=monotonic_ns()-start;
    return complete;
}
bool recomp_renderer_present(RecompRenderer *s) {
    uint64_t start=monotonic_ns();
    s->check_owner();s->sync_vi();SDL_PumpEvents();s->app->updateScreen();s->wait_workers();
    s->stats.presentations++;s->stats.present_ns+=monotonic_ns()-start;return true;
}
bool recomp_renderer_input(RecompRenderer *s,RecompPad *pad) {
    s->check_owner();SDL_Event event;
    while(SDL_PollEvent(&event)) {
        if(event.type==SDL_QUIT)return false;
        if(event.type==SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode==SDL_SCANCODE_F5) {
            s->wait_workers();s->textureFiles.refreshAndToggle(*s->app->textureCache);
        }
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
    }
    const Uint8 *keys=SDL_GetKeyboardState(nullptr);
    if(keys[SDL_SCANCODE_ESCAPE])return false;
    *pad={};pad->connected=true;
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
RecompRendererStats recomp_renderer_stats(RecompRenderer *s) {
    s->check_owner();s->stats.textures_dumped=s->textureFiles.pngsWritten;
    s->stats.textures_replaced=s->textureFiles.replacedCount();
    s->stats.replacement_files=s->textureFiles.replacementFiles;return s->stats;
}

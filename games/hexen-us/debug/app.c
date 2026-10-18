#define _POSIX_C_SOURCE 200809L
#define main recomp_debug_main
#include "hexen.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define require recomp_game_require
int main(int argc, char **argv) {
    if(argc!=5){fprintf(stderr,"usage: %s ROM REPORT FRAMES RENDERER_DATA\n",argv[0]);return 2;}
    char *end;unsigned long frames=strtoul(argv[3],&end,10);
    require(*argv[3] && !*end && frames<=UINT32_MAX,"Hexen invalid frame count",0);
    const char *capture=getenv("RECOMP_HEXEN_CAPTURE_EVERY");
    unsigned long interval=capture?strtoul(capture,&end,10):0;
    require(!capture || (*capture && !*end && interval<=UINT32_MAX),"Hexen invalid capture interval",0);
    RecompGameOptions options={.rom=argv[1],.data=argv[4],.save=getenv("RECOMP_HEXEN_SAVE"),
        .mute=getenv("RECOMP_AUDIO_MUTE")!=NULL};
    Hexen game;hexen_init(&game,&options,getenv("RECOMP_HEXEN_WAV"));
    const char *input=getenv("RECOMP_HEXEN_INPUT");FILE *replay=input?fopen(input,"r"):NULL;
    require(!input || replay!=NULL,"Hexen replay open failed",0);
    unsigned next_frame=0,buttons=0;int x=0,y=0;
    bool pending=replay && fscanf(replay,"%u %u %d %d",&next_frame,&buttons,&x,&y)==4;
    RecompPad replay_pad={.connected=true};
    uint64_t deadline=recomp_game_monotonic();uint32_t frame=0;
    for(;!frames || frame<frames;frame++) {
        RecompPad pad={.connected=true};
        if(!recomp_renderer_input(game.renderer,&pad))break;
        while(pending && next_frame<=frame) {
            require(buttons<=UINT16_MAX && x>=-128 && x<=127 && y>=-128 && y<=127,"Hexen invalid replay pad",frame);
            replay_pad.buttons=(uint16_t)buttons;replay_pad.stick_x=(int8_t)x;replay_pad.stick_y=(int8_t)y;
            pending=fscanf(replay,"%u %u %d %d",&next_frame,&buttons,&x,&y)==4;
        }
        if(replay)pad=replay_pad;
        hexen_step(&game,&pad);
        require(recomp_renderer_present(game.renderer),"Hexen presentation failed",0);
        if(interval && (frame+1)%interval==0) {
            char path[4096];snprintf(path,sizeof(path),"%s-%04u.ppm",argv[2],frame+1);
            require(recomp_renderer_capture(game.renderer,path),"Hexen capture failed",frame);
        }
        deadline+=16666667;hexen_pace(&game,deadline);
    }
    RecompRendererStats rs=recomp_renderer_stats(game.renderer);
    RecompAudioStats as=recomp_audio_stats(game.audio);
    FILE *report=fopen(argv[2],"w");require(report!=NULL,"Hexen report open failed",0);
    fprintf(report,"{\n  \"frames\": %u,\n  \"bss_verified\": %s,\n  \"program_verified\": %s,\n"
        "  \"graphics_tasks\": %" PRIu64 ",\n  \"audio_tasks\": %" PRIu64 ",\n"
        "  \"audio_nonzero_samples\": %" PRIu64 ",\n  \"audio_peak\": %u,\n"
        "  \"audio_underruns\": %" PRIu64 ",\n  \"audio_resets\": %" PRIu64 "\n}\n",
        frame,game.bss_verified?"true":"false",game.program_verified?"true":"false",
        rs.graphics_tasks,as.tasks,as.nonzero_samples,as.peak,as.queue_underruns,as.queue_resets);
    require(fclose(report)==0,"Hexen report write failed",0);
    if(replay)fclose(replay);
    hexen_destroy(&game);return 0;
}

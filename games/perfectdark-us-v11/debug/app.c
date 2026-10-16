#include "perfectdark.h"
#include <stdio.h>
#include <stdlib.h>
#define require recomp_game_require
static RecompGameOptions options;
static const char *report_path;
static FILE *input_script;
static bool next_input;
static unsigned input_frame, input_buttons;
static int input_x,input_y;
static RecompPad replay_pad={.connected=true};
static void read_input(void) {
    next_input=fscanf(input_script,"%u %u %d %d",&input_frame,&input_buttons,&input_x,&input_y)==4;
}
int recomp_debug_main(int argc,char **argv) {
    if(argc==5 && argv[1][0]!='-') {
        options.rom=argv[1];report_path=argv[2];options.frames=(uint32_t)strtoul(argv[3],NULL,10);
        options.data=argv[4];options.save=getenv("RECOMP_PERFECTDARK_SAVE");
        if(!options.save)options.save="local/saves/perfectdark-us-v11.eeprom";
        options.mute=getenv("RECOMP_AUDIO_MUTE")!=NULL;
    } else
    {int parsed=recomp_game_options(argc,argv,&options);if(parsed)return parsed<0?2:0;}
    const char *replay=getenv("RECOMP_PERFECTDARK_INPUT");
    if(replay) {
        input_script=fopen(replay,"r");require(input_script!=NULL,"Perfect Dark replay cannot open",0);
        read_input();
    }
    const char *capture=getenv("RECOMP_PERFECTDARK_CAPTURE_EVERY");
    unsigned capture_every=capture?(unsigned)strtoul(capture,NULL,10):120;
    PerfectDark game;
    perfectdark_start(&game,&options);
    uint32_t frame=0;
    for(;!options.frames || frame<options.frames;frame++) {
        RecompPad pad={.connected=true};if(!recomp_renderer_input(game.renderer,&pad))break;
        if(input_script) {
            while(next_input && input_frame<=frame) {
                replay_pad.buttons=(uint16_t)input_buttons;
                replay_pad.stick_x=(int8_t)input_x;replay_pad.stick_y=(int8_t)input_y;
                read_input();
            }
            pad=replay_pad;
        }
        perfectdark_step(&game,&pad);
        if(report_path && capture_every && game.graphics_tasks && (frame+1)%capture_every==0) {
            char path[4096];snprintf(path,sizeof(path),"%s.frame-%06u.ppm",report_path,frame+1);
            require(recomp_renderer_capture(game.renderer,path),"Perfect Dark capture failed",0);
        }
    }
    RecompAudioStats as=recomp_audio_stats(game.audio);
    RecompRendererStats rs=recomp_renderer_stats(game.renderer);
    if(report_path) {
        FILE *f=fopen(report_path,"w");require(f!=NULL,"Perfect Dark report cannot open",0);
        fprintf(f,"{\"presentations\":%u,\"host_seconds\":%.3f,\"graphics_tasks\":%u,\"audio_tasks\":%u,"
            "\"rdp_triangles\":%llu,\"audio_frames\":%llu,\"nonzero_samples\":%llu,\"stage\":%u,"
            "\"queue_underruns\":%llu,\"queue_resets\":%llu,\"game_image_verified\":%s}\n",
            frame,(recomp_game_monotonic()-game.start_ns)/1e9,game.graphics_tasks,game.audio_tasks,
            (unsigned long long)rs.rdp_triangles,(unsigned long long)as.frames,(unsigned long long)as.nonzero_samples,
            (unsigned)recomp_load(&game.runtime,0x8005d9b4,4),(unsigned long long)as.queue_underruns,
            (unsigned long long)as.queue_resets,game.runtime.section_loaded[4]?"true":"false");
        require(fclose(f)==0,"Perfect Dark report write failed",0);
    }
    if(input_script)fclose(input_script);
    perfectdark_stop(&game);
    return 0;
}

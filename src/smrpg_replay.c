/* Adapted from SMW's recorder; the unchanged shared bounded writer owns I/O.
 * Checkpoints use the full execution snapshot, including SA-1 state. Frame
 * comparisons include WRAM, CPU/SA-1 clock, SPC/BWRAM/IRAM and VRAM.
 */
#include "smrpg_replay.h"
#include "smrpg_runtime.h"
#include "smrpg_renderer.h"
#include "diagnostics/replay_writer.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "host_paths.h"
#include "mod_runtime.h"
#include "snes/snes.h"
#include "snes/cart.h"
#include "snes/apu.h"
#include "snes/ppu.h"
#include "snes/sa1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define replay_mkdir(p) _mkdir(p)
#define replay_pid() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define replay_mkdir(p) mkdir(p,0755)
#define replay_pid() getpid()
#endif
#ifndef SNESRECOMP_BUILD_VERSION
#define SNESRECOMP_BUILD_VERSION "dev"
#endif
#ifndef SMRPG_REPLAY_IDENTITY
#define SMRPG_REPLAY_IDENTITY "unknown"
#endif
enum { SEGMENT_FRAMES=600, SEGMENTS=60, STATE_LIMIT=16*1024*1024,
       SNAPSHOT_LIMIT=2*1024*1024, MEMORY_LIMIT=0x10000+0x800+0x20000 };
static bool enabled,playing,failed,loaded,pending,recording;
static SnesReplayWriter *writer;
static FILE *stream,*states;
static char stem[1200],path[1280];
static unsigned segment,oldest=1,rows,next_frame,playback_rows,rom_identity;
static uint64_t generation;
static void *snapshot;
static size_t snapshot_capacity,segment_bytes;
static unsigned char memory[MEMORY_LIMIT];
extern Snes *g_snes;
extern Ppu *g_ppu;
extern CpuState g_cpu;
extern uint8_t g_ram[0x20000];
static uint32_t hash(const void *data,size_t size) {
  uint32_t h=2166136261u;const unsigned char *p=data;
  while(size--)h=(h^*p++)*16777619u;return h;
}
static void cpu_words(uint64_t words[18]) {
  words[0]=g_cpu.A;words[1]=g_cpu.X;words[2]=g_cpu.Y;words[3]=g_cpu.S;
  words[4]=g_cpu.D;words[5]=g_cpu.DB;words[6]=g_cpu.PB;words[7]=g_cpu.P;
  words[8]=g_cpu.m_flag;words[9]=g_cpu.x_flag;words[10]=g_cpu.emulation;
  words[11]=g_cpu.cycles;words[12]=g_cpu.master_cycles;words[13]=g_cpu.coprocessor_master_cycles;
  words[14]=g_cpu.open_bus;words[15]=g_cpu.host_return_valid;
  words[16]=sa1_master_clock(g_snes->cart->sa1);words[17]=SmrpgResumePc();
}
static size_t copy_memory(void) {
  size_t size=sizeof(g_snes->apu->ram),bwram=g_snes->cart->ramSize;
  if(bwram>0x20000)return 0;
  const void *iram=sa1_cpu_memory_ptr(g_snes->cart->sa1,0,0x3000);
  if(!iram)return 0;
  memcpy(memory,g_snes->apu->ram,size);memcpy(memory+size,iram,0x800);size+=0x800;
  if(bwram)memcpy(memory+size,g_snes->cart->ram,bwram);return size+bwram;
}
static void close_segment(void) {
  if(recording){if(!SnesReplayWriterEnd(writer))failed=true;recording=false;}
  if(stream){if(fclose(stream))failed=true;stream=NULL;}
  if(states){if(fclose(states))failed=true;states=NULL;}
  pending=false;rows=0;
}
static void error(const char *why) {
  fprintf(stderr,"[smrpg-replay] %s: %s\n",path,why?why:"recording stopped");
  failed=true;close_segment();fflush(stderr);
}
void SmrpgReplayShutdown(void) {
  close_segment();const char *why=SnesReplayWriterDestroy(writer);writer=NULL;
  if(why){fprintf(stderr,"[smrpg-replay] %s\n",why);failed=true;}
  free(snapshot);snapshot=NULL;snapshot_capacity=0;
}
void SmrpgReplayEnable(bool value){enabled=value;if(!value&&!playing)close_segment();}
void SmrpgReplayRom(const uint8_t *rom,unsigned size){rom_identity=hash(rom,size);}
bool SmrpgReplayFailed(void){return failed;}
bool SmrpgReplayPlaying(void){return playing;}
static bool open_directory(const char *folder) {
  replay_mkdir(folder);char probe[1200];
  if(snprintf(probe,sizeof(probe),"%s/.smrpg-replay-%ld",folder,(long)replay_pid())>=(int)sizeof(probe))return false;
  FILE *f=fopen(probe,"wb");if(!f)return false;bool ok=fclose(f)==0;remove(probe);return ok;
}
static bool init_stem(void) {
  if(stem[0])return true;char folder[1024],stamp[32],absolute[1024];
  const char *override=getenv("SMRPG_REPLAY_DIR");
  if(override&&*override){
    if(snprintf(folder,sizeof(folder),"%s",override)>=(int)sizeof(folder)||!open_directory(folder))return false;
  }else if(!(snesrecomp_exe_dir_path("logs",folder,sizeof(folder))&&open_directory(folder))){
    const char *tmp=getenv("TEMP");if(!tmp||!*tmp)tmp=getenv("TMPDIR");if(!tmp||!*tmp)tmp="/tmp";
    if(snprintf(folder,sizeof(folder),"%s/SuperMarioRPGSNESRecomp",tmp)>=(int)sizeof(folder))return false;
    replay_mkdir(folder);size_t n=strlen(folder);
    if(snprintf(folder+n,sizeof(folder)-n,"/logs")>=(int)(sizeof(folder)-n)||!open_directory(folder))return false;
  }
  if(!snesrecomp_abspath(folder,absolute,sizeof(absolute)))return false;
  time_t now=time(NULL);struct tm *t=localtime(&now);
  if(!t||!strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",t))strcpy(stamp,"session");
  if(snprintf(stem,sizeof(stem),"%s/smrpg-replay-%s-%ld",absolute,stamp,(long)replay_pid())>=(int)sizeof(stem))return false;
  atexit(SmrpgReplayShutdown);return true;
}
static bool allocate_snapshot(size_t size) {
  if(!size||size>SNAPSHOT_LIMIT)return false;
  if(size>snapshot_capacity){void *p=realloc(snapshot,size);if(!p)return false;snapshot=p;snapshot_capacity=size;}
  return true;
}
static bool start_segment(void) {
  close_segment();if(failed||!init_stem()){error("cannot create replay directory");return false;}
  if(!writer)writer=SnesReplayWriterCreate();if(!writer){error("cannot start bounded writer");return false;}
  ++segment;
  if(segment-oldest>=SEGMENTS){
    static const char *suffix[]={"",".sav",".config.ini",".mods.toml",".mods.txt"};char old[1400];
    for(unsigned i=0;i<5;++i){snprintf(old,sizeof(old),"%s-%06u.replay%s",stem,oldest,suffix[i]);if(!SnesReplayWriterRemove(writer,old)){error(SnesReplayWriterError(writer));return false;}}
    ++oldest;
  }
  segment_bytes=0;snprintf(path,sizeof(path),"%s-%06u.replay",stem,segment);
  char header[512],config[1200],mods[1200],active[16384];
  snprintf(header,sizeof(header),"SMRPGREPLAY 1\nBUILD %s %s\nROM %08x\n",SNESRECOMP_BUILD_VERSION,SMRPG_REPLAY_IDENTITY,rom_identity);
  int length=snes_mod_runtime_effective_set_c(active,sizeof(active));
  if(length<0||length>=(int)sizeof(active)||!snesrecomp_abspath("config.ini",config,sizeof(config))||
      !snesrecomp_abspath("mods/preloaded/state.toml",mods,sizeof(mods))){error("cannot capture active settings");return false;}
  active[length]=0;
  if(!SnesReplayWriterBegin(writer,path,header,config,mods,active)){error(SnesReplayWriterError(writer));return false;}
  recording=true;if(segment==1)fprintf(stderr,"[smrpg-replay] recording %s (bounded background writer; latest ten minutes / 120 MiB checkpoints)\n",path);
  return true;
}
static bool write_snapshot(void) {
  size_t capacity=RtlRollbackSnapshotBound();if(capacity>SNAPSHOT_LIMIT)capacity=SNAPSHOT_LIMIT;
  if(!allocate_snapshot(capacity)){error("cannot allocate checkpoint");return false;}
  size_t size=RtlRollbackSaveToMemory(snapshot,snapshot_capacity);
  if(!size||size>STATE_LIMIT-segment_bytes){error("cannot serialize checkpoint");return false;}
  if(!SnesReplayWriterSnapshot(writer,snapshot,size)){error(SnesReplayWriterError(writer));return false;}
  segment_bytes+=size;return true;
}
bool SmrpgReplayPlayback(const char *name) {
  SmrpgReplayShutdown();playing=true;failed=loaded=false;playback_rows=0;
  if(!name||!snesrecomp_abspath(name,path,sizeof(path))){error("invalid replay path");return false;}
  stream=fopen(path,"r");if(!stream){error("cannot open replay");return false;}return true;
}
static bool load_playback(void) {
  char line[512],version[80],identity[80],tail;unsigned rom;
  if(!fgets(line,sizeof(line),stream)||strcmp(line,"SMRPGREPLAY 1\n")||!fgets(line,sizeof(line),stream)||
      sscanf(line,"BUILD %79s %79s %c",version,identity,&tail)!=2||!fgets(line,sizeof(line),stream)||sscanf(line,"ROM %x %c",&rom,&tail)!=1){error("invalid replay header");return false;}
  if(strcmp(version,SNESRECOMP_BUILD_VERSION)||strcmp(identity,SMRPG_REPLAY_IDENTITY)||rom!=rom_identity){error("use this recording's exact build, platform and ROM");return false;}
  char active[16384],recorded[16384],selection[1400];
  int length=snes_mod_runtime_effective_set_c(active,sizeof(active));
  snprintf(selection,sizeof(selection),"%s.mods.txt",path);FILE *mods=fopen(selection,"rb");
  size_t count=mods?fread(recorded,1,sizeof(recorded),mods):0;
  bool same=mods&&length>=0&&length<(int)sizeof(active)&&count==(size_t)length&&!memcmp(active,recorded,count)&&fgetc(mods)==EOF;
  if(mods)fclose(mods);if(!same){error("enable the recorded mod selection before replay");return false;}
  char sidecar[1400];snprintf(sidecar,sizeof(sidecar),"%s.sav",path);states=fopen(sidecar,"rb");
  if(!states){error("missing checkpoint sidecar");return false;}loaded=true;return true;
}
static bool playback_begin(uint32_t *input,int *width,bool custom) {
  if(!loaded&&!load_playback())return false;char line[256],tail;
  if(!fgets(line,sizeof(line),stream)){
    if(ferror(stream)||!playback_rows)error("input read failed or no completed frames");
    else{fprintf(stderr,"[smrpg-replay] all recorded checkpoints matched (%u frames)\n",playback_rows);close_segment();}
    return false;
  }
  if(line[0]=='S'){
    unsigned offset,size,digest;
    if(sscanf(line,"S %u %u %x %c",&offset,&size,&digest,&tail)!=3||offset>STATE_LIMIT||size>STATE_LIMIT-offset||
        !allocate_snapshot(size)||fseek(states,(long)offset,SEEK_SET)||fread(snapshot,1,size,states)!=size||
        hash(snapshot,size)!=digest||!RtlRollbackLoadFromMemory(snapshot,size)||!fgets(line,sizeof(line),stream)){error("damaged or incompatible checkpoint");return false;}
    next_frame=(unsigned)snes_frame_counter;
  }else if(!playback_rows){error("first input has no checkpoint");return false;}
  unsigned frame,word,flags,recorded_width;
  if(sscanf(line,"I %u %x %x %u %c",&frame,&word,&flags,&recorded_width,&tail)!=4||
      frame!=next_frame||recorded_width<256||recorded_width>kSmrpgRenderWidth||(recorded_width&1)||flags!=(unsigned)custom){error("invalid input or different renderer selection");return false;}
  *input=word;*width=(int)recorded_width;pending=true;return true;
}
bool SmrpgReplayBegin(uint32_t *input,int *width,bool custom) {
  if(playing)return !failed&&stream&&playback_begin(input,width,custom);
  const char *force=getenv("SMRPG_REPLAY_RECORD");
  if((!enabled&&(!force||!*force||*force=='0'))||failed)return true;
  const char *why=SnesReplayWriterError(writer);if(why){error(why);return true;}
  unsigned frame=(unsigned)snes_frame_counter;
  bool discontinuity=frame!=next_frame||generation!=RtlStateGeneration();
  /* Rotate before a discontinuity could make this segment exceed 2 MiB.
   * Sixty bounded segments keep checkpoints at or below 120 MiB. */
  if(!recording||rows>=SEGMENT_FRAMES||(discontinuity&&rows))if(!start_segment())return true;
  if((!rows||discontinuity)&&!write_snapshot())return true;
  generation=RtlStateGeneration();char line[96];
  snprintf(line,sizeof(line),"I %u %08x %x %u\n",frame,*input,custom,(unsigned)*width);
  if(!SnesReplayWriterLine(writer,line)){error(SnesReplayWriterError(writer));return true;}
  pending=true;return true;
}
void SmrpgReplayEnd(void) {
  if(!pending||failed)return;pending=false;uint64_t cpu[18];cpu_words(cpu);size_t size=copy_memory();
  if(!size){error("unsupported SA-1 memory layout");return;}
  if(playing){
    uint32_t actual[4]={hash(g_ram,sizeof(g_ram)),hash(cpu,sizeof(cpu)),hash(memory,size),hash(g_ppu->vram,sizeof(g_ppu->vram))};
    unsigned frame,expected[4];char line[256],tail;
    if(!fgets(line,sizeof(line),stream)||sscanf(line,"H %u %x %x %x %x %c",&frame,&expected[0],&expected[1],&expected[2],&expected[3],&tail)!=5){error("incomplete frame (original run may have crashed or hung)");return;}
    if(frame!=(unsigned)snes_frame_counter||memcmp(actual,expected,sizeof(actual))){fprintf(stderr,"[smrpg-replay] FIRST DIVERGENCE frame=%u WRAM=%08x/%08x CPU=%08x/%08x SPC+SA1=%08x/%08x VRAM=%08x/%08x\n",frame,actual[0],expected[0],actual[1],expected[1],actual[2],expected[2],actual[3],expected[3]);error("checkpoint mismatch");return;}
    ++playback_rows;
  }else{
    if(!SnesReplayWriterFrame(writer,(unsigned)snes_frame_counter,g_ram,sizeof(g_ram),cpu,sizeof(cpu),memory,size,g_ppu->vram,sizeof(g_ppu->vram)))error(SnesReplayWriterError(writer));
    ++rows;
  }
  next_frame=(unsigned)snes_frame_counter;
}

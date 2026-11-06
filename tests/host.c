/* The core of SB4 Send on a host.
 *   host file type comp sync(-1 slow) arrive_offset_us
 *       packets of an ordinary send
 *   host --part file part comp sync arrive_offset_us [prior_part]
 *       packets of one part of a P4A archive, after prior_part on the same
 *       object, as a batch does; comp is ignored, the file says
 *   host --start file start_part comp sync arrive_offset_us
 *       packets of the first part a run with that Start Part sends, with all
 *       rows of the table checked first; comp is ignored
 *   host --unit
 *       checks of names, archive headers and remaining time
 *   host --sim file wait_s jitter_ms seed
 *       the waits of a whole batch on a made-up clock, checking tap times */
#include "petit4send.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t rd(void*c,uint8_t*b,size_t n){return fread(b,1,n,(FILE*)c);}
static bool sk(void*c,uint32_t o){return fseek((FILE*)c,o,SEEK_SET)==0;}
static int multi_link(void){ const char*e=getenv("P4S_LINK"); return e?atoi(e):0; }
static int packets_multi(Petit4Send*p,long lat){
 fprintf(stderr,"%s total=%zu\n",petit4_name(p),petit4_total(p));
 petit4_restart(p);
 Petit4MultiPacket k; long n=0; uint32_t ms=1000;
 while(petit4_next_multi(p,&k)){ n++;
  int mk=((k.mods&8)?1:0)|((k.mods&2)?2:0)|((k.mods&1)?4:0)|((k.mods&4)?8:0);
  printf("DATA %ld,%d,0",(long)ms*1000+lat,mk);
  for(int b=0;b<PETIT4_MULTI_KEYBOARDS;b++) for(int i=0;i<6;i++) printf(",%d",k.keys[b][i]);
  printf("\n"); ms+=24; }
 fprintf(stderr,"packets=%ld endms=%u\n",n,ms); return 0;
}
static int packets(Petit4Send*p,int sync,long lat){
 if(multi_link()) return packets_multi(p,lat);
 fprintf(stderr,"%s total=%zu\n",petit4_name(p),petit4_total(p));
 Petit4Packet k; long n=0; uint32_t ms=1000;
 if(sync>=0) ms=petit4_fast_next(ms,sync);
 while(petit4_next_packet(p,&k)){ n++;
   int mk=((k.mods&8)?1:0)|((k.mods&2)?2:0)|((k.mods&1)?4:0)|((k.mods&4)?8:0);
   int mb=(k.mouse&7)|((k.mouse&16)>>1)|((k.mouse&8)<<1);
   printf("DATA %ld,%d,%d,%d,%d,%d,%d,%d,%d\n",(long)ms*1000+lat,mk,mb,k.keys[0],k.keys[1],k.keys[2],k.keys[3],k.keys[4],k.keys[5]);
   ms= sync>=0 ? petit4_fast_after_send(ms,sync) : ms+24; }
 fprintf(stderr,"packets=%ld endms=%u\n",n,ms); return 0;
}
static const char* open_batch(const char*path,FILE**f,Petit4Batch*b,uint32_t start){
 *f=fopen(path,"rb"); if(!*f) return "cannot open";
 uint8_t head[PETIT4_BATCH_HEAD_SIZE]; size_t got=fread(head,1,sizeof(head),*f);
 fseek(*f,0,SEEK_END); long size=ftell(*f); fseek(*f,0,SEEK_SET);
 return petit4_batch_parse(b,head,got,size,start);
}
static int part_cmd(char**av,int ac){
 FILE*f; Petit4Batch b; uint32_t part=atoi(av[3]); Petit4PartRow row;
 const char*e=open_batch(av[2],&f,&b,part);
 if(e){fprintf(stderr,"rejected: %s\n",e);return 2;}
 Petit4Send*p=petit4_alloc(rd,sk,f);
 petit4_set_multi(p,multi_link());
 if(ac>7){ /* something else on the object first */
  if(!petit4_part_begin(p,&b,atoi(av[7]),&row)){fprintf(stderr,"rejected: %s\n",petit4_error(p));return 2;}
  Petit4Packet k; for(int i=0;i<100;i++) petit4_next_packet(p,&k);
 }
 if(!petit4_part_begin(p,&b,part,&row)){fprintf(stderr,"rejected: %s\n",petit4_error(p));return 2;}
 return packets(p,atoi(av[5]),atol(av[6]));
}
static int start_cmd(char**av){
 FILE*f; Petit4Batch b; uint32_t start=atoi(av[3]);
 const char*e=open_batch(av[2],&f,&b,start);
 if(e){fprintf(stderr,"rejected: %s (parts %u)\n",e,b.part_count);return 2;}
 Petit4Send*p=petit4_alloc(rd,sk,f); Petit4Run run;
 petit4_set_multi(p,multi_link());
 petit4_run_init(&run,&b,start,30000);
 petit4_run_set_multi(&run,multi_link());
 if(!petit4_run_open(&run,p)){fprintf(stderr,"rejected: %s\n",petit4_error(p));return 2;}
 petit4_run_restart(&run,0);
 Petit4Packet k; int32_t w;
 if(petit4_run_step(&run,p,0,&k,&w)!=Petit4RunEventStartStream){fprintf(stderr,"not ready\n");return 3;}
 return packets(p,atoi(av[5]),atol(av[6]));
}
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
static uint8_t mem[64]; static size_t mem_at;
static size_t mrd(void*c,uint8_t*b,size_t n){(void)c; size_t l=sizeof(mem)-mem_at; if(n>l)n=l; memcpy(b,mem+mem_at,n); mem_at+=n; return n;}
static bool msk(void*c,uint32_t o){(void)c; mem_at=o; return true;}
static void name_is(Petit4Send*p,const char*file,const char*want){
 memset(mem,'A',sizeof(mem));
 bool ok=petit4_prepare(p,file,Petit4TypeAuto,Petit4CompNone);
 if(!ok||strcmp(petit4_name(p),want)){ printf("FAIL name %s gave %s, want %s\n",file,ok?petit4_name(p):petit4_error(p),want); fails++; }
}
static void put32(uint8_t*h,uint32_t v){ h[0]=v; h[1]=v>>8; h[2]=v>>16; h[3]=v>>24; }
static void head_of(uint8_t*h,const char*magic,uint32_t ps,uint32_t count,uint32_t image){
 memset(h,0,24); memcpy(h,magic,4); put32(h+8,ps); put32(h+12,count); put32(h+16,image);
}
static int unit(void){
 Petit4Send*p=petit4_alloc(mrd,msk,NULL);
 name_is(p,"dir/main.prg","TXT:MAIN.PRG");
 name_is(p,"MAIN.PRG","TXT:MAIN.PRG");
 name_is(p,"dir.x/map.bin","DAT:MAP.BIN");
 name_is(p,"hello.txt","TXT:HELLO");
 name_is(p,"HELLO.TXT","TXT:HELLO");
 name_is(p,"game.bas","TXT:GAME");
 name_is(p,"save.dat","DAT:SAVE");
 name_is(p,"save.DaT","DAT:SAVE");
 name_is(p,"a.png","DAT:A");
 name_is(p,"b.jpg","DAT:B");
 name_is(p,"c.JPEG","DAT:C");
 name_is(p,"a.b.txt","TXT:A.B");
 name_is(p,"a.txt.bin","DAT:A.TXT.BIN");
 name_is(p,"noext","DAT:NOEXT");
 name_is(p,".txt","TXT:_TXT");
 memset(mem,0,sizeof(mem)); mem[0]='B'; mem[1]='M'; mem[10]=54; mem[14]=40; mem[18]=1; mem[22]=1; mem[26]=1; mem[28]=24;
 CHECK(petit4_prepare(p,"pic.bmp",Petit4TypeAuto,Petit4CompNone)); CHECK(!strcmp(petit4_name(p),"GRP:PIC"));

 Petit4Batch b; uint8_t h[PETIT4_BATCH_HEAD_SIZE];
 head_of(h,"P4A4",65536,1,16);
 CHECK(petit4_batch_is_p4a(h,24)); CHECK(petit4_batch_is_p4a(h,4)); CHECK(!petit4_batch_is_p4a(h,3));
 CHECK(!petit4_batch_is_p4a((const uint8_t*)"P4A3",4));
 CHECK(petit4_batch_parse(&b,h,24,40,0)==NULL); CHECK(b.part_count==1);
 head_of(h,"P4A4",65536,2,65537);
 CHECK(petit4_batch_parse(&b,h,24,56,1)==NULL);
 CHECK(petit4_batch_parse(&b,h,24,56,2)!=NULL); CHECK(b.part_count==2);
 CHECK(!strcmp(petit4_batch_parse(&b,h,24,55,0),"P4A file too short")); /* the table is cut */
 CHECK(!strcmp(petit4_batch_parse(&b,h,12,12,0),"P4A file too short"));
 CHECK(!strcmp(petit4_batch_parse(&b,h,24,23,0),"P4A file too short"));
 head_of(h,"P4A4",65536,1,65537); CHECK(!strcmp(petit4_batch_parse(&b,h,24,99,0),"P4A part count is bad"));
 head_of(h,"P4A4",65536,3,65537); CHECK(!strcmp(petit4_batch_parse(&b,h,24,99,0),"P4A part count is bad"));
 head_of(h,"P4A3",65536,1,16); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"Not a P4A file"));
 head_of(h,"P4A1",65536,1,16); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"Old P4A: use new p4pack"));
 head_of(h,"P4A2",65536,1,16); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"Old P4A: use new p4pack")); CHECK(petit4_batch_is_p4a(h,24));
 head_of(h,"P4A4",65536,1,16);
 put32(h+20,31); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"P4A parity is bad"));
 put32(h+20,2); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"P4A parity is bad"));
 put32(h+20,66); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"P4A parity is bad"));
 put32(h+20,32); CHECK(petit4_batch_parse(&b,h,24,100,0)==NULL); CHECK(b.parity==32);
 put32(h+20,0);
 head_of(h,"P4A4",0,1,16); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"P4A part size is bad"));
 head_of(h,"P4A4",6,3,16); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"P4A part size is bad"));
 head_of(h,"P4A4",4,0,0); CHECK(!strcmp(petit4_batch_parse(&b,h,24,100,0),"P4A image size is bad"));
 head_of(h,"P4A4",4,100001,400004); CHECK(!strcmp(petit4_batch_parse(&b,h,24,1700000,0),"P4A has too many parts"));
 head_of(h,"P4A4",65536,1,16); CHECK(!strcmp(petit4_batch_parse(&b,h,24,0x80000000ULL,0),"P4A file too large"));

 /* rows: parts of 1000, an image of 2500, a file of 5000, the table ending at 72 */
 head_of(h,"P4A4",1000,3,2500); CHECK(petit4_batch_parse(&b,h,24,5000,0)==NULL);
 uint8_t raw[16]; Petit4PartRow row;
#define ROW(o,s_,g,c,z) do{ memset(raw,0,16); put32(raw,o); put32(raw+4,s_); put32(raw+8,g); raw[12]=0xCD; raw[13]=0xAB; raw[14]=c; raw[15]=z; }while(0)
 ROW(72,1012,1012,0,0); CHECK(petit4_part_row_parse(&row,&b,0,raw)==NULL);
 CHECK(row.offset==72&&row.sent==1012&&row.original==1012&&row.crc==0xABCD&&row.comp==0);
 ROW(72,500,1012,1,0); CHECK(petit4_part_row_parse(&row,&b,0,raw)==NULL);
 ROW(72,500,1012,2,0); CHECK(petit4_part_row_parse(&row,&b,0,raw)==NULL);
 ROW(72,512,512,0,0); CHECK(petit4_part_row_parse(&row,&b,2,raw)==NULL); /* 12 + 500 */
 ROW(4488,512,512,0,0); CHECK(petit4_part_row_parse(&row,&b,2,raw)==NULL); /* to the very end */
 ROW(4489,512,512,0,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,2,raw),"P4A row: past file end"));
 ROW(71,512,512,0,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,2,raw),"P4A row: past file end"));
 ROW(0xFFFFFFFFu,512,512,0,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,2,raw),"P4A row: past file end"));
 ROW(72,0xFFFFFF00u,512,0,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,2,raw),"P4A row: past file end"));
 ROW(72,1012,1016,0,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,0,raw),"P4A row: bad length"));
 ROW(72,0,1012,1,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,0,raw),"P4A row: bad sent length"));
 ROW(72,1013,1012,1,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,0,raw),"P4A row: bad sent length"));
 ROW(72,500,1012,0,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,0,raw),"P4A row: bad sent length"));
 ROW(72,500,1012,3,0); CHECK(!strcmp(petit4_part_row_parse(&row,&b,0,raw),"P4A row: bad compression"));
 ROW(72,500,1012,1,1); CHECK(!strcmp(petit4_part_row_parse(&row,&b,0,raw),"P4A row: bad compression"));
 /* padding of an unaligned image: 12 + 3 goes to 16 */
 head_of(h,"P4A4",1000,1,3); CHECK(petit4_batch_parse(&b,h,24,5000,0)==NULL);
 ROW(40,16,16,0,0); CHECK(petit4_part_row_parse(&row,&b,0,raw)==NULL);

 /* single files: the fixed rate until 5 s, the measured one after */
 CHECK(petit4_remaining_s(2060,0,0,206)==10);
 CHECK(petit4_remaining_s(2060,500,4999,206)==10);
 CHECK(petit4_remaining_s(1000,1000,5000,206)==5);
 CHECK(petit4_remaining_s(4000,1000,10000,206)==40);
 CHECK(petit4_remaining_s(4000,0,10000,206)==19);
 CHECK(petit4_remaining_s(2000000000,1000000000,300000000,206)==600000);
 CHECK(petit4_slow_rate(24)==205); CHECK(petit4_slow_rate(100)==49);

 /* fast mode is a packet with every VSYNC, as a batch's remaining time takes it */
 { uint32_t t=petit4_fast_next(1000,7),t0=t; for(int i=0;i<6000;i++) t=petit4_fast_after_send(t,7);
   long us=(long)(t-t0)*1000/6000; if(us<16500||us>16834){printf("FAIL fast packet takes %ld us\n",us);fails++;} }

 /* the tap times, at their edges: a wait ending at 10000, from 1000 */
 Petit4Tap t; petit4_tap_start(&t,1000,10000,false);
 CHECK(petit4_tap_next(&t,1499)==Petit4TapNone); CHECK(petit4_tap_next(&t,1500)==Petit4TapPress);
 petit4_tap_done(&t,Petit4TapPress,1503);
 CHECK(petit4_tap_next(&t,1552)==Petit4TapNone); CHECK(petit4_tap_next(&t,1553)==Petit4TapRelease);
 petit4_tap_done(&t,Petit4TapRelease,1553);
 CHECK(petit4_tap_next(&t,2002)==Petit4TapNone); CHECK(petit4_tap_next(&t,2003)==Petit4TapPress);
 t.last_press=8000;
 CHECK(petit4_tap_next(&t,8850)==Petit4TapPress); CHECK(petit4_tap_next(&t,8851)==Petit4TapNone);
 CHECK(petit4_tap_next(&t,9500)==Petit4TapNone);
 petit4_tap_start(&t,1000,1000,true);
 CHECK(petit4_tap_next(&t,1500)==Petit4TapPress); t.last_press=500000;
 CHECK(petit4_tap_next(&t,500500)==Petit4TapPress);
 /* the tap packets */
 Petit4Packet k; memset(&k,0xFF,sizeof(k));
 petit4_tap_packet(Petit4TapPress,&k);
 CHECK(k.keys[0]==0x28&&k.mods==0&&k.mouse==0&&!k.keys[1]&&!k.keys[5]);
 petit4_tap_packet(Petit4TapRelease,&k);
 CHECK(k.keys[0]==0&&k.mods==0&&k.mouse==0);
 /* Type Text against a small model of SB4's editor */
 static const struct{const char*file,*want;}ti[]={
  {"",""},
  {"A","A"},
  {"A\nB\n","A\nB\n"},
  {"A\n B\n  C\n   D\n  E\nF\n","A\n B\n  C\n   D\n  E\nF\n"}, /* nested */
  {"    A\nB","    A\nB"}, /* to 0, no final newline */
  {"  A\n\n  B\n\n\nC\n","  A\n\n  B\n\n\nC\n"}, /* blank lines */
  {"  A\n    \n B\n  \n\n","  A\n    \n B\n  \n\n"}, /* whitespace only */
  {"\tA\n\t\tB\n \tC\nD\tE  F\n"," A\n  B\n  C\nD\tE  F\n"}, /* tabs, one inside a line */
  {" A\r\n\r\n  B\r\n   \r\nC"," A\n\n  B\n   \nC"}, /* CRLF */
  {"  A\n   ","  A\n   "}, /* ends in whitespace */
  {"     ","     "},
  {"\n\n","\n\n"},
 };
 for(size_t n=0;n<sizeof(ti)/sizeof(ti[0]);n++){
  char ed[256]; size_t len=0; int under=0;
  Petit4Indent in; petit4_indent_init(&in);
  const char*f=ti[n].file;
  for(int end=0;!end;f++){
   bool typed=false; char c=*f;
   if(c) typed=petit4_indent_byte(&in,(uint8_t)c); else{petit4_indent_end(&in);end=1;}
   for(Petit4IndentKey key;(key=petit4_indent_key(&in))!=Petit4IndentNone;){
    if(key==Petit4IndentSpace) ed[len++]=' ';
    else if(len==0||ed[len-1]!=' ') under=1; /* would join lines, or eat text */
    else len--;
   }
   if(!typed) continue;
   ed[len++]=c;
   if(c=='\n'){ /* the editor starts the next line with this line's indent */
    size_t s=len-1; while(s>0&&ed[s-1]!='\n') s--;
    size_t e=s; while(ed[e]==' ') e++;
    for(size_t i=s;i<e;i++) ed[len++]=' ';
   }
  }
  ed[len]=0;
  if(under||strcmp(ed,ti[n].want)){printf("FAIL type text %zu: [%s] under %d\n",n,ed,under);fails++;}
 }
 { /* The NS2 link */
  uint8_t c[PETIT4_MULTI_CONFIG_SIZE]; uint16_t rs; const uint8_t*r=petit4_multi_report_desc(&rs);
  petit4_multi_config_desc(c,6); CHECK(c[2]==9+6*25); CHECK(c[4]==6); CHECK(c[9+5*25+18+2]==0x86);
  petit4_multi_config_desc(c,7);
  CHECK(c[2]==PETIT4_MULTI_CONFIG_SIZE); CHECK(c[4]==7); CHECK(rs==65); CHECK(r[rs-1]==0xC0);
  for(int i=0;i<7;i++){ uint8_t*f=c+9+i*25; CHECK(f[2]==i); CHECK(f[6]==1&&f[7]==1); CHECK(f[9+7+0]==65); CHECK(f[18+2]==0x81+i); CHECK(f[18+4]==8); }
  Petit4Packet k; memset(&k,0,sizeof(k)); k.mods=8; k.keys[0]=40; Petit4MultiPacket m; memset(&m,0xEE,sizeof(m));
  petit4_multi_from_packet(&k,&m); CHECK(m.mods==8&&m.keys[0][0]==40); for(int b=1;b<7;b++) for(int i=0;i<6;i++) CHECK(m.keys[b][i]==0);
  for(int g=16;g<=64;g*=2){
   memset(mem,0x5A,sizeof(mem));
   CHECK(petit4_prepare(p,"noext",Petit4TypeAuto,Petit4CompNone)); CHECK(petit4_set_parity(p,g)); petit4_set_multi(p,7); CHECK(petit4_restart(p));
   uint32_t n=0,lastc=99; int okc=1,held=1; Petit4MultiPacket q;
   while(petit4_next_multi(p,&q)){ n++;
    if(n<=PETIT4_MULTI_LEAD||q.keys[0][0]){ int cl=((q.mods&8)?1:0)|((q.mods&2)?2:0); if(lastc!=99&&cl!=((lastc+1)&3)) okc=0; lastc=cl;
     for(int b=0;b<7;b++) for(int i=0;i<6;i++){ if(!q.keys[b][i]) held=0; if(i&&q.keys[b][i]>=q.keys[b][i-1]) held=0; } }
   }
   CHECK(okc); CHECK(held);
   CHECK(n==petit4_part_packets_multi(7,g,petit4_total(p)-51));
   petit4_set_multi(p,0); CHECK(petit4_set_parity(p,0)); CHECK(petit4_restart(p));
  }
  /* Off has none, the link some. */
  memset(mem,0x5A,sizeof(mem)); CHECK(petit4_prepare(p,"noext",Petit4TypeAuto,Petit4CompNone)); petit4_set_multi(p,7); CHECK(petit4_restart(p));
  uint32_t n=0; Petit4MultiPacket q; while(petit4_next_multi(p,&q)) n++; CHECK(n==petit4_part_packets_multi(7,32,petit4_total(p)-51));
  petit4_set_multi(p,0);
  /* A batch's remaining time counts the link's packets. */
  Petit4Batch nb; memset(&nb,0,sizeof(nb)); nb.part_size=16384; nb.part_count=2; nb.image_size=20000; nb.parity=0; Petit4Run run; petit4_run_init(&run,&nb,0,10000); petit4_run_set_multi(&run,7);
  CHECK(run.batch.parity==32);
  uint32_t want=(uint64_t)(petit4_part_packets_multi(7,32,12+16384)+petit4_part_packets_multi(7,32,12+3616))*24000/1000000+10;
  run.stage=Petit4RunStageWait; run.part=0; run.wait_end=0; CHECK(petit4_run_remaining_s(&run,p,0,24000)==want);
  /* 6 keyboards: the same, 6 values a packet, keyboard 7's keys none. */
  memset(mem,0x5A,sizeof(mem)); CHECK(petit4_prepare(p,"noext",Petit4TypeAuto,Petit4CompNone)); petit4_set_multi(p,6); CHECK(petit4_restart(p));
  n=0; int six=1; while(petit4_next_multi(p,&q)){ n++; for(int i=0;i<6;i++) if(q.keys[6][i]) six=0; }
  CHECK(six); CHECK(n==petit4_part_packets_multi(6,32,petit4_total(p)-51));
  petit4_set_multi(p,0);
  /* No keyboard shows the same keys twice running, even over a run of one value. */
  for(int kb=6;kb<=7;kb++){ memset(mem,0,sizeof(mem)); CHECK(petit4_prepare(p,"noext",Petit4TypeAuto,Petit4CompNone)); petit4_set_multi(p,kb); CHECK(petit4_restart(p));
   Petit4MultiPacket was; memset(&was,0,sizeof(was)); int same=0; uint32_t m2=0;
   while(petit4_next_multi(p,&q)){ m2++; if(m2>PETIT4_MULTI_LEAD&&q.keys[0][0]) for(int b=0;b<kb;b++) if(!memcmp(q.keys[b],was.keys[b],6)) same=1; was=q; }
   CHECK(!same); petit4_set_multi(p,0); }
 }
 printf("unit failures %d\n",fails);
 return fails?1:0;
}
/* The worker's loop, with made-up time: each step costs jitter, a slice of
 * preparing costs more, and now and then the card stalls. */
static uint32_t rnd(uint32_t n){ return n? (uint32_t)rand()%n:0; }
/* remaining time foretold 100 packets into each part, against what it came to */
#define SIM_MAX_PARTS 4096
static long sim_said[SIM_MAX_PARTS],sim_n[SIM_MAX_PARTS];
static long sim_stream(Petit4Send*p,Petit4Send*q,const Petit4Batch*b,uint32_t part,uint32_t jitter,uint32_t*now,const Petit4Run*run){
 /* the packets must be those of a part started on its own */
 Petit4PartRow row;
 if(!petit4_part_begin(q,b,part,&row)) return -1;
 Petit4Packet k,l; long n=0;
 for(;;){
  bool ka=petit4_next_packet(p,&k),la=petit4_next_packet(q,&l);
  if(ka!=la||(ka&&memcmp(&k,&l,sizeof(k)))){ printf("FAIL part %u differs from fresh at packet %ld\n",part,n); fails++; return n; }
  if(!ka){
   if(b->parity&&n!=(long)petit4_part_packets(b->parity,row.sent)){printf("FAIL part %u: %ld packets\n",part,n);fails++;}
   if(part<SIM_MAX_PARTS) sim_n[part]=n;
   return n;
  }
  n++;
  if(n==100&&part<SIM_MAX_PARTS) sim_said[part]=petit4_run_remaining_s(run,p,*now,24000);
  *now+=24+rnd(jitter);
 }
}
static int sim(char**av){
 FILE*f; Petit4Batch b; uint32_t wait_s=atoi(av[3]),jitter=atoi(av[4]); srand(atoi(av[5]));
 const char*e=open_batch(av[2],&f,&b,0);
 if(e){fprintf(stderr,"rejected: %s\n",e);return 2;}
 FILE*f2=fopen(av[2],"rb");
 Petit4Send*p=petit4_alloc(rd,sk,f),*q=petit4_alloc(rd,sk,f2);
 Petit4Run run; petit4_run_init(&run,&b,0,wait_s*1000);
 if(!petit4_run_open(&run,p)){printf("FAIL open %s\n",petit4_error(p));return 1;}
 uint32_t now=5000; petit4_run_restart(&run,now);
 long predicted=petit4_run_remaining_s(&run,p,now,24000),total_packets=0;
 long presses=0,min_gap=1L<<40,max_gap=0,min_hold=1L<<40,max_hold=0,min_quiet=1L<<40,min_first=1L<<40,pauses=0;
 uint32_t last_press=0,last_release=0,wait_from=0,wait_began=0,confirm_taps=0;
 bool any=false,down=false,first=false,done=false;
 while(!done){
  if(run.stage==Petit4RunStageSend){
   if(any&&(long)(now-last_release)<min_quiet) min_quiet=(long)(now-last_release);
   if(down){printf("FAIL Enter down at stream start\n");fails++;}
   long n=sim_stream(p,q,&b,run.part,jitter,&now,&run);
   if(n<0) return 3;
   total_packets+=n;
   petit4_run_part_sent(&run,now);
   wait_from=wait_began=now; first=true;
   continue;
  }
  Petit4Packet k; int32_t w;
  Petit4RunEvent ev=petit4_run_step(&run,p,now,&k,&w);
  switch(ev){
  case Petit4RunEventError: printf("FAIL step error %s\n",petit4_error(p)); return 1;
  case Petit4RunEventTapPress:
   now+=rnd(jitter)+rnd(9); /* the report waits for a poll */
   if(k.keys[0]!=0x28||k.mods||k.mouse){printf("FAIL press packet\n");fails++;}
   if(first){ if((long)(now-wait_from)<min_first) min_first=(long)(now-wait_from); }
   else if(any){ long g=(long)(now-last_press); if(g<min_gap)min_gap=g; if(g>max_gap)max_gap=g; }
   first=false; last_press=now; any=true; down=true; presses++;
   petit4_tap_done(&run.tap,Petit4TapPress,now);
   if(run.stage==Petit4RunStageConfirm&&++confirm_taps>=40) done=true;
   break;
  case Petit4RunEventTapRelease:
   now+=rnd(jitter)+rnd(9);
   if(k.keys[0]||k.mods||k.mouse){printf("FAIL release packet\n");fails++;}
   { long h=(long)(now-last_press); if(h<min_hold)min_hold=h; if(h>max_hold)max_hold=h; }
   last_release=now; down=false;
   petit4_tap_done(&run.tap,Petit4TapRelease,now);
   break;
  case Petit4RunEventStartStream:
   if(any&&(long)(now-wait_began)<(long)wait_s*1000){printf("FAIL wait of %ld ms\n",(long)(now-wait_began));fails++;}
   break;
  default: {
   now+=(w>0?(uint32_t)w:1)+rnd(jitter+1)+(rnd(400)==0?rnd(40):0);
   /* now and then a pause of 3 s, as the worker's on_pause and on_resume */
   if(run.stage==Petit4RunStageWait&&rnd(3000)==0){
    if(down){ petit4_tap_done(&run.tap,Petit4TapRelease,now); down=false; last_release=now; }
    now+=3000; petit4_run_shift(&run,3000); wait_began+=3000; pauses++;
   }
  }
  }
 }
 /* what was foretold at the start, against packets and waits as they were */
 long actual=total_packets*24/1000+(b.part_count-1)*wait_s;
 printf("sim wait=%us jitter=%ums parts=%u presses=%ld gap=%ld..%ld first=%ld hold=%ld..%ld quiet=%ld pauses=%ld confirm_taps=%u foretold=%lds actual=%lds\n",
  wait_s,jitter,b.part_count,presses,min_gap,max_gap,min_first,min_hold,max_hold,min_quiet,pauses,confirm_taps,predicted,actual);
 CHECK(min_gap>=500); CHECK(min_first>=500); CHECK(min_hold>=50); CHECK(confirm_taps>=40);
 CHECK(min_quiet>=1000);
 CHECK(max_gap<=3000+500+40+(long)jitter*3+20); /* a pause may be in the gap */
 CHECK(labs(predicted-actual)<=1);
 if(b.parity){
  long later=0;
  for(long i=(long)b.part_count-1;i>=0&&i<SIM_MAX_PARTS;i--){
   long truth=(sim_n[i]-100+later)*24/1000+((long)b.part_count-1-i)*wait_s;
   if(labs(sim_said[i]-truth)>1){printf("FAIL part %ld: said %ld s, came to %ld s\n",i,sim_said[i],truth);fails++;}
   later+=sim_n[i];
  }
 }
 return fails?1:0;
}
int main(int ac,char**av){
 if(ac>1&&!strcmp(av[1],"--unit")) return unit();
 if(ac>1&&!strcmp(av[1],"--part")) return part_cmd(av,ac);
 if(ac>1&&!strcmp(av[1],"--start")) return start_cmd(av);
 if(ac>1&&!strcmp(av[1],"--sim")) return sim(av);
 FILE*f=fopen(av[1],"rb"); Petit4Send*p=petit4_alloc(rd,sk,f);
 int sync=atoi(av[4]); long lat=atol(av[5]);
 if(!petit4_prepare(p,av[1],atoi(av[2]),atoi(av[3]))){fprintf(stderr,"prepare failed: %s\n",petit4_error(p));return 2;}
 if(ac>6&&!petit4_set_parity(p,atoi(av[6]))){fprintf(stderr,"parity failed\n"); return 1;}
 petit4_set_multi(p,multi_link());
 return packets(p,sync,lat);
}

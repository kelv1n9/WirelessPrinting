import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

marlin = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/Users/elvin/Documents/My Projects/Kingroon KP3S/KP3S/Marlin-2.1.2')


def body(path, signature):
    source = (marlin / path).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


driver = body('Marlin/src/sd/disk_io_driver.h', 'class DiskIODriver') + ';'
sdio_driver = body('Marlin/src/sd/Sd2Card_sdio.h', 'class DiskIODriver_SDIO') + ';'
write = body('Marlin/src/sd/SdBaseFile.cpp', 'int16_t SdBaseFile::write(')
file_test = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
using pin_t=int;
struct csd_t {};
#define ENABLED(X) 0
#define NOMORE(A,B) do { if ((A)>(B)) (A)=(B); } while (0)
DRIVER
struct Run { uint32_t block; uint16_t count; };
struct Driver : DiskIODriver {
  std::vector<uint8_t> disk=std::vector<uint8_t>(512*256,0xCC);
  std::vector<Run> runs;
  int failAt=-1, singles=0;
  bool failBatch=false;
  bool init(uint8_t,pin_t) override {return true;}
  bool readCSD(csd_t*) override {return false;}
  bool readStart(uint32_t) override {return true;}
  bool readData(uint8_t*) override {return true;}
  bool readStop() override {return true;}
  bool writeStart(uint32_t,uint32_t) override {return true;}
  bool writeData(const uint8_t*) override {return true;}
  bool writeStop() override {return true;}
  bool readBlock(uint32_t b,uint8_t *p) override {
    assert(b<256); memcpy(p,&disk[b*512],512); return true;
  }
  bool writeBlock(uint32_t b,const uint8_t *p) override {
    assert(b<256); runs.push_back({b,1});
    if(singles++==failAt) return false;
    memcpy(&disk[b*512],p,512); return true;
  }
  bool writeBlocks(uint32_t b,const uint8_t *p,uint16_t n) override {
    assert(n==2 && b+n<=256); runs.push_back({b,n});
    if(failBatch) return false;
    memcpy(&disk[b*512],p,n*512); return true;
  }
  uint32_t cardSize() override {return 256;}
  bool isReady() override {return true;}
  void idle() override {}
};
struct SdVolume {
  enum { CACHE_FOR_WRITE=1 };
  struct Cache { alignas(4) uint8_t data[512]; } cached{};
  Driver driver;
  uint32_t cachedBlock=UINT32_MAX;
  bool dirty=false, fatOK=true, flushOK=true;
  int flushes=0, allocations=0;
  std::map<uint32_t,uint32_t> fat{{2,7},{7,UINT32_MAX}};
  uint8_t blockOfCluster(uint32_t pos) {return (pos/512)%4;}
  uint8_t blocksPerCluster() {return 4;}
  uint32_t clusterStartBlock(uint32_t c) {return 8+(c-2)*4;}
  uint32_t cacheBlockNumber() {return cachedBlock;}
  void cacheSetBlockNumber(uint32_t b,bool d) {cachedBlock=b;dirty=d;}
  Cache *cache() {return &cached;}
  DiskIODriver *sdCard() {return &driver;}
  bool writeBlock(uint32_t b,const uint8_t *p) {return driver.writeBlock(b,p);}
  bool fatGet(uint32_t c,uint32_t *n) {if(!fatOK) return false;*n=fat.at(c);return true;}
  bool isEOC(uint32_t n) {return n==UINT32_MAX;}
  bool cacheFlush() {
    ++flushes;
    if(!flushOK) return false;
    if(dirty && !writeBlock(cachedBlock,cached.data)) return false;
    dirty=false;return true;
  }
  bool cacheRawBlock(uint32_t b,bool d) {
    if(cachedBlock!=b) {
      if(!cacheFlush() || !driver.readBlock(b,cached.data)) return false;
      cachedBlock=b;
    }
    dirty|=d;return true;
  }
};
constexpr uint8_t O_WRITE=1,O_APPEND=2,O_SYNC=4,F_FILE_DIR_DIRTY=8;
struct SdBaseFile {
  SdVolume *vol_;
  uint32_t curPosition_=0,fileSize_=0,curCluster_=0,firstCluster_=2;
  uint8_t flags_=O_WRITE;
  bool writeError=false,normal=true,dateTime_=false,allocateOK=true,seekOK=true;
  int syncs=0;
  bool isFile() {return normal;}
  bool seekEnd() {curPosition_=fileSize_;curCluster_=firstCluster_;return seekOK;}
  bool addCluster() {
    ++vol_->allocations;
    if(!allocateOK) return false;
    if(!firstCluster_) firstCluster_=2;
    curCluster_=firstCluster_;return true;
  }
  bool sync() {++syncs;return vol_->cacheFlush();}
  int16_t write(const void*,uint16_t);
};
WRITE
static int sdSingles=0,sdBatches=0;
bool SDIO_Init() {return true;}
bool SDIO_ReadBlock(uint32_t,uint8_t*) {return true;}
bool SDIO_WriteBlock(uint32_t,const uint8_t*) {++sdSingles;return true;}
bool SDIO_WriteBlocks(uint32_t,const uint8_t*,uint16_t) {++sdBatches;return true;}
bool SDIO_IsReady() {return true;}
uint32_t SDIO_GetCardSize() {return 256;}
SDIO_DRIVER
int main() {
  alignas(4) uint8_t bytes[4096];
  for(unsigned i=0;i<sizeof bytes;++i) bytes[i]=uint8_t(i*17+i/512);
  Driver fallback;
  assert(fallback.DiskIODriver::writeBlocks(3,bytes,3));
  assert(fallback.singles==3 && fallback.runs.size()==3);
  assert(!memcmp(&fallback.disk[3*512],bytes,1536));
  fallback.failAt=4;
  assert(!fallback.DiskIODriver::writeBlocks(9,bytes,3));
  assert(fallback.singles==5);
  const auto calls=fallback.runs.size();
  assert(!fallback.DiskIODriver::writeBlocks(0,nullptr,2));
  assert(!fallback.DiskIODriver::writeBlocks(0,bytes,0));
  assert(!fallback.DiskIODriver::writeBlocks(UINT32_MAX,bytes,2));
  assert(fallback.runs.size()==calls);
  DiskIODriver_SDIO wrapper;
  assert(wrapper.writeBlocks(2,bytes,2));
  #ifdef ARDUINO_ARCH_STM32F1
    assert(sdBatches==1 && sdSingles==0);
  #else
    assert(sdBatches==0 && sdSingles==2);
  #endif
  SdVolume v;
  SdBaseFile f{&v};
  v.cachedBlock=200;v.dirty=true;memset(v.cached.data,0x5A,512);
  assert(f.write(bytes,1024)==1024);
  assert(f.curPosition_==1024 && f.fileSize_==1024);
  assert(v.driver.runs.size()==1 && v.driver.runs[0].count==2);
  assert(v.cachedBlock==200 && v.dirty && v.flushes==0);
  assert(!memcmp(&v.driver.disk[8*512],bytes,1024));
  v.cachedBlock=11;v.dirty=true;
  assert(f.write(bytes+1024,1024)==1024);
  assert(v.cachedBlock==UINT32_MAX && !v.dirty);
  assert(f.write(bytes+2048,1536)==1536);
  assert(v.driver.runs[2].block==28 && v.driver.runs[2].count==2);
  assert(v.driver.runs[3].block==30 && v.driver.runs[3].count==1);
  assert(!memcmp(&v.driver.disk[28*512],bytes+2048,1536));
  SdVolume edge;
  SdBaseFile e{&edge};e.curPosition_=e.fileSize_=1536;e.curCluster_=2;
  assert(e.write(bytes,1536)==1536);
  assert(edge.driver.runs.size()==2);
  assert(edge.driver.runs[0].block==11 && edge.driver.runs[0].count==1);
  assert(edge.driver.runs[1].block==28 && edge.driver.runs[1].count==2);
  SdVolume partial;
  SdBaseFile p{&partial};p.curPosition_=p.fileSize_=100;p.curCluster_=2;
  assert(p.write(bytes,1100)==1100);
  assert(partial.cacheFlush());
  assert(!memcmp(&partial.driver.disk[8*512+100],bytes,1100));
  assert(partial.driver.disk[8*512+99]==0xCC && p.fileSize_==1200);
  SdVolume failed;
  SdBaseFile bad{&failed};failed.driver.failBatch=true;
  assert(bad.write(bytes,1024)==-1 && bad.writeError);
  assert(bad.curPosition_==0 && bad.fileSize_==0 && failed.driver.singles==0);
  assert(failed.driver.runs.size()==1);
  failed.driver.failBatch=false;bad.writeError=false;
  assert(bad.write(bytes,1024)==1024);
  SdVolume boundary;
  SdBaseFile stop{&boundary};stop.curPosition_=stop.fileSize_=1536;stop.curCluster_=2;
  boundary.driver.failBatch=true;
  assert(stop.write(bytes,1536)==-1 && stop.curPosition_==2048);
  SdVolume allocation;
  SdBaseFile fresh{&allocation};fresh.firstCluster_=0;fresh.allocateOK=false;
  assert(fresh.write(bytes,1024)==-1 && allocation.driver.runs.empty());
  fresh.allocateOK=true;fresh.flags_=O_WRITE|O_SYNC;
  assert(fresh.write(bytes,1024)==1024 && fresh.syncs==1);
  fresh.flags_=0;
  assert(fresh.write(bytes,1024)==-1);
  SdVolume append;
  SdBaseFile a{&append};a.fileSize_=1024;a.flags_=O_WRITE|O_APPEND;
  assert(a.write(bytes,1024)==1024 && a.curPosition_==2048);
  assert(append.driver.runs[0].block==10);
  puts("actual SdBaseFile write, fallback and SDIO dispatch passed");
}
'''.replace('SDIO_DRIVER', sdio_driver).replace('DRIVER', driver).replace('WRITE\n', write + '\n')

source = (marlin / 'Marlin/src/HAL/STM32F1/sdio.cpp').read_text()
functions = '\n'.join(body('Marlin/src/HAL/STM32F1/sdio.cpp', sig) for sig in [
    'bool SDIO_WriteBlocks(', 'inline uint32_t SDIO_GetCardState()',
    'bool SDIO_CmdSendStatus(', 'bool SDIO_CmdWriteMultipleBlock(',
    'bool SDIO_CmdStopTransfer()', 'bool SDIO_GetCmdResp1('])
wait = source[source.index('#define SDIO_WAIT('):source.index('\nbool SDIO_GetCmdError()')]
register_test = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#define F_CPU 8000U
uint32_t flags=0;
uint32_t reads=0,lateAt=0,lateError=0;
struct Status { operator uint32_t() const {if(lateAt && ++reads>=lateAt) flags|=lateError;return flags;} };
struct Clear { void operator=(uint32_t mask) {flags&=~mask;} };
struct Registers { uint32_t FIFO=0,DCTRL=0,RESP[4]{},RESPCMD=0;Status STA;Clear ICR; } regs;
auto SDIO=&regs;
struct { uint32_t ISR=0; } dma;
auto DMA2_BASE=&dma;
constexpr int SDIO_DMA_DEV=2,SDIO_DMA_CHANNEL=4,DMA_SIZE_32BITS=4,DMA_MINC_MODE=128,DMA_FROM_MEM=16;
constexpr uint32_t DMA_ISR_TEIF4=1U<<15,DMA_ISR_TCIF4=1U<<13;
#include "SDIO_HEADER"
SDIO_CardInfoTypeDef SdCard{};
uint32_t sdioWriteCalls=0,sdioTransferUs=0,sdioBusyUs=0,sdioMaxBusyUs=0;
std::vector<std::string> events;
uint32_t tick=0, lastStatus=0;
uint16_t words=0;
int singleCalls=0;
bool dmaEnabled=false,stopping=false;
enum Fault { NONE,CMD25_TIMEOUT,CMD25_CRC,CMD25_WRONG,CMD25_R1,DATA_STALL,DMA_STALL,DMA_ERROR,
             DATA_CRC,LATE_CRC,DATA_TIMEOUT,DATA_UNDERRUN,STOP_TIMEOUT,STOP_CRC,STOP_R1,BUSY,NOT_READY,STATUS_ERROR,INITIAL_BUSY } fault=NONE;
uint32_t millis() {return tick++;}
uint32_t micros() {return tick*1000;}
void dma_disable(int,int) {dmaEnabled=false;events.push_back("disable");}
void dma_clear_isr_bits(int,int) {dma.ISR=0;}
void dma_setup_transfer(int,int,volatile void*,int,volatile void*,int,int) {
  assert(stopping==false);events.push_back("dma");
}
void dma_set_num_transfers(int,int,uint16_t n) {words=n;}
void dma_enable(int,int) {dmaEnabled=true;events.push_back("enable");}
void sdio_setup_transfer(uint32_t timeout,uint32_t length,uint32_t ctrl) {
  assert(dmaEnabled && timeout==SDIO_DATA_TIMEOUT*(words/128)*(F_CPU/1000));
  assert(length==words*4 && (ctrl&SDIO_BLOCKSIZE_512)==SDIO_BLOCKSIZE_512);
  regs.DCTRL=ctrl;events.push_back("data");
  flags=SDIO_STA_DATAEND;dma.ISR=DMA_ISR_TCIF4;
  if(fault==DATA_STALL) flags=0;
  if(fault==DMA_STALL) dma.ISR=0;
  if(fault==DMA_ERROR) dma.ISR=DMA_ISR_TEIF4;
  if(fault==DATA_CRC) flags|=SDIO_STA_DCRCFAIL;
  if(fault==LATE_CRC) {reads=0;lateAt=2;lateError=SDIO_STA_DCRCFAIL;}
  if(fault==DATA_TIMEOUT) flags|=SDIO_STA_DTIMEOUT;
  if(fault==DATA_UNDERRUN) flags|=SDIO_STA_TXUNDERR;
}
void SDIO_SendCommand(uint16_t c,uint32_t arg) {
  lateAt=0;
  const auto cmd=c&0x3F;
  regs.RESPCMD=cmd;regs.RESP[0]=0;flags=SDIO_STA_CMDREND;
  if(cmd==25) {
    events.push_back("25:"+std::to_string(arg));
    if(fault==CMD25_TIMEOUT) flags=SDIO_STA_CTIMEOUT;
    if(fault==CMD25_CRC) flags=SDIO_STA_CCRCFAIL;
    if(fault==CMD25_WRONG) regs.RESPCMD=24;
    if(fault==CMD25_R1) regs.RESP[0]=1U<<31;
  }
  else if(cmd==12) {
    assert(!dmaEnabled && !(regs.DCTRL&(SDIO_DCTRL_DTEN|SDIO_DCTRL_DMAEN)));
    events.push_back("12");stopping=true;
    if(fault==STOP_TIMEOUT) flags=SDIO_STA_CTIMEOUT;
    if(fault==STOP_CRC) flags=SDIO_STA_CCRCFAIL;
    if(fault==STOP_R1) regs.RESP[0]=1U<<31;
  }
  else {
    assert(cmd==13);events.push_back(stopping?"13:after":"13:before");
    regs.RESP[0]=(SDIO_CARD_TRANSFER<<9)|(1U<<8);
    if((stopping && fault==BUSY) || (!stopping && fault==INITIAL_BUSY)) regs.RESP[0]=7U<<9;
    if(stopping && fault==NOT_READY) regs.RESP[0]&=~(1U<<8);
    if(stopping && fault==STATUS_ERROR) flags=SDIO_STA_CTIMEOUT;
    lastStatus=regs.RESP[0];
  }
}
uint8_t SDIO_GetCommandResponse() {return regs.RESPCMD;}
uint32_t SDIO_GetResponse(uint32_t i) {return regs.RESP[i];}
bool SDIO_WriteBlock(uint32_t,const uint8_t*) {++singleCalls;return true;}
WAIT
FUNCTIONS
static void reset(Fault f=NONE) {
  fault=f;events.clear();dmaEnabled=false;stopping=false;flags=0;dma.ISR=0;regs.DCTRL=0;lateAt=0;
  words=0;singleCalls=0;tick=0;sdioWriteCalls=sdioTransferUs=sdioBusyUs=sdioMaxBusyUs=0;
  SdCard.LogBlockNbr=1000;SdCard.CardType=CARD_SDHC_SDXC;SdCard.RelCardAdd=3;
}
static void clean() {
  assert(!dmaEnabled && !(regs.DCTRL&(SDIO_DCTRL_DTEN|SDIO_DCTRL_DMAEN)));
  assert(dma.ISR==0 && flags==0 && singleCalls==0);
}
int main() {
  alignas(4) uint8_t bytes[2048]{};
  reset();assert(SDIO_WriteBlocks(9,bytes,2));clean();
  assert(words==256 && sdioWriteCalls==2 && sdioTransferUs>0 && sdioBusyUs>0);
  const std::vector<std::string> expected={"13:before","disable","25:9","dma","enable","data","disable","12","13:after"};
  assert(events==expected);
  reset();SdCard.CardType=CARD_SDSC;
  assert(SDIO_WriteBlocks(9,bytes,2));clean();assert(events[2]=="25:4608");
  reset();assert(SDIO_WriteBlocks(9,bytes,4));assert(words==512 && sdioWriteCalls==4);clean();
  for(auto f:{CMD25_TIMEOUT,CMD25_CRC,CMD25_WRONG,CMD25_R1,DATA_STALL,DMA_STALL,DMA_ERROR,
              DATA_CRC,LATE_CRC,DATA_TIMEOUT,DATA_UNDERRUN,STOP_TIMEOUT,STOP_CRC,STOP_R1,BUSY,NOT_READY,STATUS_ERROR}) {
    reset(f);assert(!SDIO_WriteBlocks(9,bytes,2));clean();
    assert(stopping && tick<1000);
    assert(sdioWriteCalls==0 || sdioWriteCalls==2);
    fault=NONE;stopping=false;events.clear();
    assert(SDIO_WriteBlocks(9,bytes,2));clean();
  }
  reset(INITIAL_BUSY);assert(!SDIO_WriteBlocks(9,bytes,2));assert(events.size()==1);
  reset();assert(!SDIO_WriteBlocks(9,nullptr,2));assert(events.empty());
  assert(!SDIO_WriteBlocks(9,bytes+1,2));assert(events.empty());
  assert(!SDIO_WriteBlocks(999,bytes,2));assert(events.empty());
  assert(!SDIO_WriteBlocks(UINT32_MAX,bytes,2));assert(events.empty());
  assert(!SDIO_WriteBlocks(0,bytes,0));assert(events.empty());
  assert(!SDIO_WriteBlocks(0,bytes,512));assert(events.empty());
  assert(SDIO_WriteBlocks(9,bytes,1) && singleCalls==1 && events.empty());
  reset();tick=UINT32_MAX-4;assert(SDIO_WriteBlocks(9,bytes,2));clean();
  reset(DATA_STALL);tick=UINT32_MAX-4;assert(!SDIO_WriteBlocks(9,bytes,2));clean();
  puts("actual SDIO DMA commands, deadlines, cleanup and failures passed");
}
'''.replace('SDIO_HEADER', 'sdio_under_test.h').replace('WAIT\n', wait + '\n').replace('FUNCTIONS', functions)

with tempfile.TemporaryDirectory(prefix='printer-sdio-test-') as temp:
    directory = Path(temp)
    (directory / 'libmaple').mkdir()
    constants = (Path('/Users/elvin/.platformio/packages/framework-arduinoststm32-maple/STM32F1/system/libmaple/include/libmaple/sdio.h')).read_text()
    constants = '\n'.join(line for line in constants.splitlines() if line.startswith('#define ') and not any(word in line for word in ['REGS', 'BASE', 'DEV', 'CHANNEL']))
    (directory / 'libmaple/sdio.h').write_text('#pragma once\n' + constants + '\n')
    (directory / 'libmaple/dma.h').write_text('#pragma once\n')
    header = (marlin / 'Marlin/src/HAL/STM32F1/sdio.h').read_text()
    (directory / 'sdio_under_test.h').write_text('\n'.join(line for line in header.splitlines() if not (line.startswith('#include') and 'MarlinConfig' in line)))
    compiler = shlex.split(os.environ.get('CXX', '/Library/Developer/CommandLineTools/usr/bin/clang++'))
    flags = ['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', temp]
    if sys.platform == 'darwin':
        flags += ['-isysroot', '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk']
    for name, program, defines in [('file_f1', file_test, ['-DARDUINO_ARCH_STM32F1']), ('file_other', file_test, []), ('registers', register_test, [])]:
        path = directory / (name + '.cpp')
        path.write_text(program)
        executable = directory / name
        subprocess.run(compiler + flags + defines + [str(path), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=20)

patch_dir = Path(__file__).resolve().parent.parent / 'patches'
patches = [patch_dir / ('printer-' + name + '.patch') for name in [
    'speed', 'speed-diagnostics', 'uart-diagnostics', 'sdio-diagnostics', 'rx-dma', 'sdio-batching', 'rx-window', 'rx-read']]
paths = sorted({name for patch in patches for name in re.findall(r'^--- a/(.+)$', patch.read_text(), re.M)})
with tempfile.TemporaryDirectory(prefix='printer-sdio-replay-') as temp:
    replay = Path(temp)
    for name in paths:
        target = replay / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(marlin / name, target)
    for reverse, sequence in [(True, reversed(patches)), (False, patches)]:
        for patch in sequence:
            command = ['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)]
            if reverse:
                command.append('-R')
            subprocess.run(command, cwd=replay, check=True, capture_output=True, text=True)
    for name in paths:
        assert (replay / name).read_bytes() == (marlin / name).read_bytes(), name
    print('Printer speed, DMA, SDIO and window patches replay byte-for-byte')

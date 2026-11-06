import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'bare/bare.cpp').read_text()


def extract(marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(source, opening):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError(marker)


helpers = '\n'.join(extract(marker) + (';' if marker.startswith('struct ') else '') for marker in (
    'static void sawLine(', 'static String metaPath(', 'static String metaReadPath(',
    'struct MetaOut :', 'static bool metaHead(', 'static uint32_t metaLayerAt(',
    'static int metaFanAt(', 'static void loosePacket(', 'static bool askedBack() {', 'static bool settleWire() {', 'static bool askPrinterCard(const char *wanted, const uint16_t from) {',
    'static String printerHandle(', 'static String openPrinterFile(', 'static String printOnPrinter('))
upload = '\n'.join(['File upInto;', '#define UP_BLOCK 16384', 'static uint8_t upBlock[UP_BLOCK]; static size_t upHeld;', 'static volatile bool relayPutting = false;', 'static bool printerWireBusy() { return false; }', 'static char sendName[64];', 'static void wifiDropTick() {}', 'static void serviceSender() {}', 'static const char EMPTY_FILE[] = "empty";', 'static const char SAME_NAME[] = "busy";'] +
                   [extract(m) for m in ('static bool uploadBegin(', 'static void cardWrite(', 'static void uploadWrite(', 'static void uploadAbort(',
                                         'static void uploadEnd(', 'static void takeUpload()')])
list_route = extract('route("/printer/files",').split('[]() ', 1)[1]
thumb_route = extract('route("/thumbnail",').split('[]() ', 1)[1]
print_route = extract('route("/printer/print",').split('[]() ', 1)[1]
telegram_print = extract('else if (verb == "/print")').split(')', 1)[1]
telegram_files = extract('else if (verb == "/files")').split(')', 1)[1]
program = r'''
#include "Arduino.h"
#include "bare/printercard.h"
#include "bare/gcodemeta.h"
#include <cassert>
#include <map>
#include <memory>
#include <vector>
#include <algorithm>
uint32_t testClock=1;
void delay(unsigned ms) { testClock+=ms; }
#define PRINTER_BAUD 115200
#define PRINTER_FAST 2000000
uint32_t linkRate=PRINTER_BAUD;
struct { void updateBaudRate(uint32_t) {} size_t write(const uint8_t *, size_t n) { return n; } } Printer;
void drain() {}
volatile uint32_t loopBeat=0;
struct TestString : String {
  using String::String;
  TestString(const String &v):String(v) {}
  int toInt() const { return std::atoi(c_str()); }
  bool startsWith(const char *v) const { return s.rfind(v, 0) == 0; }
  int indexOf(const char *v) const {
    auto at=s.find(v); return at==std::string::npos ? -1 : int(at);
  }
};
#define String TestString
static inline void journal(const TestString &) {}
static uint32_t upSince, upCardUs, upScanUs;
static uint32_t micros() { return 0; }
static int heardSync = -1;
std::map<std::string,std::shared_ptr<std::vector<uint8_t>>> disk;
constexpr int FILE_WRITE=1;
struct File {
  std::shared_ptr<std::vector<uint8_t>> bytes;
  size_t pos=0;
  std::vector<uint8_t> pending;
  bool writing=false;
  explicit operator bool() const { return bool(bytes); }
  bool isDirectory() const { return false; }
  size_t size() const { return bytes ? bytes->size() : 0; }
  void flush() { if (bytes && writing) *bytes=pending; }
  void close() { flush();bytes.reset(); }
  bool seek(size_t n) { pos=n; return true; }
  int read(uint8_t *out,size_t n) {
    if (!bytes) return 0;
    n=std::min(n,bytes->size()-std::min(pos,bytes->size()));
    std::copy_n(bytes->data()+pos,n,out); pos+=n; return int(n);
  }
  size_t write(const uint8_t *in,size_t n) {
    if (!bytes) return 0;
    assert(writing);
    pending.resize(std::max(pending.size(),pos+n));
    std::copy_n(in,n,pending.data()+pos); pos+=n; return n;
  }
  size_t write(uint8_t byte) { return write(&byte,1); }
};
struct {
  File open(const String &path,int mode=0) {
    if (mode) disk[path.s]=std::make_shared<std::vector<uint8_t>>();
    auto it=disk.find(path.s);
    return it==disk.end() ? File{} : File{it->second,0,{},bool(mode)};
  }
  bool remove(const String &path) { return disk.erase(path.s); }
  bool mkdir(const char *) { return true; }
  bool rename(const String &from,const String &to) {
    disk[to.s]=disk.at(from.s); disk.erase(from.s); return true;
  }
} SD_MMC;
enum { UPLOAD_FILE_START,UPLOAD_FILE_WRITE,UPLOAD_FILE_END,UPLOAD_FILE_ABORTED };
struct HTTPUpload { int status=0;String filename;size_t currentSize=0,totalSize=0;uint8_t *buf=nullptr; } up;
struct {
  int code=0;String body,name,cache;
  HTTPUpload &upload() { return up; }
  String arg(const char *) { return name; }
  void send(int status,const char *,const String &text) { code=status;body=text; }
  void sendHeader(const char *,const char *value) { cache=value; }
  void streamFile(File &f,const char *) { code=200;body=String(int(f.size())); }
} server;
struct { bool active=false;bool running() const { return active; } } sender;
PrinterCard printerCard,printerListing;
static bool askPrinterCard(const char *wanted = nullptr, const uint16_t from = 0);
bool printing=false,sendAsked=false,listingRequested=false,sawOk=false,autoPowerOff=false,powerArmed=false,powerSawIdle=false;
uint32_t printAt=0,printOf=0,printSince=0,heardTemp=0,powerReadySince=0,powerTries=0;
char selected[PRINTER_CARD_NAME_BYTES]={},machine[32]={};
bool sawTemps(const char *) { return false; }
void remember(const char *,size_t) {}
std::vector<std::string> commands,listing;
bool wireWorks=true;
String say(const String &, uint32_t = 0);
#define META_DIR "/meta"
HELPERS
static GcodeMetaScan metaScan;
static MetaOut metaOut;
static char metaTarget[PRINTER_CARD_NAME_BYTES];
static bool metaWanted=false,cardUp=true;
static String lastUpload,sendTrouble;
UPLOAD
void listFiles() LIST_ROUTE
void thumbnail() THUMB_ROUTE
void webPrint() PRINT_ROUTE
String chat="test",rest="1";
struct { String text;void send(const String &,const String &message) { text=message; } } telegramBot;
void telegramFiles() TELEGRAM_FILES
void telegramPrint() TELEGRAM_PRINT
void line(const std::string &s) { sawLine(s.c_str(),s.size()); }
String say(const String &cmd, uint32_t) {
  commands.push_back(cmd.s);
  if (cmd=="M105") sawOk=wireWorks;
  if (cmd=="M20 L") for (const auto &s:listing) line(s);
  if (cmd.s.find("M23 ")==0) {
    const PrinterCardEntry *e=printerCard.find(cmd.c_str()+4);
    assert(e);
    std::string reply="File opened: "+std::string(cmd.c_str()+4)+" Size: "+std::to_string(e->size);
    line(reply); return reply+"\nFile selected\n";
  }
  return "ok";
}
void put(const std::string &path,const void *data,size_t size) {
  auto f=SD_MMC.open(path,FILE_WRITE);assert(f.write((const uint8_t *)data,size)==size);f.close();
}
int main() {
  const std::string name="Boat with spaces.gcode";
  const std::string body="; total layer number: 2\n; thumbnail begin 1x1 4\n; AQID\n; thumbnail end\n;LAYER_CHANGE\n;Z:0.2\nM106 S128\n;LAYER_CHANGE\n;Z:0.4\n";
  up.filename=name;up.status=UPLOAD_FILE_START;takeUpload();
  up.buf=(uint8_t *)body.data();up.currentSize=body.size();up.status=UPLOAD_FILE_WRITE;takeUpload();
  assert(disk.at("/"+name)->empty());
  up.totalSize=body.size();up.status=UPLOAD_FILE_END;takeUpload();
  assert(disk.count("/"+name) && disk.count("/meta/"+name+".hdr"));
  GcodeMetaHead head;
  assert(metaHead(name.c_str(),head) && head.size==body.size() && head.found==2 && head.thumbLen);
  listing={"Begin file list","BOATWI~1.GCO "+std::to_string(body.size())+" "+name,"End file list"};
  assert(askPrinterCard());
  assert(openPrinterFile(name.c_str()).indexOf("File opened")>=0);
  assert(commands.back()=="M23 BOATWI~1.GCO" && selected==name);
  assert(metaHead(selected,head) && head.found==2);
  uint16_t z=0,left=0;
  assert(metaLayerAt(selected,head.found,body.size(),&z,&left)==2 && z==40);
  assert(metaFanAt(selected,head.fans,body.size())==128);
  assert(metaReadPath("boatwi~1.gco",".png")=="/meta/Boat with spaces.gcode.png");
  server.name="boatwi~1.gco";thumbnail();assert(server.code==200 && server.cache=="no-cache");
  for (const char *asked : {"Boat with spaces.gcode","boatwi~1.gco"}) {
    server.name=asked;commands.clear();webPrint();assert(server.code==200);
    assert(std::find(commands.begin(),commands.end(),"M23 BOATWI~1.GCO")!=commands.end());
    assert(commands.back()=="M24");
  }
  commands.clear();telegramPrint();assert(commands.back()=="M24" && commands[3]=="M23 BOATWI~1.GCO");
  printing=true;commands.clear();listFiles();assert(server.code==200 && server.body.indexOf("cached:")==0);
  assert(server.body.indexOf(name.c_str())>=0 && commands.empty());
  telegramFiles();assert(telegramBot.text.indexOf("Cached")>=0 && commands.empty());
  webPrint();assert(server.code==409 && commands.empty());
  assert(openPrinterFile(name.c_str()).indexOf("busy")>=0 && commands.empty());
  printing=false;
  for (auto bad : std::vector<std::vector<std::string>>{{},{"Begin file list","OTHER.GCO 2 other.gcode"},{"Begin file list","broken","End file list"}}) {
    listing=bad;assert(!askPrinterCard());assert(printerCard.holds(name.c_str(),body.size()));
    listFiles();assert(server.code==200 && server.body.indexOf("cached:")==0);
  }
  listing={"Begin file list","End file list"};assert(askPrinterCard());assert(!printerCard.count());
  printing=true;commands.clear();listFiles();assert(server.code==200 && server.body.indexOf("nothing on")>=0 && commands.empty());
  printerCard=PrinterCard();selected[0]=0;printOf=0;listFiles();
  assert(server.code==503 && server.body.indexOf("no cached")>=0 && commands.empty());
  telegramFiles();assert(telegramBot.text.indexOf("No cached")>=0 && commands.empty());
  server.name="BOATWI~1.GCO";thumbnail();assert(server.code==404);
  assert(metaHead(name.c_str(),head));
  printing=false;listing={"Begin file list","BOATWI~1.GCO "+std::to_string(body.size())+" "+name,"End file list"};
  assert(askPrinterCard());line("File opened: BOATWI~1.GCO Size: "+std::to_string(body.size()));
  assert(selected==name && metaHead(selected,head));
  line("File opened: BOATWI~1.GCO Size: 999");assert(!metaHead(selected,head));
  server.name=name;thumbnail();assert(server.code==404);
  line("start");assert(!selected[0] && !printOf && !printerCard.complete());
  server.name=name;thumbnail();assert(server.code==200);
  disk.erase("/meta/"+name+".idx");z=left=0;
  assert(metaLayerAt(name.c_str(),2,body.size(),&z,&left)==0 && !z && !left);
  assert(metaLayerAt("unknown.gcode",2,20,&z,&left)==0);
  assert(!metaHead("unknown.gcode",head));
  const char *original="part?.gcode";
  char target[PRINTER_CARD_NAME_BYTES],resolved[PRINTER_CARD_NAME_BYTES];
  assert(printerCardTarget(original,target,sizeof target));
  assert(printerCardResolve(printerCard,original,resolved,sizeof resolved) && !strcmp(target,resolved));
  listing={"Begin file list",std::string("PART~1.GCO 55 ")+target,"End file list"};assert(askPrinterCard());
  assert(printerCardResolve(printerCard,"part~1.gco",resolved,sizeof resolved) && !strcmp(target,resolved));
  assert(openPrinterFile(original).indexOf("File opened")>=0 && commands.back()==std::string("M23 ")+target);
  assert(!strcmp(selected,target));
  head={};head.magic=GCODEMETA_MAGIC;head.size=55;head.layers=9;
  put(std::string("/meta/")+target+".hdr",&head,sizeof head);
  assert(metaHead(original,head) && head.layers==9);
  assert(!printerCardResolve(printerCard,"",resolved,sizeof resolved));
  assert(!printerCardResolve(printerCard,name.c_str(),resolved,2));
  printOf=0;selected[0]=0;
  up.filename=name;up.status=UPLOAD_FILE_START;takeUpload();
  assert(!disk.count("/meta/"+name+".png") && !metaHead(name.c_str(),head));
  up.buf=(uint8_t *)"G28\n";up.currentSize=4;up.status=UPLOAD_FILE_WRITE;takeUpload();
  up.totalSize=4;up.status=UPLOAD_FILE_END;takeUpload();
  assert(metaHead(name.c_str(),head) && !head.found && !head.layers && !head.thumbLen);
  server.name=name;thumbnail();assert(server.code==404);
  up.status=UPLOAD_FILE_START;takeUpload();up.status=UPLOAD_FILE_ABORTED;takeUpload();
  assert(!metaHead(name.c_str(),head) && sendTrouble=="upload aborted");
  up.status=UPLOAD_FILE_START;takeUpload();up.status=UPLOAD_FILE_WRITE;takeUpload();
  sendTrouble="write failed";up.status=UPLOAD_FILE_END;takeUpload();assert(!metaHead(name.c_str(),head));
  listing={"Begin file list","SPEED~67.GCO 1010991 speed115.gcode","SPEED~67.GCO 20219820 speed20mb.gcode","End file list"};
  assert(askPrinterCard());
  bool ambiguous=false;
  assert(!printerCard.find("SPEED~67.GCO",&ambiguous) && ambiguous);
  assert(!printerCardResolve(printerCard,"SPEED~67.GCO",resolved,sizeof resolved));
  commands.clear();assert(openPrinterFile("SPEED~67.GCO").indexOf("File opened")<0 && commands.empty());
  for (const char *name : {"speed115.gcode","speed20mb.gcode"}) {
    commands.clear();assert(openPrinterFile(name).indexOf("File opened")>=0);
    assert(commands.size()==1 && commands.back()==std::string("M23 ")+name);
  }
  selected[0]=0;
  for (const char *size : {"1010991","20219820"}) {
    line(std::string("File opened: SPEED~67.GCO Size: ")+size);
    assert(!selected[0] && !metaHead("SPEED~67.GCO",head));
  }
  sender.active=true;commands.clear();assert(!askPrinterCard() && commands.empty());
  sender.active=false;sendAsked=true;assert(!askPrinterCard() && commands.empty());
  puts("PASS UI metadata: buffered upload/header size, duplicate DOS safety, M23, cache, web/Telegram, reboot and missing sidecars");
}
'''
for key, value in {'HELPERS': helpers, 'UPLOAD': upload, 'LIST_ROUTE': list_route,
                   'THUMB_ROUTE': thumb_route, 'PRINT_ROUTE': print_route,
                   'TELEGRAM_FILES': telegram_files, 'TELEGRAM_PRINT': telegram_print}.items():
    program = re.sub(r'\b' + key + r'\b', lambda _: value, program)
with tempfile.TemporaryDirectory(prefix='ui-metadata-') as tmp:
    cpp, exe = Path(tmp) / 'check.cpp', Path(tmp) / 'check'
    cpp.write_text(program)
    compiler = shlex.split(os.environ.get('CXX', 'c++')) + shlex.split(os.environ.get('CXXFLAGS', ''))
    subprocess.run(compiler + ['-std=c++14', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                              '-I', str(root / 'test/stubs'), '-I', str(root), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

gone = extract('route("/delete"')
assert 'relayPutting || uploadActive || printerWireBusy()' in gone, 'deleting a file that is still arriving or leaving took the module down'
assert 'printerWireBusy()' in extract('route("/printer/delete"'), 'the printer card is not to be touched while a transfer is starting'

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source_path = root / 'bare/bare.cpp'
original = source_path.read_bytes()
source = original.decode()


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
            if depth == 0:
                return source[start:token.end()]
    raise AssertionError('Unclosed source block: ' + marker)


completion = extract('if (chore == ChoreSending && !sender.running() && !sendAsked)')
bin_branch = completion.split('else if (!worthPacking(choreTarget)) {', 1)[1].split('else if (!askPrinterCard())', 1)[0]
assert bin_branch.count('say("M21");') == 1, 'Expected one bin remount for mutation check'
mutant = completion.replace('say("M21");', '', 1)
helpers = '\n'.join(extract(marker) for marker in ('static bool worthPacking(', 'static bool settleWire() {', 'static bool askPrinterCard() {'))
chore_enum = re.search(r'enum Chore\s*:\s*uint8_t\s*\{[^}]+\};', source).group()
binary_header = (root / 'ESP8266WirelessPrintAsync/BinaryTransfer.h').read_text()
binary_enum = re.search(r'enum BinaryPhase\s*:\s*uint8_t\s*\{[^}]+\};', binary_header).group()
program = r'''
#include "Arduino.h"
#include "bare/printercard.h"
#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <strings.h>
#include <vector>
uint32_t testClock=0;
void delay(unsigned ms) { testClock+=ms; }
struct CompletionString : String {
  using String::String;
  CompletionString(const String &value):String(value) {}
  int indexOf(const char *needle) const {
    const auto at=s.find(needle);
    return at==std::string::npos ? -1 : static_cast<int>(at);
  }
};
#define String CompletionString
BINARY_ENUM
CHORE_ENUM
struct {
  BinaryPhase phase=BinaryDone;
  const char *trouble=nullptr;
  bool active=false;
  bool running() const { return active; }
} sender;
struct { uint32_t size() const { return 4096; } } sendBytes;
Chore chore;
String choreTrouble;
char choreTarget[PRINTER_CARD_NAME_BYTES];
bool choreShouldPrint=false,sendAsked=false,sawOk=false;
PrinterCard printerCard;
std::vector<std::string> commands;
const char *scenario="";
unsigned checks=0,deletions=0;
bool mounted=false,mountWorks=true,wireWorks=true,readWorks=true,listEnd=true,openWorks=true;
std::string listedName,openedReply;
uint32_t listedSize=4096;
void require(bool good,const char *message) {
  ++checks;
  if (!good) {
    std::fprintf(stderr,"FAIL %s: %s\n",scenario,message);
    std::exit(1);
  }
}
struct {
  bool remove(const String &) { ++deletions;return true; }
} SD_MMC;
String say(const String &text) {
  const std::string cmd=text.c_str();
  commands.push_back(cmd);
  if (cmd=="M105") {
    sawOk=wireWorks;
    return wireWorks ? "ok" : "";
  }
  if (cmd=="M21") {
    mounted=mountWorks;
    return mounted ? "SD card ok" : "SD init fail";
  }
  if (cmd=="M20 L") {
    if (!mounted || !readWorks) return "SD read error";
    printerCard.observe("Begin file list");
    char line[160];
    std::snprintf(line,sizeof line,"%s %lu",listedName.c_str(),static_cast<unsigned long>(listedSize));
    printerCard.observe(line);
    if (listEnd) printerCard.observe("End file list");
    return "ok";
  }
  if (cmd.compare(0,4,"M23 ")==0) {
    if (!mounted || !readWorks || !openWorks) return "open failed";
    if (!openedReply.empty()) return openedReply;
    char reply[160];
    std::snprintf(reply,sizeof reply,"File opened: %s Size: %lu\nFile selected\nok\n",
                  cmd.c_str()+4,static_cast<unsigned long>(sendBytes.size()));
    return reply;
  }
  if (cmd=="M27 S3" || cmd=="M24") return "ok";
  if (cmd.compare(0,3,"M30")==0) ++deletions;
  require(false,"unexpected printer command or deletion");
  return "";
}
HELPERS
void complete() { COMPLETION }
void complete_without_bin_mount() { MUTANT }
void (*finish)()=complete;
void reset(const char *name,bool print=false) {
  scenario=name;
  chore=ChoreSending;choreTrouble="";choreShouldPrint=print;sendAsked=false;
  sender.phase=BinaryDone;sender.active=false;sender.trouble=nullptr;
  std::snprintf(choreTarget,sizeof choreTarget,"%s",name);
  commands.clear();deletions=0;
  mounted=false;mountWorks=wireWorks=readWorks=listEnd=openWorks=true;
  listedName=name;listedSize=sendBytes.size();openedReply.clear();
  printerCard.begin();
  printerCard.observe("Begin file list");
  char stale[160];
  std::snprintf(stale,sizeof stale,"%s 4096",name);
  printerCard.observe(stale);
  printerCard.observe("End file list");
  require(printerCard.complete(),"stale index fixture is complete before binary close");
}
bool sent(const char *cmd) {
  return std::find(commands.begin(),commands.end(),cmd)!=commands.end();
}
void expect_commands(std::initializer_list<const char *> expected) {
  std::vector<std::string> wanted(expected.begin(),expected.end());
  require(commands==wanted,"command order differs");
}
void failed() {
  require(chore==ChoreFailed,"verification failure must fail the chore");
  require(choreTrouble.length()>0,"failure must retain its reason");
  require(!sent("M24"),"verification failure must never start printing");
  require(deletions==0,"verification failure must never delete files");
}
int main(int argc,char **) {
  (void)SD_MMC;
  if (argc>1) finish=complete_without_bin_mount;
  reset("Robin_nano35.bin",true);
  finish();
  require(chore==ChoreDone,"bin verification must remount the card after binary close");
  expect_commands({"M21","M23 Robin_nano35.bin"});
  require(!sent("M24") && deletions==0,"firmware must not print or be deleted");
  for (const char *reply : {"File opened: other.bin Size: 4096\n",
                           "File opened: Robin_nano35.bin Size: 4095\n",
                           "File opened: Robin_nano35.bin Size: 40960\n",
                           "File opened: Robin_nano35.bin Size: 4096junk\n",
                           "echo:File opened: Robin_nano35.bin Size: 4096\n",
                           "File selected\nok\n"}) {
    reset("Robin_nano35.bin");openedReply=reply;finish();failed();
  }
  for (int fault=0;fault<3;++fault) {
    reset("Robin_nano35.bin");
    if (fault==0) mountWorks=false;
    if (fault==1) readWorks=false;
    if (fault==2) openWorks=false;
    finish();failed();
  }
  reset("PART.GCO");finish();
  require(chore==ChoreDone && mounted && printerCard.complete(),"gcode must use a fresh complete listing");
  expect_commands({"M105","M21","M20 L"});
  require(!sent("M24") && deletions==0,"send-only must not print or delete");
  for (bool print : {false,true}) for (int fault=0;fault<6;++fault) {
    reset("PART.GCO",print);
    if (fault==0) listedSize=4095;
    if (fault==1) listedName="OTHER.GCO";
    if (fault==2) readWorks=false;
    if (fault==3) listEnd=false;
    if (fault==4) mountWorks=false;
    if (fault==5) wireWorks=false;
    finish();failed();
    require(!sent("M23 PART.GCO"),"bad listing must not reach file selection");
    if (fault==1 || fault==3) require(!printerCard.holds("PART.GCO",4096),"old successful listing must not survive refresh");
  }
  reset("PART.GCO",true);openWorks=false;finish();failed();
  require(!sent("M27 S3"),"failed open must not enable print reporting");
  reset("PART.GCO",true);finish();
  require(chore==ChoreDone && deletions==0,"verified print request must complete without deletion");
  expect_commands({"M105","M21","M20 L","M23 PART.GCO","M27 S3","M24"});
  reset("PART.GCO",true);sender.phase=BinaryFailed;sender.trouble="source read failed";
  finish();failed();require(commands.empty(),"failed transfer must not verify or start");
  for (int gate=0;gate<3;++gate) {
    reset("PART.GCO",true);
    if (gate==0) sender.active=true;
    if (gate==1) sendAsked=true;
    if (gate==2) chore=ChoreDone;
    const Chore before=chore;
    finish();require(chore==before && commands.empty() && deletions==0,"completion must wait for the correct state");
  }
  std::printf("PASS transfer completion: %u checks, extracted bare.cpp flow, fresh card verification and print ordering\n",checks);
}
'''
for key, value in {'BINARY_ENUM': binary_enum, 'CHORE_ENUM': chore_enum, 'HELPERS': helpers,
                   'COMPLETION': completion, 'MUTANT': mutant}.items():
    program = program.replace(key, value)
with tempfile.TemporaryDirectory(prefix='transfer-completion-') as tmp:
    cpp = Path(tmp) / 'check.cpp'
    exe = Path(tmp) / 'check'
    cpp.write_text(program)
    compiler = shlex.split(os.environ.get('CXX', 'c++')) + shlex.split(os.environ.get('CXXFLAGS', ''))
    subprocess.run(compiler + ['-std=c++14', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                              '-fno-omit-frame-pointer', '-I', str(root / 'test/stubs'), '-I', str(root),
                              str(cpp), '-o', str(exe)], check=True)
    env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1')
    subprocess.run([str(exe)], check=True, env=env)
    result = subprocess.run([str(exe), '--without-bin-m21'], capture_output=True, text=True, env=env)
    expected = 'FAIL Robin_nano35.bin: bin verification must remount the card after binary close'
    assert result.returncode == 1 and expected in result.stderr, result.stdout + result.stderr
    print('PASS mutation: removing only bin say("M21") fails the completion check')
assert source_path.read_bytes() == original, 'Production source changed during the test'

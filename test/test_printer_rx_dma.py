import os
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile

marlin = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('/Users/elvin/Documents/My Projects/Kingroon KP3S/KP3S/Marlin-2.1.2')
header = (marlin / 'Marlin/src/HAL/STM32F1/MarlinSerial.h').read_text()
source = (marlin / 'Marlin/src/HAL/STM32F1/MarlinSerial.cpp').read_text()
cursor = header.split('struct PrinterRxDmaCursor {', 1)[1].split('\n};', 1)[0]
serial = header.split('struct MarlinSerial : public HardwareSerial {', 1)[1].split('\n};', 1)[0]
driver = source.split('static PrinterRxDmaCursor printerRxDma;', 1)[1].split('\n// Copied from ', 1)[0]
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
struct PrinterRxDmaCursor {CURSOR
};
#define MB(...) 1
#define ENABLED(...) 1
#define HAS_SPI_TFT 0
#define SPI_DEVICE 2
#define UART_IRQ_PRIO 1
using uint8=uint8_t;
using uint32=uint32_t;
constexpr int DMA_CH3=3, DMA_ISR_TCIF=2, DMA_ISR_TEIF=8, USART_CR3_DMAR=64;
constexpr int USART_CR1_RXNEIE=32, USART_CR1_PEIE=256, USART_CR3_EIE=1;
constexpr int DMA_SIZE_8BITS=0, DMA_MINC_MODE=128, DMA_CIRC_MODE=32;
constexpr int DMA_TRNS_CMPLT=2, DMA_TRNS_ERR=8, DMA_PRIORITY_VERY_HIGH=3;
struct usart_reg_map { uint32_t CR1=USART_CR1_RXNEIE, CR3=USART_CR3_DMAR, DR=0; } regs;
struct ring_buffer { uint16_t head=0,tail=0,size=1279; volatile uint8_t *buf; } rb;
struct usart_dev { usart_reg_map *regs; ring_buffer *rb; int irq_num; } uart{&regs,&rb,3};
auto USART3=&uart;
struct { struct { void (*handler)()=nullptr; } handlers[7]; } dma;
auto DMA1=&dma;
uint8_t flags=0;
uint16_t remaining=1280;
bool enabled=true, cross_during_sample=false;
uint8_t dma_get_isr_bits(decltype(DMA1), int) { return flags; }
void dma_clear_isr_bits(decltype(DMA1), int) { flags=0; }
void dma_disable(decltype(DMA1), int) { enabled=false; }
bool dma_is_enabled(decltype(DMA1), int) { return enabled; }
uint16_t dma_get_count(decltype(DMA1), int) {
  if (cross_during_sample) {
    cross_during_sample=false;
    flags=DMA_ISR_TCIF;
    remaining=1279;
    return 1;
  }
  return remaining;
}
void dma_init(decltype(DMA1)) {}
void dma_setup_transfer(decltype(DMA1),int,volatile void*,int,volatile void*,int,int) {}
void dma_set_num_transfers(decltype(DMA1),int,uint16_t count) { remaining=count; }
void dma_set_priority(decltype(DMA1),int,int) {}
void dma_attach_interrupt(decltype(DMA1),int,void (*handler)()) { dma.handlers[2].handler=handler; }
void dma_detach_interrupt(decltype(DMA1),int) { dma.handlers[2].handler=nullptr; }
void dma_enable(decltype(DMA1),int) { enabled=true; }
bool masked=false,uart_irq=true;
bool __get_primask() { return masked; }
void nvic_globalirq_disable() { masked=true; }
void nvic_globalirq_enable() { masked=false; }
void nvic_irq_disable(int) { uart_irq=false; }
void nvic_irq_enable(int) { uart_irq=true; }
void nvic_irq_set_priority(int,int) {}
void rb_init(ring_buffer *r,uint16_t n,volatile uint8_t *buf) { r->head=r->tail=0;r->size=n-1;r->buf=buf; }
struct HardwareSerial {
  usart_dev *dev;
  HardwareSerial(usart_dev *d,uint8_t,uint8_t):dev(d) {}
  virtual ~HardwareSerial()=default;
  usart_dev *c_dev() { return dev; }
  void begin(uint32_t,uint8_t) { dev->regs->CR1=USART_CR1_RXNEIE; }
  void end() { dev->regs->CR1=0; }
  virtual int available() { return (dev->rb->tail+1280-dev->rb->head)%1280; }
  virtual int peek() { return available() ? dev->rb->buf[dev->rb->head] : -1; }
  virtual int read() {
    const int byte=peek();
    if (byte>=0) dev->rb->head=(dev->rb->head+1)%1280;
    return byte;
  }
};
constexpr uint8_t SERIAL_8N1=0;
struct MarlinSerial : public HardwareSerial {SERIAL
};
using MSerialT=MarlinSerial;
MSerialT MSerial3(USART3,0,0);
volatile uint32_t printerUartFull=0;
uint32_t printerUartDmaStarts=0;
static PrinterRxDmaCursor printerRxDma;DRIVER
int main() {
  assert(!printerRxDma.active && printerRxDmaCr1==0 && printerRxDmaCr3==0);
  constexpr uint16_t N=PrinterRxDmaCursor::capacity;
  uint8_t buffer[N];
  for (uint16_t tail=0; tail<N; ++tail) {
    for (uint16_t count : {uint16_t(0), uint16_t(1), uint16_t(1034), uint16_t(1279)}) {
      const uint16_t head=(tail+N-count)%N;
      for (uint16_t i=0; i<N; ++i) buffer[i]=uint8_t(i*17+tail);
      std::deque<int> expected;
      for (uint16_t i=0; i<count; ++i) expected.push_back(buffer[(head+i)%N]);
      PrinterRxDmaCursor c;
      assert(!c.active);
      c.begin(buffer,head,tail);
      assert(c.active && c.available(0)==count);
      while (!expected.empty()) {
        assert(c.peek(buffer,0)==expected.front());
        assert(c.read(buffer,0)==expected.front());
        expected.pop_front();
      }
      assert(c.read(buffer,0)==-1);
      assert(!c.end(0,0) && !c.active && c.head==0);
    }
  }
  PrinterRxDmaCursor c;
  c.begin(buffer,0,0);
  uint32_t produced=0;
  uint16_t writer=0;
  std::deque<int> expected;
  for (unsigned packet=0; packet<10000; ++packet) {
    for (unsigned i=0; i<1034; ++i) {
      buffer[writer]=uint8_t(produced);
      expected.push_back(buffer[writer]);
      writer=(writer+1)%N;
      ++produced;
    }
    assert(c.available(produced)==1034);
    for (unsigned i=0; i<1000; ++i) {
      assert(c.read(buffer,produced)==expected.front());
      expected.pop_front();
    }
    assert(!c.end(produced,writer));
    assert(!c.active && (writer+N-c.head)%N==34);
    const uint16_t old_head=c.head;
    c.begin(buffer,old_head,writer);
    produced=0;writer=0;
    while (!expected.empty()) {
      assert(c.read(buffer,produced)==expected.front());
      expected.pop_front();
    }
  }
  c.consumed=std::numeric_limits<uint32_t>::max()-20;
  assert(c.available(19)==40);
  assert(!c.end(19,100) && !c.active);
  c.begin(buffer,0,0);
  assert(c.end(N,0) && c.head==1 && c.available(N)==N-1);
  c.begin(buffer,c.head,0);
  assert(c.available(0)==N-1);
  assert(c.end(3*N+17,17) && c.head==18);
  uint16_t tail;
  printerRxDmaProduced=0;
  remaining=N;
  assert(printer_rx_dma_position(tail)==0 && tail==0);
  remaining=1;
  assert(printer_rx_dma_position(tail)==N-1 && tail==N-1);
  flags=DMA_ISR_TCIF;remaining=N;
  assert(printer_rx_dma_position(tail)==N && tail==0);
  assert(printer_rx_dma_position(tail)==N);
  remaining=1;cross_during_sample=true;
  assert(printer_rx_dma_position(tail)==2*N+1 && tail==1);
  printer_rx_dma_irq();
  assert(printer_rx_dma_position(tail)==2*N+1);
  enabled=false;remaining=0;flags=DMA_ISR_TCIF;
  assert(printer_rx_dma_position(tail)==3*N && tail==0);
  flags=DMA_ISR_TEIF;remaining=N-17;enabled=true;
  assert(printer_rx_dma_position(tail)==3*N+17);
  assert(printerRxDmaError && !enabled && !(regs.CR3 & USART_CR3_DMAR));
  regs.CR1=USART_CR1_RXNEIE|USART_CR1_PEIE;
  regs.CR3=USART_CR3_EIE;
  rb_init(&rb,N,buffer);
  buffer[0]='\n';rb.tail=1;
  assert(MSerial3.beginRxDma() && uart_irq);
  assert(!(regs.CR1 & (USART_CR1_RXNEIE|USART_CR1_PEIE)) && (regs.CR3 & USART_CR3_DMAR));
  assert(MSerial3.read()=='\n');
  buffer[0]='M';buffer[1]='1';buffer[2]='0';buffer[3]='5';remaining=N-4;
  assert(MSerial3.peek()=='M' && MSerial3.read()=='M');
  MSerial3.endRxDma();
  assert(!printerRxDma.active && !enabled && !masked);
  assert((regs.CR1 & USART_CR1_RXNEIE) && (regs.CR1 & USART_CR1_PEIE));
  assert((regs.CR3 & USART_CR3_EIE) && !(regs.CR3 & USART_CR3_DMAR));
  assert(!dma.handlers[2].handler && MSerial3.read()=='1');
  assert(MSerial3.beginRxDma() && MSerial3.read()=='0');
  MSerial3.end();
  assert(!printerRxDma.active && !enabled && regs.CR1==0);
  MSerial3.begin(2000000);
  assert(!printerRxDma.active && (regs.CR1 & USART_CR1_RXNEIE));
  assert(MSerial3.beginRxDma());
  MSerial3.begin(115200);
  assert(!printerRxDma.active && !enabled && (regs.CR1 & USART_CR1_RXNEIE));
  enabled=true;
  assert(!MSerial3.beginRxDma());
  enabled=false;
  usart_reg_map other_regs;
  ring_buffer other_rb;rb_init(&other_rb,N,buffer);
  usart_dev other_dev{&other_regs,&other_rb,1};
  MarlinSerial other(&other_dev,0,0);
  assert(!other.beginRxDma() && (other_regs.CR1 & USART_CR1_RXNEIE));
  std::puts("RX DMA: FIFO transitions, 1034-byte packets, wrap, overflow, uint32 wrap, pending TC, sample race, DMA error passed");
}
'''.replace('CURSOR', cursor).replace('SERIAL\n', serial+'\n').replace('DRIVER', driver)
with tempfile.TemporaryDirectory(prefix='printer-rx-dma-check-') as tmp:
    cpp = Path(tmp) / 'check.cpp'
    exe = Path(tmp) / 'check'
    cpp.write_text(program)
    subprocess.run([os.environ.get('CXX', 'c++')] + shlex.split(os.environ.get('CXXFLAGS', '')) + ['-std=c++14', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

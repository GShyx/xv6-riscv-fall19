#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

// SOL_E1000
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char tx_data[TX_RING_SIZE][DATA_MAX];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char rx_data[RX_RING_SIZE][2048];
// END_E1000

// remember where the e1000's registers live.
static volatile uint32 *regs;

// SOL_E1000
struct spinlock e1000_lock;
// END_E1000

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
// SOL_E1000
  int i;

  initlock(&e1000_lock, "e1000");
// END_E1000

  regs = xregs;

  //
  // Your code here.
  // Set up transmit and receive DMA rings.
  //

// SOL_E1000
  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].addr = (uint64) tx_data[i];
    tx_ring[i].status = E1000_TXD_STAT_DD;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_ring[i].addr = (uint64) rx_data[i];
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);
// END_E1000

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXT0 -- RX timer expiry
}

int
e1000_transmit(const char *buf, unsigned int len)
{
  //
  // Your code here.
  //
  // buf contains an ethernet frame; copy it to the
  // tx descriptor ring so that the e100 sends it.
  //
// SOL_E1000
  if (!regs || len > DATA_MAX)
    return -1;

  acquire(&e1000_lock);
        
  int tail = regs[E1000_TDT];

  // [E1000 3.3.3.2] Check if this descriptor is done.
  // According to [E1000 13.4.39], using TDH for this is not
  // reliable.
  if (!(tx_ring[tail].status & E1000_TXD_STAT_DD)) {
    release(&e1000_lock);
    printf("TX ring overflow\n");
    return 0;
  }
  
  // Fill in the next descriptor
  memmove(tx_data[tail], buf, len);
  tx_ring[tail].length = len;
  tx_ring[tail].status = 0;
  // Set EOP to actually send this packet.  Set RS to get DD
  // status bit when sent.
  tx_ring[tail].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  
  __sync_synchronize();
  
  // Move the tail pointer
  regs[E1000_TDT] = (tail + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  
  printf("sent a packet\n");
// END_E1000
  
  return 0;
}

void
e1000_intr(void)
{
  //
  // Your code here.
  //
  // trap.c calls e1000_intr() when the e1000
  // raises an interrupt.
  //
// SOL_E1000
  printf("e1000_intr\n");

  acquire(&e1000_lock);

  // XXX tx complete, for waiting sender when ring is full?

  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR];

  wakeup(&e1000_lock);

  release(&e1000_lock);
// END_E1000
}

// blocks; doesn't return until a packet is available.
// caller owns the returned packet buffer and should kfree() it.
void
e1000_recv(char **buf, int *len)
{
  //
  // Your code here.
  //
  // Wait for a packet to arrive from the e1000
  // and copy it into buf.
  //
  // SOL_E1000
  while(1){
    int tail;
    
    acquire(&e1000_lock);
  
    // any packets waiting in the receive DMA ring?
    // RDT points to one before the place the
    // driver should next look.
    while(1){
      tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

      if (rx_ring[tail].status & E1000_RXD_STAT_DD)
        break;

      // there's no packet in the next descriptor;
      // wait for an interrupt.
      sleep(&e1000_lock, &e1000_lock);
    }

    __sync_synchronize();
    *len = rx_ring[tail].length;
    if(*len > PGSIZE)
      panic("e1000 len");
    *buf = kalloc();
    if(*buf == 0)
      panic("e1000 kalloc");
    memmove(*buf, rx_data[tail], *len);

    rx_ring[tail].status = 0;
    __sync_synchronize();
  
    // Move the tail pointer
    regs[E1000_RDT] = tail % RX_RING_SIZE;

    release(&e1000_lock);

    printf("e1000_recv got pkt index=%d len=%d\n", tail, *len);

    return;
  }
  // END_E1000
}

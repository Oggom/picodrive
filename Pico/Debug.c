#include "PicoInt.h"

// note: set SPLIT_MOVEL_PD to 0

typedef unsigned char  u8;

static unsigned int pppc, ops=0;
extern unsigned int lastread_a, lastread_d[16], lastwrite_cyc_d[16], lastwrite_mus_d[16];
extern int lrp_cyc, lrp_mus, lwp_cyc, lwp_mus;
unsigned int old_regs[16], old_sr, ppop, have_illegal = 0, dbg_irq_level = 0;

#undef dprintf
#define dprintf(f,...) printf("%05i:%03i: " f "\n",Pico.m.frame_count,Pico.m.scanline,##__VA_ARGS__)

#if defined(EMU_C68K)
#define other_get_sr()     CycloneGetSr(&PicoCpuCM68k)
#define other_dar(i)       PicoCpuCM68k.d[i]
#define other_osp          PicoCpuCM68k.osp
#define other_get_irq()    PicoCpuCM68k.irq
#define other_set_irq(irq) PicoCpuCM68k.irq=irq
#define other_is_stopped()  (PicoCpuCM68k.state_flags&1)
#define other_is_tracing() ((PicoCpuCM68k.state_flags&2)?1:0)
#elif defined(EMU_F68K)
#define other_get_sr()     PicoCpuFM68k.sr
#define other_dar(i)       ((unsigned int*)PicoCpuFM68k.dreg)[i]
#define other_osp          PicoCpuFM68k.asp
#define other_get_irq()    PicoCpuFM68k.interrupts[0]
#define other_set_irq(irq) PicoCpuFM68k.interrupts[0]=irq
#define other_is_stopped() ((PicoCpuFM68k.execinfo&FM68K_HALTED)?1:0)
#define other_is_tracing() ((PicoCpuFM68k.execinfo&FM68K_EMULATE_TRACE)?1:0)
#else
#error other core missing, don't compile this file
#endif

static int otherRun(void)
{
#if defined(EMU_C68K)
  PicoCpuCM68k.cycles=1;
  CycloneRun(&PicoCpuCM68k);
  return 1-PicoCpuCM68k.cycles;
#elif defined(EMU_F68K)
  return fm68k_emulate(1);
#endif
}

//static
void dumpPCandExit()
{
  char buff[128];
  int i;

  m68k_disassemble(buff, pppc, M68K_CPU_TYPE_68000);
  dprintf("PC: %06x: %04x: %s", pppc, ppop, buff);
  dprintf("                    this | prev");
  for(i=0; i < 8; i++)
    dprintf("d%i=%08x, a%i=%08x | d%i=%08x, a%i=%08x", i, other_dar(i), i, other_dar(i+8), i, old_regs[i], i, old_regs[i+8]);
  dprintf("SR:                 %04x | %04x (??s? 0iii 000x nzvc)", other_get_sr(), old_sr);
  dprintf("last_read: %08x @ %06x", lastread_d[--lrp_cyc&15], lastread_a);
  dprintf("ops done: %i", ops);
  exit(1);
}

int CM_compareRun(int cyc)
{
  char *str;
  int cyc_done=0, cyc_other, cyc_musashi, err=0;
  unsigned int i, mu_sr;

  lrp_cyc = lrp_mus = 0;

  while(cyc > cyc_done)
  {
    if (have_illegal && m68k_read_disassembler_16(m68ki_cpu.pc) != 0x4e73) // not rte
    {
      have_illegal = 0;
      m68ki_cpu.pc += 2;
#ifdef EMU_C68K
      PicoCpuCM68k.pc=PicoCpuCM68k.checkpc(PicoCpuCM68k.pc + 2);
#endif
    }
    // hacks for test_misc2
    if (m68ki_cpu.pc == 0x0002e0 && m68k_read_disassembler_16(m68ki_cpu.pc) == 0x4e73)
    {
      // get out of "priviledge violation" loop
      have_illegal = 1;
      //m68ki_cpu.s_flag = SFLAG_SET;
      //PicoCpuCM68k.srh|=0x20;
    }

    pppc = SekPc;
    ppop = m68k_read_disassembler_16(pppc);
    memcpy(old_regs, &other_dar(0), 4*16);
    old_sr = other_get_sr();

#if 0
    {
      char buff[128];
      dprintf("---");
      m68k_disassemble(buff, pppc, M68K_CPU_TYPE_68000);
      dprintf("PC: %06x: %04x: %s", pppc, ppop, buff);
      //dprintf("A7: %08x", PicoCpuCM68k.a[7]);
    }
#endif

    if (dbg_irq_level)
    {
      other_set_irq(dbg_irq_level);
      m68k_set_irq(dbg_irq_level);
      dbg_irq_level=0;
    }

    cyc_other=otherRun();
    cyc_musashi=m68k_execute(1);

    if (cyc_other != cyc_musashi) {
      dprintf("cycles: %i vs %i", cyc_other, cyc_musashi);
      err=1;
    }

    if (lrp_cyc != lrp_mus) {
      dprintf("lrp: %i vs %i", lrp_cyc&15, lrp_mus&15);
      err=1;
    }

    if (lwp_cyc != lwp_mus) {
      dprintf("lwp: %i vs %i", lwp_cyc&15, lwp_mus&15);
      err=1;
    }

    for (i=0; i < 16; i++) {
      if (lastwrite_cyc_d[i] != lastwrite_mus_d[i]) {
        dprintf("lastwrite: [%i]= %08x vs %08x", i, lastwrite_cyc_d[i], lastwrite_mus_d[i]);
        err=1;
        break;
      }
    }

    // compare PC
    m68ki_cpu.pc&=~1;
    if (SekPc != m68ki_cpu.pc) {
      dprintf("PC: %06x vs %06x", SekPc, m68ki_cpu.pc);
      err=1;
    }

#if 0
    if( SekPc > Pico.romsize || SekPc < 0x200 ) {
      dprintf("PC out of bounds: %06x", SekPc);
      err=1;
    }
#endif

    // compare regs
    for (i=0; i < 16; i++) {
      if (other_dar(i) != m68ki_cpu.dar[i]) {
        str = (i < 8) ? "d" : "a";
        dprintf("reg: %s%i: %08x vs %08x", str, i&7, other_dar(i), m68ki_cpu.dar[i]);
        err=1;
      }
    }

    // SR
    if (other_get_sr() != (mu_sr = m68k_get_reg(NULL, M68K_REG_SR))) {
      dprintf("SR: %04x vs %04x (??s? 0iii 000x nzvc)", other_get_sr(), mu_sr);
      err=1;
    }

    // IRQl
    if (other_get_irq() != (m68ki_cpu.int_level>>8)) {
      dprintf("IRQ: %i vs %i", other_get_irq(), (m68ki_cpu.int_level>>8));
      err=1;
    }

    // OSP/USP
    if (other_osp != m68ki_cpu.sp[((mu_sr>>11)&4)^4]) {
      dprintf("OSP: %06x vs %06x", other_osp, m68ki_cpu.sp[((mu_sr>>11)&4)^4]);
      err=1;
    }

    // stopped
    if ((other_is_stopped() && !m68ki_cpu.stopped) || (!other_is_stopped() && m68ki_cpu.stopped)) {
      dprintf("stopped: %i vs %i", other_is_stopped(), m68ki_cpu.stopped);
      err=1;
    }

    // tracing
    if((other_is_tracing() && !m68ki_tracing) || (!other_is_tracing() && m68ki_tracing)) {
      dprintf("tracing: %i vs %i", other_is_tracing(), m68ki_tracing);
      err=1;
    }

    if(err) dumpPCandExit();

#if 0
    if (PicoCpuCM68k.a[7] < 0x00ff0000 || PicoCpuCM68k.a[7] >= 0x01000000)
    {
      PicoCpuCM68k.a[7] = m68ki_cpu.dar[15] = 0xff8000;
    }
#endif
#if 0
    m68k_set_reg(M68K_REG_SR, ((mu_sr-1)&~0x2000)|(mu_sr&0x2000)); // broken
    CycloneSetSr(&PicoCpuCM68k, ((mu_sr-1)&~0x2000)|(mu_sr&0x2000));
    PicoCpuCM68k.stopped = m68ki_cpu.stopped = 0;
    if(SekPc > 0x400 && (PicoCpuCM68k.a[7] < 0xff0000 || PicoCpuCM68k.a[7] > 0xffffff))
    PicoCpuCM68k.a[7] = m68ki_cpu.dar[15] = 0xff8000;
#endif

    cyc_done += cyc_other;
    ops++;
  }

  return cyc_done;
}

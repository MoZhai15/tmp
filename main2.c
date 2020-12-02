/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * * * * * * * * * * * * Copyright (C) 2018 Institute of Computing
 * Technology, CAS Author : Han Shukai (email :
 * hanshukai@ict.ac.cn)
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * * * * * * * * * * * * The kernel's entry, where most of the
 * initialization work is done.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * * * * * * * * * * * *
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * * * * * * * * * * * */

#include <common.h>
#include <fdt.h>
#include <os/elf.h>
#include <os/futex.h>
#include <os/ioremap.h>
#include <os/irq.h>
#include <os/mm.h>
#include <os/sched.h>
#include <os/stdio.h>
#include <os/syscall.h>
#include <os/time.h>
#include <screen.h>
#include <assert.h>

#include <sbi.h>

#include <plic.h>
#include <emacps/xemacps_example.h>
#include <net.h>

static void init_pcb()
{
    for (int i = 0; i < NUM_MAX_TASK; ++i) {
        pcb[i].status = TASK_EXITED;
        pcb[i].needFree = 0;
    }

    for (int i = 0; i < NR_CPUS; ++i) {
        current_running[i] = &kernel_pcb[i];
        kernel_pcb[i].status = TASK_RUNNING;
        kernel_pcb[i].pgdir = pa2kva(PGDIR_PA);
        kernel_pcb[i].cursor_x = 1;
        kernel_pcb[i].cursor_y = 1;
        kernel_pcb[i].needFree = 0;
    }

    char* argv[1] = {"shell"};
    do_exec("shell", 1, argv, AUTO_CLEANUP_ON_EXIT);
}

static void init_syscall(void)
{
    // init system call table.
    syscall[SYSCALL_SPAWN]     = (long (*)())do_spawn;
    syscall[SYSCALL_EXIT]      = (long (*)())do_exit;
    syscall[SYSCALL_SLEEP]     = (long (*)())do_sleep;
    syscall[SYSCALL_KILL]      = (long (*)())do_kill;
    syscall[SYSCALL_WAITPID]   = (long (*)())do_waitpid;
    syscall[SYSCALL_PS]        = (long (*)())do_process_show;
    syscall[SYSCALL_GETPID]    = (long (*)())do_getpid;
    syscall[SYSCALL_EXEC]      = (long (*)())do_exec;
    syscall[SYSCALL_SHOW_EXEC] = (long (*)())do_show_exec;

    syscall[SYSCALL_FUTEX_WAIT]   = (long (*)())futex_wait;
    syscall[SYSCALL_FUTEX_WAKEUP] = (long (*)())futex_wakeup;
    syscall[SYSCALL_SHMPGET] = (long (*)()) shm_page_get;
    syscall[SYSCALL_SHMPDT] = (long (*)()) shm_page_dt;

    syscall[SYSCALL_CURSOR]       = (long (*)())screen_move_cursor;
    syscall[SYSCALL_REFLUSH]      = (long (*)())screen_reflush;
    syscall[SYSCALL_WRITE]        = (long (*)())screen_write;
    syscall[SYSCALL_SCREEN_CLEAR] = (long (*)())screen_clear;

    syscall[SYSCALL_GET_TIMEBASE] = (long (*)())get_time_base;
    syscall[SYSCALL_GET_TICK]     = (long (*)())get_ticks;
    syscall[SYSCALL_GET_CHAR]     = (long (*)())sbi_console_getchar;

    syscall[SYSCALL_NET_RECV] = (long(*)())do_net_recv;
    syscall[SYSCALL_NET_SEND] = (long(*)())do_net_send;
    syscall[SYSCALL_NET_IRQ_MODE] = (long(*)())do_net_irq_mode;
}

void boot_first_core(uint64_t mhartid, uintptr_t _dtb)
{
    // init Process Control Block (-_-!)
    init_pcb();
    printk("> [INIT] PCB initialization succeeded.\n\r");

    // setup timebase
    // fdt_print(_dtb);
    // get_prop_u32(_dtb, "/cpus/cpu/timebase-frequency", &time_base);
    time_base = sbi_read_fdt(TIMEBASE);
    uint32_t slcr_bade_addr = 0, ethernet_addr = 0;

    // get_prop_u32(_dtb, "/soc/slcr/reg", &slcr_bade_addr);
    slcr_bade_addr = sbi_read_fdt(SLCR_BADE_ADDR);
    printk("[slcr] phy: 0x%x\n\r", slcr_bade_addr);

    // get_prop_u32(_dtb, "/soc/ethernet/reg", &ethernet_addr);
    ethernet_addr = sbi_read_fdt(ETHERNET_ADDR);
    printk("[ethernet] phy: 0x%x\n\r", ethernet_addr);

    uint32_t plic_addr = 0;
    // get_prop_u32(_dtb, "/soc/interrupt-controller/reg", &plic_addr);
    plic_addr = sbi_read_fdt(PLIC_ADDR);
    printk("[plic] plic: 0x%x\n\r", plic_addr);

    uint32_t nr_irqs = sbi_read_fdt(NR_IRQS);
    // get_prop_u32(_dtb, "/soc/interrupt-controller/riscv,ndev", &nr_irqs);
    printk("[plic] nr_irqs: 0x%x\n\r", nr_irqs);

    // unmap temporay 2MB page
    // PTE* early_pgdir = (PTE*)pa2kva(PGDIR_PA);
    // early_pgdir[1]   = 0;

    XPS_SYS_CTRL_BASEADDR =
        (uintptr_t)ioremap((uint64_t)slcr_bade_addr, NORMAL_PAGE_SIZE);
    xemacps_config.BaseAddress =
        (uintptr_t)ioremap((uint64_t)ethernet_addr, 9 * NORMAL_PAGE_SIZE);
	xemacps_config.BaseAddress += 0x8000;
		
    uintptr_t _plic_addr =
        (uintptr_t)ioremap((uint64_t)plic_addr, 0x4000*NORMAL_PAGE_SIZE);
    // XPS_SYS_CTRL_BASEADDR = slcr_bade_addr;
    // xemacps_config.BaseAddress = ethernet_addr;
    xemacps_config.DeviceId        = 0;
    xemacps_config.IsCacheCoherent = 0;


    printk(
        "[slcr_bade_addr] phy:%x virt:%lx\n\r", slcr_bade_addr,
        XPS_SYS_CTRL_BASEADDR);
    printk(
        "[ethernet_addr] phy:%x virt:%lx\n\r", ethernet_addr,
        xemacps_config.BaseAddress);
    printk("[plic_addr] phy:%x virt:%lx\n\r", plic_addr, _plic_addr);
    plic_init(_plic_addr, nr_irqs);
    
    long status = EmacPsInit(&EmacPsInstance);
    if (status != XST_SUCCESS) {
        printk("Error: initialize ethernet driver failed!\n\r");
        assert(0);
    }

    // init futex mechanism
    init_system_futex();

    // init interrupt (^_^)
    init_exception();
    printk(
        "> [INIT] Interrupt processing initialization "
        "succeeded.\n\r");

    // init system call table (0_0)
    init_syscall();
    printk("> [INIT] System call initialized successfully.\n\r");

    // enable_interrupt();
    net_poll_mode = 1;
    // xemacps_example_main();

    // printk("Done\n\r");
    // for (;;);

    // fdt_print(riscv_dtb);

    /*printk("> ");
    while (1) {
        int ch = sbi_console_getchar();
    if (ch == -1) {
        continue;
    }
        if (ch == '\n') printk("\n> ");
    printk("%c", ch);
    }*/

    // init screen (QAQ)
    init_screen();
    printk("> [INIT] SCREEN initialization succeeded.\n\r");
    // screen_clear(0, SCREEN_HEIGHT - 1);
}

// jump from bootloader.
// The beginning of everything >_< ~~~~~~~~~~~~~~
int main(unsigned long mhartid, uintptr_t _dtb)
{
    if (mhartid == 0) {
        boot_first_core(mhartid, _dtb);
        printk("Core %d start up\n\r", mhartid);
        smp_init();
        lock_kernel();
        wakeup_other_hart();
        clear_sip();
        wait_other_hart_boot();
        // unmap temporay 2MB page
        PTE* early_pgdir = (PTE*) pa2kva(PGDIR_PA);
        early_pgdir[1] = 0;
        // flush_tlb_all();
        // unlock_kernel();
        // while(1);
    } else {
        wait_other_hart_boot();
        lock_kernel();
        printk("Core %d start up\n\r", mhartid);
        setup_exception();
        // unlock_kernel();
        // while(1);
    }
    // TODO Enable interrupt
    reset_irq_timer();

    unlock_kernel();
    while (1) {
        // (QAQQQQQQQQQQQ)
        // If you do non-preemptive scheduling, you need to use it
        // to surrender control do_scheduler();
        enable_interrupt();
        __asm__ __volatile__("wfi\n\r" :::);
        // do_scheduler();
    };
    return 0;
}

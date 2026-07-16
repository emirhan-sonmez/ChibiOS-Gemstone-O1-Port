/*
    ChibiOS - Copyright (C) 2006-2026 Giovanni Di Sirio.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/*
 * T3 Gemstone O1 (TI AM67A/J722S) Cortex-R5F memory setup.
 *
 * The image is loaded by the Linux k3-r5 remoteproc driver, addresses are
 * device addresses as seen by the R5F core and must match the reserved
 * memory carveouts in the host device tree.
 */

__und_stack_size__ = DEFINED(__und_stack_size__) ? __und_stack_size__ : 0x100;
__abt_stack_size__ = DEFINED(__abt_stack_size__) ? __abt_stack_size__ : 0x100;
__fiq_stack_size__ = DEFINED(__fiq_stack_size__) ? __fiq_stack_size__ : 0x100;
__irq_stack_size__ = DEFINED(__irq_stack_size__) ? __irq_stack_size__ : 0x400;
__svc_stack_size__ = DEFINED(__svc_stack_size__) ? __svc_stack_size__ : 0x100;
__sys_stack_size__ = DEFINED(__sys_stack_size__) ? __sys_stack_size__ : 0x800;

MEMORY
{
    atcm      (rwx) : org = 0x00000000, len = 32k   /* vectors             */
    btcm      (rwx) : org = 0x41010000, len = 32k   /* banked mode stacks  */
    ddr_rsc   (rw)  : org = 0xA2100000, len = 4k    /* resource table      */
    ddr_trace (rw)  : org = 0xA2110000, len = 16k   /* remoteproc trace    */
    ddr       (rwx) : org = 0xA2240000, len = 14080k /* code, data, heap   */

    /* Unused regions referenced by the generic memory rules.*/
    flash0 (rx) : org = 0x00000000, len = 0
    flash1 (rx) : org = 0x00000000, len = 0
    flash2 (rx) : org = 0x00000000, len = 0
    flash3 (rx) : org = 0x00000000, len = 0
    flash4 (rx) : org = 0x00000000, len = 0
    flash5 (rx) : org = 0x00000000, len = 0
    flash6 (rx) : org = 0x00000000, len = 0
    flash7 (rx) : org = 0x00000000, len = 0
    ram0   (wx) : org = 0x00000000, len = 0
    ram1   (wx) : org = 0x00000000, len = 0
    ram2   (wx) : org = 0x00000000, len = 0
    ram3   (wx) : org = 0x00000000, len = 0
    ram4   (wx) : org = 0x00000000, len = 0
    ram5   (wx) : org = 0x00000000, len = 0
    ram6   (wx) : org = 0x00000000, len = 0
    ram7   (wx) : org = 0x00000000, len = 0
}

/* For each data/text section two region are defined, a virtual region
   and a load region (_LMA suffix).*/

/* Region to be used for exception vectors.*/
REGION_ALIAS("VECTORS_FLASH", atcm);
REGION_ALIAS("VECTORS_FLASH_LMA", atcm);

/* Region to be used for constructors and destructors.*/
REGION_ALIAS("XTORS_FLASH", ddr);
REGION_ALIAS("XTORS_FLASH_LMA", ddr);

/* Region to be used for code text.*/
REGION_ALIAS("TEXT_FLASH", ddr);
REGION_ALIAS("TEXT_FLASH_LMA", ddr);

/* Region to be used for read only data.*/
REGION_ALIAS("RODATA_FLASH", ddr);
REGION_ALIAS("RODATA_FLASH_LMA", ddr);

/* Region to be used for various.*/
REGION_ALIAS("VARIOUS_FLASH", ddr);
REGION_ALIAS("VARIOUS_FLASH_LMA", ddr);

/* Region to be used for RAM(n) initialization data.*/
REGION_ALIAS("RAM_INIT_FLASH_LMA", ddr);

/* RAM region to be used for the banked mode stacks.*/
REGION_ALIAS("STACKS_RAM", btcm);
REGION_ALIAS("C1_STACKS_RAM", btcm);

/* RAM region to be used for data segment.*/
REGION_ALIAS("DATA_RAM", ddr);
REGION_ALIAS("DATA_RAM_LMA", ddr);

/* RAM region to be used for BSS segment.*/
REGION_ALIAS("BSS_RAM", ddr);

/* RAM region to be used for the default heap.*/
REGION_ALIAS("HEAP_RAM", ddr);

/* RemoteProc sections at fixed device addresses.*/
SECTIONS
{
    .resource_table : ALIGN(4)
    {
        KEEP(*(.resource_table))
    } > ddr_rsc

    .trace (NOLOAD) : ALIGN(4)
    {
        KEEP(*(.trace))
    } > ddr_trace

    /* Bring-up boot probe, runs from BTCM before any DDR execution, the
       banked mode stacks are allocated after it by rules_stacks.ld.*/
    .tcm_probe : ALIGN(4)
    {
        KEEP(*(.tcm_probe))
    } > btcm
}

/* Stack rules inclusion.*/
INCLUDE rules_stacks.ld
INCLUDE rules_stacks_c1.ld

/* Code rules inclusion.*/
INCLUDE rules_code.ld

/* Data rules inclusion.*/
INCLUDE rules_data.ld

/* Memory rules inclusion.*/
INCLUDE rules_memory.ld

// Code to setup clocks and gpio on stm32f1
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
// https://github.com/KevinOConnor/klipper
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#if defined(STM32F1xx)
#include "Arduino.h"
#include "internal.h"
#include "gpio.h"
#include "targets.h"
#include "irq.h"

// Copied from STM32F1xx CMSIS header file since platform.io includes wrong file!
#define FLASH_ACR_LATENCY_Pos (0U)
#define RCC_CFGR_PLLSRC_Pos   (16U)
#define RCC_CFGR_PLLMULL_Pos  (18U)
#define RCC_CFGR_SWS_Pos      (2U)
#define RCC_CFGR_SWS_Msk      (0x3UL << RCC_CFGR_SWS_Pos) /*!< 0x0000000C */

#define CONFIG_CLOCK_FREQ 72000000 // 72MHz
#ifdef HSE_VALUE
#define CONFIG_CLOCK_REF_FREQ HSE_VALUE
#else
#define CONFIG_CLOCK_REF_FREQ 12000000U // 12MHz
#endif
#define CONFIG_STM32_CLOCK_REF_INTERNAL 0
#define FREQ_PERIPH                     (CONFIG_CLOCK_FREQ / 2)

// Enable a peripheral clock
void enable_pclock(uint32_t periph_base)
{
    if (periph_base < APB2PERIPH_BASE)
    {
        uint32_t pos = (periph_base - APB1PERIPH_BASE) / 0x400;
        RCC->APB1ENR |= (1 << pos);
        RCC->APB1ENR;
    }
    else if (periph_base < AHBPERIPH_BASE)
    {
        uint32_t pos = (periph_base - APB2PERIPH_BASE) / 0x400;
        RCC->APB2ENR |= (1 << pos);
        RCC->APB2ENR;
    }
    else
    {
        uint32_t pos = (periph_base - AHBPERIPH_BASE) / 0x400;
        RCC->AHBENR |= (1 << pos);
        RCC->AHBENR;
    }
}

// Check if a peripheral clock has been enabled
int is_enabled_pclock(uint32_t periph_base)
{
    if (periph_base < APB2PERIPH_BASE)
    {
        uint32_t pos = (periph_base - APB1PERIPH_BASE) / 0x400;
        return RCC->APB1ENR & (1 << pos);
    }
    else if (periph_base < AHBPERIPH_BASE)
    {
        uint32_t pos = (periph_base - APB2PERIPH_BASE) / 0x400;
        return RCC->APB2ENR & (1 << pos);
    }
    else
    {
        uint32_t pos = (periph_base - AHBPERIPH_BASE) / 0x400;
        return RCC->AHBENR & (1 << pos);
    }
}

// Return the frequency of the given peripheral clock
uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    return FREQ_PERIPH;
}

// Enable a GPIO peripheral clock
void gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t rcc_pos = ((uint32_t)regs - APB2PERIPH_BASE) / 0x400;
    RCC->APB2ENR |= 1 << rcc_pos;
    RCC->APB2ENR;
}

// Set the mode and extended function of a pin
void gpio_peripheral(uint32_t gpio, uint32_t mode, int pullup)
{
    GPIO_TypeDef *regs = digital_regs[GPIO2PORT(gpio)];

    // Enable GPIO clock
    gpio_clock_enable(regs);

    // Configure GPIO
    uint32_t pos = gpio % 16, shift = (pos % 8) * 4, msk = 0xf << shift, cfg;
    if (mode == GPIO_INPUT)
    {
        cfg = pullup ? 0x8 : 0x4;
    }
    else if (mode == GPIO_OUTPUT)
    {
        cfg = 0x1;
    }
    else if (mode == (GPIO_OUTPUT | GPIO_OPEN_DRAIN))
    {
        cfg = 0x5;
    }
    else if (mode == GPIO_ANALOG)
    {
        cfg = 0x0;
    }
    else
    {
        if (mode & GPIO_OPEN_DRAIN)
            // Alternate function with open-drain mode
            cfg = 0xd;
        else if (pullup > 0)
            // Alternate function input pins use GPIO_INPUT mode on the stm32f1
            cfg = 0x8;
        else
            cfg = 0x9;
    }
    if (pos & 0x8)
        regs->CRH = (regs->CRH & ~msk) | (cfg << shift);
    else
        regs->CRL = (regs->CRL & ~msk) | (cfg << shift);

    if (pullup > 0)
        regs->BSRR = 1 << pos;
    else if (pullup < 0)
        regs->BSRR = 1 << (pos + 16);

    if (gpio == GPIO('A', 13) || gpio == GPIO('A', 14))
        // Disable SWD to free PA13, PA14
        AFIO->MAPR = AFIO_MAPR_SWJ_CFG_DISABLE;
}

// Handle USB reboot requests
void usb_request_bootloader(void)
{
    if (!VECT_TAB_OFFSET)
        return;
    // Enter "stm32duino" bootloader
    irq_disable();
    RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;
    BKP->DR10 = 0x01;
    PWR->CR &= ~PWR_CR_DBP;
    NVIC_SystemReset();
}

// Main clock setup called at chip startup
static void
clock_setup(void)
{
    // Configure and enable PLL
    uint32_t cfgr;
    if (!CONFIG_STM32_CLOCK_REF_INTERNAL)
    {
        // Configure 72Mhz PLL from external crystal (HSE)
        uint32_t div = CONFIG_CLOCK_FREQ / CONFIG_CLOCK_REF_FREQ;
        RCC->CR |= RCC_CR_HSEON;
        cfgr = (1 << RCC_CFGR_PLLSRC_Pos) | ((div - 2) << RCC_CFGR_PLLMULL_Pos);
    }
    else
    {
        // Configure 72Mhz PLL from internal 8Mhz oscillator (HSI)
        uint32_t div2 = (CONFIG_CLOCK_FREQ / 8000000) * 2;
        cfgr = ((0 << RCC_CFGR_PLLSRC_Pos) | ((div2 - 2) << RCC_CFGR_PLLMULL_Pos));
    }
    RCC->CFGR = (cfgr | RCC_CFGR_PPRE1_DIV2 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_ADCPRE_DIV4);
    RCC->CR |= RCC_CR_PLLON;

    // Set flash latency
    FLASH->ACR = (2 << FLASH_ACR_LATENCY_Pos) | FLASH_ACR_PRFTBE;

    // Wait for PLL lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;

    // Switch system clock to PLL
    RCC->CFGR = cfgr | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL)
        ;
}

static __IO uint64_t us_counter = 0;

// IRQ handler
void SysTick_Handler(void)
{
    us_counter++;
    //irq_disable();
    //uint32_t diff = timer_dispatch_many();
    //timer_set_diff(diff);
    //irq_enable();
}

// Return the number of clock ticks for a given number of microseconds
uint32_t timer_from_us(uint32_t us)
{
    return us * (CONFIG_CLOCK_FREQ / 1000000);
}

uint32_t us_from_timer(uint32_t tmr)
{
    return tmr / (CONFIG_CLOCK_FREQ / 1000000);
}

// Return true if time1 is before time2.  Always use this function to
// compare times as regular C comparisons can fail if the counter
// rolls over.
uint8_t timer_is_before(uint32_t time1, uint32_t time2)
{
    return (int32_t)(time1 - time2) < 0;
}

// Set the next irq time
void timer_set_diff(uint32_t value)
{
    SysTick->LOAD = value;
    SysTick->VAL = 0;
    SysTick->LOAD = 0;
}

// Return the current time (in absolute clock ticks).
uint32_t timer_read_time(void)
{
    return DWT->CYCCNT;
}

void timer_kick(void)
{
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTSET_Msk;
}

static void timer_init(void)
{
    // Enable Debug Watchpoint and Trace (DWT) for its 32bit timer
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    DWT->CYCCNT = 0;

    // Enable SysTick
    NVIC_SetPriority(SysTick_IRQn, 2);
    SysTick->LOAD = timer_from_us(1); //(uint32_t)(SystemCoreClock / (1000UL - 1UL));
    SysTick->VAL = 0UL;
    SysTick->CTRL = (SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk);
    //timer_kick();
}

uint32_t micros(void)
{
    return (uint32_t)us_counter;
}

uint32_t millis(void)
{
    return us_counter / 1000;
}

void delay(uint32_t ms)
{
    ms += millis();
    while (millis() < ms)
        ;
}

void delayMicroseconds(uint32_t usecs)
{
    if (!(CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk))
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }

    uint32_t end = timer_read_time() + timer_from_us(usecs);
    while (timer_is_before(timer_read_time(), end))
        ;
}

// Main entry point - called from armcm_boot.c:ResetHandler()
int main(void)
{
    extern void setup();
    extern void loop();

    setup();
    for (;;)
        loop();
}

// Force init to be called *first*, i.e. before static object allocation.
// Otherwise, statically allocated objects that need HAL may fail.
__attribute__((constructor(101))) void premain()
{

  // Required by FreeRTOS, see http://www.freertos.org/RTOS-Cortex-M3-M4.html
#ifdef NVIC_PRIORITYGROUP_4
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
#endif
#if (__CORTEX_M == 0x07U)
  // Defined in CMSIS core_cm7.h
#ifndef I_CACHE_DISABLED
  SCB_EnableICache();
#endif
#ifndef D_CACHE_DISABLED
  SCB_EnableDCache();
#endif
#endif

  //init();
    // Run SystemInit() and then restore VTOR
    SystemInit();
    //SCB->VTOR = (uint32_t)VectorTable;
    //SCB->VTOR = FLASH_BASE | VECT_TAB_OFFSET;
    SystemCoreClockUpdate();
    //NVIC_SetPriorityGrouping(0x00000003U);

    // Setup clocks
    clock_setup();

    // Disable JTAG to free PA15, PB3, PB4
    enable_pclock(AFIO_BASE);
    AFIO->MAPR = AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

    timer_init();
}



extern uint32_t _data_start, _data_end, _data_flash;
extern uint32_t _bss_start, _bss_end, _stack_start;

// Initial code entry point - invoked by the processor after a reset
void ResetHandler(void)
{
    // Copy global variables from flash to ram
    //uint32_t count = (&_data_end - &_data_start) * 4;
    //__builtin_memcpy(&_data_start, &_data_flash, count);

    // Clear the bss segment
    //__builtin_memset(&_bss_start, 0, (&_bss_end - &_bss_start) * 4);

    //barrier();

    // Initializing the C library isn't needed...
    //__libc_init_array();

    // Run the main board specific code
    main();

    // The armcm_main() call should not return
    for (;;)
        ;
}

// Code called for any undefined interrupts
void DefaultHandler(void)
{
    for (;;)
        ;
}

#endif

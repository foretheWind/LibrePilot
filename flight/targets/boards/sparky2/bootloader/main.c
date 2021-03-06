/**
 ***********************************************************************************
 * @addtogroup Sparky2BL Sparky2 BootLoader
 * @brief These files contain the code to the Sparky2 Bootloader.
 *
 * @{
 * @file       main.c
 * @author     The LibrePilot Project, http://www.librepilot.org Copyright (C) 2016.
 *             The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      This is the file with the main function of the Sparky2 BootLoader
 * @see        The GNU Public License (GPL) Version 3
 *
 **********************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <pios.h>
#include <pios_board_info.h>
#include <op_dfu.h>
#include <pios_iap.h>
#include <fifo_buffer.h>
#include <pios_com_msg.h>
#include <pios_usbhook.h> /* PIOS_USBHOOK_* */
#include <stdbool.h>
#include <pios_board_init.h>

extern void FLASH_Download();
void check_bor();
#define BSL_HOLD_STATE ((PIOS_USB_DETECT_GPIO_PORT->IDR & PIOS_USB_DETECT_GPIO_PIN) ? 0 : 1)

/* Private typedef -----------------------------------------------------------*/
typedef void (*pFunction)(void);
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
pFunction Jump_To_Application;
uint32_t JumpAddress;

/// LEDs PWM
uint32_t period1 = 5000; // 5 mS
uint32_t sweep_steps1 = 100; // * 5 mS -> 500 mS
uint32_t period2 = 5000; // 5 mS
uint32_t sweep_steps2 = 100; // * 5 mS -> 500 mS


////////////////////////////////////////
uint8_t tempcount = 0;

/* Extern variables ----------------------------------------------------------*/
DFUStates DeviceState;
int16_t status        = 0;
bool JumpToApp        = false;
bool GO_dfu           = false;
bool USB_connected    = false;
bool User_DFU_request = false;
static uint8_t mReceive_Buffer[63];
/* Private function prototypes -----------------------------------------------*/
uint32_t LedPWM(uint32_t pwm_period, uint32_t pwm_sweep_steps, uint32_t count);
uint8_t processRX();
void jump_to_app();

int main()
{
    PIOS_SYS_Init();
    PIOS_Board_Init();
    PIOS_IAP_Init();

    // Make sure the brown out reset value for this chip
    // is 2.7 volts
    check_bor();

    USB_connected = PIOS_USB_CheckAvailable(0);

    if (PIOS_IAP_CheckRequest() == true) {
        PIOS_DELAY_WaitmS(1000);
        User_DFU_request = true;
        PIOS_IAP_ClearRequest();
    }

    GO_dfu = (USB_connected == true) || (User_DFU_request == true);

    if (GO_dfu == true) {
        if (User_DFU_request == true) {
            DeviceState = DFUidle;
        } else {
            DeviceState = BLidle;
        }
    } else {
        JumpToApp = true;
    }

    uint32_t stopwatch  = 0;
    uint32_t prev_ticks = PIOS_DELAY_GetuS();
    while (true) {
        /* Update the stopwatch */
        uint32_t elapsed_ticks = PIOS_DELAY_GetuSSince(prev_ticks);
        prev_ticks += elapsed_ticks;
        stopwatch  += elapsed_ticks;

        if (JumpToApp == true) {
            jump_to_app();
        }

        switch (DeviceState) {
        case Last_operation_Success:
        case uploadingStarting:
        case DFUidle:
            period1 = 5000;
            sweep_steps1 = 100;
            PIOS_LED_Off(PIOS_LED_HEARTBEAT);
            period2 = 0;
            break;
        case uploading:
            period1 = 5000;
            sweep_steps1 = 100;
            period2 = 2500;
            sweep_steps2 = 50;
            break;
        case downloading:
            period1 = 2500;
            sweep_steps1 = 50;
            PIOS_LED_Off(PIOS_LED_HEARTBEAT);
            period2 = 0;
            break;
        case BLidle:
            period1 = 0;
            PIOS_LED_On(PIOS_LED_HEARTBEAT);
            period2 = 0;
            break;
        default: // error
            period1 = 5000;
            sweep_steps1 = 100;
            period2 = 5000;
            sweep_steps2 = 100;
        }

        if (period1 != 0) {
            if (LedPWM(period1, sweep_steps1, stopwatch)) {
                PIOS_LED_On(PIOS_LED_HEARTBEAT);
            } else {
                PIOS_LED_Off(PIOS_LED_HEARTBEAT);
            }
        } else {
            PIOS_LED_On(PIOS_LED_HEARTBEAT);
        }

        if (period2 != 0) {
            if (LedPWM(period2, sweep_steps2, stopwatch)) {
                PIOS_LED_On(PIOS_LED_HEARTBEAT);
            } else {
                PIOS_LED_Off(PIOS_LED_HEARTBEAT);
            }
        } else {
            PIOS_LED_Off(PIOS_LED_HEARTBEAT);
        }

        if (stopwatch > 50 * 1000 * 1000) {
            stopwatch = 0;
        }
        if ((stopwatch > 6 * 1000 * 1000) && ((DeviceState == BLidle) || (DeviceState == DFUidle && !USB_connected))) {
            JumpToApp = true;
        }

        processRX();
        DataDownload(start);
    }
}

void jump_to_app()
{
    const struct pios_board_info *bdinfo = &pios_board_info_blob;

    PIOS_LED_On(PIOS_LED_HEARTBEAT);
    // Look at cm3_vectors struct in startup. In a fw image the first uint32_t contains the address of the top of irqstack
    uint32_t fwIrqStackBase = (*(__IO uint32_t *)bdinfo->fw_base) & 0xFFFE0000;
    // Check for the two possible irqstack locations (sram or core coupled sram)
    if (fwIrqStackBase == 0x20000000 || fwIrqStackBase == 0x10000000) {
        /* Jump to user application */
        FLASH_Lock();
        RCC_APB2PeriphResetCmd(0xffffffff, ENABLE);
        RCC_APB1PeriphResetCmd(0xffffffff, ENABLE);
        RCC_APB2PeriphResetCmd(0xffffffff, DISABLE);
        RCC_APB1PeriphResetCmd(0xffffffff, DISABLE);

        PIOS_USBHOOK_Deactivate();

        JumpAddress = *(__IO uint32_t *)(bdinfo->fw_base + 4);
        Jump_To_Application = (pFunction)JumpAddress;
        /* Initialize user application's Stack Pointer */
        __set_MSP(*(__IO uint32_t *)bdinfo->fw_base);
        Jump_To_Application();
    } else {
        DeviceState = failed_jump;
        return;
    }
}
uint32_t LedPWM(uint32_t pwm_period, uint32_t pwm_sweep_steps, uint32_t count)
{
    uint32_t curr_step  = (count / pwm_period) % pwm_sweep_steps; /* 0 - pwm_sweep_steps */
    uint32_t pwm_duty   = pwm_period * curr_step / pwm_sweep_steps; /* fraction of pwm_period */

    uint32_t curr_sweep = (count / (pwm_period * pwm_sweep_steps)); /* ticks once per full sweep */

    if (curr_sweep & 1) {
        pwm_duty = pwm_period - pwm_duty; /* reverse direction in odd sweeps */
    }
    return ((count % pwm_period) > pwm_duty) ? 1 : 0;
}

uint8_t processRX()
{
    if (PIOS_COM_MSG_Receive(PIOS_COM_TELEM_USB, mReceive_Buffer, sizeof(mReceive_Buffer))) {
        processComand(mReceive_Buffer);
    }
    return true;
}

/**
 * Check the brown out reset threshold is 2.7 volts and if not
 * resets it.  This solves an issue that can prevent boards
 * powering up with some BEC
 */
void check_bor()
{
    uint8_t bor = FLASH_OB_GetBOR();

    if (bor != OB_BOR_LEVEL3) {
        FLASH_OB_Unlock();
        FLASH_OB_BORConfig(OB_BOR_LEVEL3);
        FLASH_OB_Launch();
        while (FLASH_WaitForLastOperation() == FLASH_BUSY) {
            ;
        }
        FLASH_OB_Lock();
        while (FLASH_WaitForLastOperation() == FLASH_BUSY) {
            ;
        }
    }
}

int32_t platform_senddata(const uint8_t *msg, uint16_t msg_len)
{
    return PIOS_COM_MSG_Send(PIOS_COM_TELEM_USB, msg, msg_len);
}

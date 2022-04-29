/*
* OutputGECEUart.cpp - GECE driver code for ESPixelStick UART
*
* Project: ESPixelStick - An ESP8266 / ESP32 and E1.31 based pixel driver
* Copyright (c) 2015, 2022 Shelby Merrick
* http://www.forkineye.com
*
*  This program is provided free for you to use in any way that you wish,
*  subject to the laws and regulations where you are using it.  Due diligence
*  is strongly suggested before using this code.  Please give credit where due.
*
*  The Author makes no warranty of any kind, express or implied, with regard
*  to this program or the documentation contained in this document.  The
*  Author shall not be liable in any event for incidental or consequential
*  damages in connection with, or arising out of, the furnishing, performance
*  or use of these programs.
*
*/

#include "../ESPixelStick.h"
#if defined(SUPPORT_OutputType_GECE) && defined(SUPPORT_UART_OUTPUT)

#include "OutputGECEUart.hpp"

/*
 * 7N1 UART lookup table for GECE, first bit is ignored.
 * Start bit and stop bits are part of the packet.
 * Bits are backwards since we need MSB out.
 */
struct Convert2BitIntensityToGECEUartDataStreamEntry_t
{
    uint8_t Translation;
    c_OutputUart::UartDataBitTranslationId_t Id;
};
const Convert2BitIntensityToGECEUartDataStreamEntry_t PROGMEM Convert2BitIntensityToGECEUartDataStream[] =
{
//     {0b11111110, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_00_ID},
//     {0b10000000, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_01_ID},

    {0b11101111, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_00_ID},
    {0b11101000, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_01_ID},
    {0b00001111, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_10_ID},
    {0b00001000, c_OutputUart::UartDataBitTranslationId_t::Uart_DATA_BIT_11_ID},
};

// Static arrays are initialized to zero at boot time
static c_OutputGECEUart *GECE_OutputChanArray[c_OutputMgr::e_OutputChannelIds::OutputChannelId_End];

#ifdef GECE_UART_DEBUG_COUNTERS
    static uint32_t TimerIsrCounter = 0;
#endif // def GECE_UART_DEBUG_COUNTERS

//----------------------------------------------------------------------------
/* shell function to set the 'this' pointer of the real ISR
   This allows me to use non static variables in the ISR.
 */
static void IRAM_ATTR timer_intr_handler()
{
#ifdef GECE_UART_DEBUG_COUNTERS
    TimerIsrCounter++;
#endif // def GECE_UART_DEBUG_COUNTERS

#ifdef ARDUINO_ARCH_ESP32
    portENTER_CRITICAL_ISR(&timerMux);
#endif
    // (*((volatile uint32_t*)(UART_FIFO_AHB_REG (0)))) = (uint32_t)('.');
    for (auto currentChannel : GECE_OutputChanArray)
    {
        if (nullptr != currentChannel)
        {
            currentChannel->ISR_Handler();
        }
    }
#ifdef ARDUINO_ARCH_ESP32
    portEXIT_CRITICAL_ISR(&timerMux);
#endif

} // timer_intr_handler

//----------------------------------------------------------------------------
c_OutputGECEUart::c_OutputGECEUart (c_OutputMgr::e_OutputChannelIds OutputChannelId,
                                    gpio_num_t outputGpio,
                                    uart_port_t uart,
                                    c_OutputMgr::e_OutputType outputType) :
                                    c_OutputGECE (OutputChannelId, outputGpio, uart, outputType)
{
    // DEBUG_START;

    GECE_OutputChanArray[OutputChannelId] = nullptr;

    // DEBUG_END;
} // c_OutputGECEUart

//----------------------------------------------------------------------------
c_OutputGECEUart::~c_OutputGECEUart ()
{
    // DEBUG_START;
    if (HasBeenInitialized)
    {
        GECE_OutputChanArray[OutputChannelId] = nullptr;

        // clean up the timer ISR
        bool foundActiveChannel = false;
        for (auto currentChannel : GECE_OutputChanArray)
        {
            // DEBUG_V (String ("currentChannel: ") + String (uint(currentChannel), HEX));
            if (nullptr != currentChannel)
            {
                // DEBUG_V ("foundActiveChannel");
                foundActiveChannel = true;
            }
        }

        // DEBUG_V ();

        // have all of the GECE channels been killed?
        if (!foundActiveChannel)
        {
            // DEBUG_V ("Detach Interrupts");
#ifdef ARDUINO_ARCH_ESP8266
            timer1_detachInterrupt();
#elif defined(ARDUINO_ARCH_ESP32)
            if (pHwTimer)
            {
                timerAlarmDisable(pHwTimer);
            }
#endif
        }
    }

    // DEBUG_END;
} // ~c_OutputGECEUart

//----------------------------------------------------------------------------
void c_OutputGECEUart::Begin ()
{
    // DEBUG_START;

    c_OutputGECE::Begin();

    for (auto CurrentTranslation : Convert2BitIntensityToGECEUartDataStream)
    {
        Uart.SetIntensity2Uart(CurrentTranslation.Translation, CurrentTranslation.Id);
    }
    
    // DEBUG_V(String("GECE_PIXEL_UART_BAUDRATE: ") + String(GECE_PIXEL_UART_BAUDRATE));

    SetIntensityBitTimeInUS(float(GECE_USEC_PER_GECE_BIT));

    c_OutputUart::OutputUartConfig_t OutputUartConfig;
    OutputUartConfig.ChannelId                      = OutputChannelId;
    OutputUartConfig.UartId                         = UartId;
    OutputUartConfig.DataPin                        = DataPin;
    OutputUartConfig.IntensityDataWidth             = GECE_PACKET_SIZE;
    OutputUartConfig.UartDataSize                   = c_OutputUart::UartDataSize_t::OUTPUT_UART_8N1;
    OutputUartConfig.TranslateIntensityData         = c_OutputUart::TranslateIntensityData_t::TwoToOne;
    OutputUartConfig.pPixelDataSource               = this;
    OutputUartConfig.Baudrate                       = GECE_BAUDRATE;
    OutputUartConfig.InvertOutputPolarity           = false;
    OutputUartConfig.SendBreakAfterIntensityData    = GECE_UART_BREAK_BITS; // number of bit times to delay
    OutputUartConfig.TriggerIsrExternally           = false;
    OutputUartConfig.SendExtendedStartBit           = uint32_t((float(GECE_PIXEL_START_TIME_NS / 1000.0) / float(GECE_UART_USEC_PER_BIT))+0.5);
    Uart.Begin(OutputUartConfig);

    // DEBUG_V (String ("       TIMER_FREQUENCY: ") + String (TIMER_FREQUENCY));
    // DEBUG_V (String ("     TIMER_ClockTimeNS: ") + String (TIMER_ClockTimeNS));
    // DEBUG_V (String ("                 F_CPU: ") + String (F_CPU));
    // DEBUG_V (String ("       CPU_ClockTimeNS: ") + String (CPU_ClockTimeNS));
    // DEBUG_V (String ("  GECE_FRAME_TIME_USEC: ") + String (GECE_FRAME_TIME_USEC));
    // DEBUG_V (String ("  GECE_FRAME_TIME_NSEC: ") + String (GECE_FRAME_TIME_NSEC));
    // DEBUG_V (String ("GECE_CCOUNT_FRAME_TIME: ") + String (GECE_CCOUNT_FRAME_TIME));

    HasBeenInitialized = true;

    // DEBUG_END;
} // init

//----------------------------------------------------------------------------
bool c_OutputGECEUart::SetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    // DEBUG_START;

    bool response = c_OutputGECE::SetConfig(jsonConfig);
    response |= Uart.SetConfig(jsonConfig);

    // DEBUG_END;
    return response;

} // SetConfig

//----------------------------------------------------------------------------
void c_OutputGECEUart::GetConfig(ArduinoJson::JsonObject &jsonConfig)
{
    // DEBUG_START;

    c_OutputGECE::GetConfig(jsonConfig);
    Uart.GetConfig(jsonConfig);

    // DEBUG_END;

} // GetConfig

//----------------------------------------------------------------------------
void c_OutputGECEUart::GetStatus(ArduinoJson::JsonObject &jsonStatus)
{
    // DEBUG_START;

    c_OutputGECE::GetStatus(jsonStatus);
    Uart.GetStatus(jsonStatus);

#ifdef GECE_UART_DEBUG_COUNTERS
    JsonObject debugStatus = jsonStatus.createNestedObject("GECE UART Debug");
    debugStatus["NewFrameCounter"]          = NewFrameCounter;
    debugStatus["TimeSinceLastFrameMS"]     = TimeSinceLastFrameMS;
    debugStatus["TimeLastFrameStartedMS"]   = TimeLastFrameStartedMS;
    debugStatus["IsrHandlerCount"]          = IsrHandlerCount;
    debugStatus["TimerIsrCounter"]          = TimerIsrCounter;
#endif // def GECE_UART_DEBUG_COUNTERS

    // DEBUG_END;

} // GetStatus

//----------------------------------------------------------------------------
void c_OutputGECEUart::Render ()
{
    // DEBUG_START;

    // DEBUG_V (String ("RemainingIntensityCount: ") + RemainingIntensityCount)

    // start processing the timer interrupts
    if (nullptr != pOutputBuffer)
    {
        GECE_OutputChanArray[OutputChannelId] = this;
    }

    do // Once
    {
        if (gpio_num_t(-1) == DataPin)
        {
            break;
        }

        if (!canRefresh())
        {
            break;
        }

        // DEBUG_V("get the next frame started");
#ifdef GECE_UART_DEBUG_COUNTERS
        NewFrameCounter++;
        TimeSinceLastFrameMS = millis() - TimeLastFrameStartedMS;
        TimeLastFrameStartedMS = millis();
#endif // def GECE_UART_DEBUG_COUNTERS

        ReportNewFrame();
        Uart.StartNewFrame();

        // DEBUG_V();

    } while (false);

    // DEBUG_END;

} // render

//----------------------------------------------------------------------------
void c_OutputGECEUart::PauseOutput (bool State)
{
    // DEBUG_START;

    c_OutputGECE::PauseOutput(State);
    Uart.PauseOutput(State);

    // DEBUG_END;
} // PauseOutput

#endif // defined(SUPPORT_OutputType_GECE) && defined(SUPPORT_UART_OUTPUT)

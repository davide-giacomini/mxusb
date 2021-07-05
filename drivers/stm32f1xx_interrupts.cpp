//#include "stm32_usb_regs.h"
//FIXME: include should be implementation independent
#include "drivers/stm32f1xx_peripheral.h"
#include "def_ctrl_pipe.h"
#include "usb_tracer.h"
#include "usb_impl.h"
//#include <config/usb_gpio.h>
//FIXME: include should be implementation independent
#include "drivers/stm32f1xx_gpio.h"
#include <config/usb_config.h>

#ifdef _MIOSIX
#include "interfaces/delays.h"
#include "kernel/kernel.h"
#include "interfaces-impl/portability_impl.h"
using namespace miosix;
#else //_MIOSIX
#include "libraries/system.h"
#endif //_MIOSIX

#include "drivers/stm32f1xx_memory.h"

//
// interrupt handler
//

namespace mxusb {

/**
 * \internal
 * Handles USB device RESET
 */
static void IRQhandleReset()
{
    Tracer::IRQtrace(Ut::DEVICE_RESET);

    USB->DADDR=0;  //Disable transaction handling
    USB->ISTR=0;   //When the device is reset, clear all pending interrupts
    //USB->BTABLE=SharedMemory::BTABLE_ADDR; //Set BTABLE

    USB->BTABLE=SharedMemoryImpl::BTABLE_ADDR; //Set BTABLE

    for(int i=1;i<NUM_ENDPOINTS;i++) EndpointImpl::get(i)->IRQdeconfigure(i);
    SharedMemory::instance().reset();
    DefCtrlPipe::IRQdefaultStatus();

    //After a reset device address is zero, enable transaction handling
    USB->DADDR=0 | USB_DADDR_EF;

    //Enable more interrupt sources now that reset happened
    USB->CNTR=USB_CNTR_CTRM | USB_CNTR_SUSPM | USB_CNTR_WKUPM | USB_CNTR_RESETM;

    //Device is now in the default address state
    DeviceStateImpl::IRQsetState(USBdevice::DEFAULT);
}

/**
 * \internal
 * Actual low priority interrupt handler.
 */
void USBirqLpHandler() __attribute__ ((noinline));
void USBirqLpHandler()
{
    unsigned short flags=USB->ISTR;
    Callbacks *callbacks=Callbacks::IRQgetCallbacks();
    if(flags & USB_ISTR_RESET)
    {
        IRQhandleReset();
        callbacks->IRQreset();
        return; //Reset causes all interrupt flags to be ignored
    }
    if(flags & USB_ISTR_SUSP)
    {
        USB->ISTR= ~USB_ISTR_SUSP; //Clear interrupt flag
        USB->CNTR|=USB_CNTR_FSUSP;
        USB->CNTR|=USB_CNTR_LP_MODE;
        Tracer::IRQtrace(Ut::SUSPEND_REQUEST);
        DeviceStateImpl::IRQsetSuspended(true);
        //If device is configured, deconfigure all endpoints. This in turn will
        //wake the threads waiting to write/read on endpoints
        if(USBdevice::IRQgetState()==USBdevice::CONFIGURED)
            EndpointImpl::IRQdeconfigureAll();
        callbacks->IRQsuspend();
    }
    if(flags & USB_ISTR_WKUP)
    {
        USB->ISTR= ~USB_ISTR_WKUP; //Clear interrupt flag
        USB->CNTR&= ~USB_CNTR_FSUSP;
        Tracer::IRQtrace(Ut::RESUME_REQUEST);
        DeviceStateImpl::IRQsetSuspended(false);
        callbacks->IRQresume();
        //Reconfigure all previously deconfigured endpoints
        unsigned char conf=USBdevice::IRQgetConfiguration();
        if(conf!=0)
            EndpointImpl::IRQconfigureAll(DefCtrlPipe::IRQgetConfigDesc(conf));
    }
    while(flags & USB_ISTR_CTR)
    {
        int epNum=flags & USB_ISTR_EP_ID;
        unsigned short reg=USB->endpoint[epNum].get();
        if(epNum==0)
        {
            DefCtrlPipe::IRQstatusNak();
            //Transaction on endpoint zero
            if(reg & USB_EP0R_CTR_RX)
            {
                bool isSetupPacket=reg & USB_EP0R_SETUP;
                USB->endpoint[epNum].IRQclearRxInterruptFlag();
                if(isSetupPacket) DefCtrlPipe::IRQsetup();
                else DefCtrlPipe::IRQout();
            }

            if(reg & USB_EP0R_CTR_TX)
            {
                USB->endpoint[epNum].IRQclearTxInterruptFlag();
                DefCtrlPipe::IRQin();
            }
            DefCtrlPipe::IRQrestoreStatus();

        } else {
            //Transaction on other endpoints
            EndpointImpl *epi=EndpointImpl::IRQget(epNum);
            if(reg & USB_EP0R_CTR_RX)
            {
                USB->endpoint[epNum].IRQclearRxInterruptFlag();
                //NOTE: Increment buffer before the callabck
                epi->IRQincBufferCount();
                callbacks->IRQendpoint(epNum,Endpoint::OUT);
                epi->IRQwakeWaitingThreadOnOutEndpoint();
            }

            if(reg & USB_EP0R_CTR_TX)
            {
                USB->endpoint[epNum].IRQclearTxInterruptFlag();

                //NOTE: Decrement buffer before the callabck
                epi->IRQdecBufferCount();
                callbacks->IRQendpoint(epNum,Endpoint::IN);
                epi->IRQwakeWaitingThreadOnInEndpoint();
            }
        }
        //Read again the ISTR register so that if more endpoints have completed
        //a transaction, they are all serviced
        flags=USB->ISTR;
    }
}

/**
 * \internal
 * Actual high priority interrupt handler.
 */
void USBirqHpHandler() __attribute__ ((noinline));
void USBirqHpHandler()
{
    unsigned short flags=USB->ISTR;
    Callbacks *callbacks=Callbacks::IRQgetCallbacks();
    while(flags & USB_ISTR_CTR)
    {
        int epNum=flags & USB_ISTR_EP_ID;
        unsigned short reg=USB->endpoint[epNum].get();
        EndpointImpl *epi=EndpointImpl::IRQget(epNum);
        if(reg & USB_EP0R_CTR_RX)
        {
            USB->endpoint[epNum].IRQclearRxInterruptFlag();
            //NOTE: Increment buffer before the callabck
            epi->IRQincBufferCount();
            callbacks->IRQendpoint(epNum,Endpoint::OUT);
            epi->IRQwakeWaitingThreadOnOutEndpoint();
        }

        if(reg & USB_EP0R_CTR_TX)
        {
            USB->endpoint[epNum].IRQclearTxInterruptFlag();

            //NOTE: Decrement buffer before the callabck
            epi->IRQdecBufferCount();
            callbacks->IRQendpoint(epNum,Endpoint::IN);
            epi->IRQwakeWaitingThreadOnInEndpoint();
        }
        //Read again the ISTR register so that if more endpoints have completed
        //a transaction, they are all serviced
        flags=USB->ISTR;
    }
}

} //namespace mxusb

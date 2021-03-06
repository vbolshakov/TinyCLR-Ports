// Copyright Microsoft Corporation
// Copyright GHI Electronics, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string.h>
#include "LPC24.h"
#include "../../Drivers/USBClient/USBClient.h"

///////////////////////////////////////////////////////////////////////////////////////////
/// LPC24 USB Hardware state
///////////////////////////////////////////////////////////////////////////////////////////
/* Device Interrupt Bit Definitions */
#define FRAME_INT           0x00000001
#define EP_FAST_INT         0x00000002
#define EP_SLOW_INT         0x00000004
#define DEV_STAT_INT        0x00000008
#define CCEMTY_INT          0x00000010
#define CDFULL_INT          0x00000020
#define RxENDPKT_INT        0x00000040
#define TxENDPKT_INT        0x00000080
#define EP_RLZED_INT        0x00000100
#define ERR_INT             0x00000200

/* Rx & Tx Packet Length Definitions */
#define PKT_LNGTH_MASK      0x000003FF
#define PKT_DV              0x00000400
#define PKT_RDY             0x00000800

/* USB Control Definitions */
#define CTRL_RD_EN          0x00000001
#define CTRL_WR_EN          0x00000002

/* Command Codes */
#define CMD_SET_ADDR        0x00D00500
#define CMD_CFG_DEV         0x00D80500
#define CMD_SET_MODE        0x00F30500
#define CMD_RD_FRAME        0x00F50500
#define DAT_RD_FRAME        0x00F50200
#define CMD_RD_TEST         0x00FD0500
#define DAT_RD_TEST         0x00FD0200
#define CMD_SET_DEV_STAT    0x00FE0500
#define CMD_GET_DEV_STAT    0x00FE0500
#define DAT_GET_DEV_STAT    0x00FE0200
#define CMD_GET_ERR_CODE    0x00FF0500
#define DAT_GET_ERR_CODE    0x00FF0200
#define CMD_RD_ERR_STAT     0x00FB0500
#define DAT_RD_ERR_STAT     0x00FB0200
#define DAT_WR_BYTE(x)     (0x00000100 | ((x) << 16))
#define CMD_SEL_EP(x)      (0x00000500 | ((x) << 16))
#define DAT_SEL_EP(x)      (0x00000200 | ((x) << 16))
#define CMD_SEL_EP_CLRI(x) (0x00400500 | ((x) << 16))
#define DAT_SEL_EP_CLRI(x) (0x00400200 | ((x) << 16))
#define CMD_SET_EP_STAT(x) (0x00400500 | ((x) << 16))
#define CMD_CLR_BUF         0x00F20500
#define DAT_CLR_BUF         0x00F20200
#define CMD_VALID_BUF       0x00FA0500

/* Device Address Register Definitions */
#define DEV_ADDR_MASK       0x7F
#define DEV_EN              0x80

/* Device Configure Register Definitions */
#define CONF_DVICE          0x01

/* Device Mode Register Definitions */
#define AP_CLK              0x01
#define INAK_CI             0x02
#define INAK_CO             0x04
#define INAK_II             0x08
#define INAK_IO             0x10
#define INAK_BI             0x20
#define INAK_BO             0x40

/* Device Status Register Definitions */
#define DEV_CON             0x01
#define DEV_CON_CH          0x02
#define DEV_SUS             0x04
#define DEV_SUS_CH          0x08
#define DEV_RST             0x10

/* Error Code Register Definitions */
#define ERR_EC_MASK         0x0F
#define ERR_EA              0x10

/* Error Status Register Definitions */
#define ERR_PID             0x01
#define ERR_UEPKT           0x02
#define ERR_DCRC            0x04
#define ERR_TIMOUT          0x08
#define ERR_EOP             0x10
#define ERR_B_OVRN          0x20
#define ERR_BTSTF           0x40
#define ERR_TGL             0x80

/* Endpoint Select Register Definitions */
#define EP_SEL_F            0x01
#define EP_SEL_ST           0x02
#define EP_SEL_STP          0x04
#define EP_SEL_PO           0x08
#define EP_SEL_EPN          0x10
#define EP_SEL_B_1_FULL     0x20
#define EP_SEL_B_2_FULL     0x40

/* Endpoint Status Register Definitions */
#define EP_STAT_ST          0x01
#define EP_STAT_DA          0x20
#define EP_STAT_RF_MO       0x40
#define EP_STAT_CND_ST      0x80

#define USB_IRQn			22
struct UsbDeviceDriver {
    UsClientState *usClientState;

    bool txRunning[LPC24_USB_ENDPOINT_COUNT];
    bool txNeedZLPS[LPC24_USB_ENDPOINT_COUNT];

    uint8_t previousDeviceState;
    bool firstDescriptorPacket;
};

UsbDeviceDriver usbDeviceDrivers[LPC24_TOTAL_USB_CONTROLLERS];

union EndpointConfiguration {
    struct {
        unsigned EE : 1;      // Endpoint enable (1 = enable)
        unsigned DE : 1;      // Double buffer enable (1 = double buffered)
        unsigned MPS : 10;      // Maximum packet size (iso=1-1023, blk=8,16,32,64, int=1-64
        unsigned ED : 1;      // Endpoint direction (1 = IN)
        unsigned ET : 2;      // Endpoint type (1=iso, 2=blk, 3=int)
        unsigned EN : 4;      // Endpoint number (1-15)
        unsigned AISN : 3;      // Alternate Interface number
        unsigned IN : 3;      // Interface number
        unsigned CN : 2;      // Configuration number
    } bits;
    uint32_t word;
};

static EndpointConfiguration EndpointInit[LPC24_USB_ENDPOINT_COUNT];     // Corresponds to endpoint configuration RAM at LPC24xx_USB::UDCCRx
static int32_t nacking_rx_OUT_data[LPC24_USB_ENDPOINT_COUNT];

bool LPC24_UsbDevice_ProtectPins(int32_t controllerIndex, bool On);
void LPC24_UsbDevice_InterruptHandler(void* param);
void LPC24_UsbDevice_TxPacket(UsClientState* usClientState, int32_t endpoint);
void LPC24_UsbDevice_ProcessEP0(UsClientState* usClientState, int32_t in, int32_t setup);
void LPC24_UsbDevice_ProcessEndPoint(UsClientState* usClientState, int32_t ep, int32_t in);
void LPC24_UsbDevice_Enpoint_TxInterruptHandler(UsClientState* usClientState, uint32_t endpoint);
void LPC24_UsbDevice_Enpoint_RxInterruptHandler(UsClientState* usClientState, uint32_t endpoint);
void LPC24_UsbDevice_ResetEvent(UsClientState* usClientState);
void LPC24_UsbDevice_SuspendEvent(UsClientState* usClientState);
void LPC24_UsbDevice_ResumeEvent(UsClientState* usClientState);
void LPC24_UsbDevice_ControlNext(UsClientState* usClientState);
uint32_t LPC24_UsbDevice_EPAdr(uint32_t EPNum, int8_t in);

void LPC24_UsbDevice_AddApi(const TinyCLR_Api_Manager* apiManager) {
    TinyCLR_UsbClient_AddApi(apiManager);

}
const TinyCLR_Api_Info*LPC24_UsbDevice_GetRequiredApi() {
    return TinyCLR_UsbClient_GetRequiredApi();
}

void LPC24_UsbDevice_Reset() {
    return TinyCLR_UsbClient_Reset(0);
}

void LPC24_UsbDevice_InitializeConfiguration(UsClientState *usClientState) {
    int32_t controllerIndex = 0;

    if (usClientState != nullptr) {
        usClientState->controllerIndex = controllerIndex;

        usClientState->maxFifoPacketCountDefault = LPC24_USB_PACKET_FIFO_COUNT;
        usClientState->totalEndpointsCount = LPC24_USB_ENDPOINT_COUNT;
        usClientState->totalPipesCount = LPC24_USB_PIPE_COUNT;

        // Update endpoint size DeviceDescriptor Configuration if device value is different to default value
        usClientState->deviceDescriptor.MaxPacketSizeEp0 = TinyCLR_UsbClient_GetEndpointSize(0);

        usbDeviceDrivers[controllerIndex].usClientState = usClientState;
    }
}

bool LPC24_UsbDevice_Initialize(UsClientState *usClientState) {
    DISABLE_INTERRUPTS_SCOPED(irq);

    if (usClientState == nullptr)
        return false;

    int32_t controllerIndex = usClientState->controllerIndex;

    LPC24_InterruptInternal_Activate(USB_IRQn, (uint32_t*)&LPC24_UsbDevice_InterruptHandler, 0);

    for (int32_t i = 0; i < LPC24_USB_ENDPOINT_COUNT; i++)
        EndpointInit[i].word = 0;       // All useable endpoints initialize to unused

    for (auto pipe = 0; pipe < LPC24_USB_ENDPOINT_COUNT; pipe++) {
        auto idx = 0;
        if (usClientState->pipes[pipe].RxEP != USB_ENDPOINT_NULL) {
            idx = usClientState->pipes[pipe].RxEP;
            EndpointInit[idx].bits.ED = 0;
            EndpointInit[idx].bits.DE = 0;
        }

        if (usClientState->pipes[pipe].TxEP != USB_ENDPOINT_NULL) {
            idx = usClientState->pipes[pipe].TxEP;
            EndpointInit[idx].bits.ED = 1;
            EndpointInit[idx].bits.DE = 1;
        }

        if (idx != 0) {
            EndpointInit[idx].bits.EN = idx;
            EndpointInit[idx].bits.IN = 0;//itfc->bInterfaceNumber;
            EndpointInit[idx].bits.ET = USB_ENDPOINT_ATTRIBUTE_BULK & 0x03; //ep->bmAttributes & 0x03;
            EndpointInit[idx].bits.CN = 1;        // Always only 1 configuration = 1
            EndpointInit[idx].bits.AISN = 0;        // No alternate interfaces
            EndpointInit[idx].bits.EE = 1;        // Enable this endpoint
            EndpointInit[idx].bits.MPS = usClientState->maxEndpointsPacketSize[idx];
        }
    }

    usClientState->firstGetDescriptor = true;

    LPC24_UsbDevice_ProtectPins(controllerIndex, true);

    return true;
}

bool LPC24_UsbDevice_Uninitialize(UsClientState *usClientState) {
    DISABLE_INTERRUPTS_SCOPED(irq);

    LPC24_InterruptInternal_Deactivate(USB_IRQn);

    if (usClientState != nullptr) {
        LPC24_UsbDevice_ProtectPins(usClientState->controllerIndex, false);
        usClientState->currentState = USB_DEVICE_STATE_UNINITIALIZED;
    }

    return true;
}

bool LPC24_UsbDevice_StartOutput(UsClientState* usClientState, int32_t endpoint) {
    int32_t m, n, val;

    DISABLE_INTERRUPTS_SCOPED(irq);

    /* if the halt feature for this endpoint is set, then just
       clear all the characters */
    if (usClientState->endpointStatus[endpoint] & USB_STATUS_ENDPOINT_HALT) {
        TinyCLR_UsbClient_ClearEndpoints(usClientState, endpoint);
        return true;
    }

    //If txRunning, interrupts will drain the queue
    if (!usbDeviceDrivers[usClientState->controllerIndex].txRunning[endpoint]) {
        usbDeviceDrivers[usClientState->controllerIndex].txRunning[endpoint] = true;

        // Calling both LPC24_UsbDevice_TxPacket & EP_TxISR in this routine could cause a TX FIFO overflow
        LPC24_UsbDevice_TxPacket(usClientState, endpoint);
    }
    else if (irq.IsDisabled()) {

        n = LPC24_UsbDevice_EPAdr(endpoint, 1); // It is an output endpoint for sure
        if ((USBEpIntSt & (1 << n)))//&& (USBEpIntEn & (1 << n)) )//only if enabled
        {
            m = n >> 1;

            if (m == 0)//EP0
            {
                USBEpIntClr = 1 << n;
                while ((USBDevIntSt & CDFULL_INT) == 0);
                val = USBCmdData;

                if (val & EP_SEL_STP)        /* Setup Packet */
                {
                    LPC24_UsbDevice_ProcessEP0(usClientState, 0, 1);// out setup
                }
                else {
                    if ((n & 1) == 0)                /* OUT Endpoint */
                    {
                        LPC24_UsbDevice_ProcessEP0(usClientState, 0, 0);// out not setup
                    }
                    else {
                        LPC24_UsbDevice_ProcessEP0(usClientState, 1, 0);// in not setup
                    }
                }
            }
            else {
                if (usClientState->queues[m] && usClientState->isTxQueue[endpoint])
                    LPC24_UsbDevice_ProcessEndPoint(usClientState, m, 1);//out
                else
                    LPC24_UsbDevice_ProcessEndPoint(usClientState, m, 0);//in
            }
        }
    }

    return true;
}

bool LPC24_UsbDevice_RxEnable(UsClientState* usClientState, int32_t endpoint) {
    if (endpoint >= LPC24_USB_ENDPOINT_COUNT)
        return false;

    DISABLE_INTERRUPTS_SCOPED(irq);

    if (nacking_rx_OUT_data[endpoint])
        LPC24_UsbDevice_Enpoint_RxInterruptHandler(usClientState, endpoint);//force interrupt to read the pending EP

    return true;
}

static uint8_t  LPC24_UsbDevice_DeviceAddress = 0;

#define CONTORL_EP_ADDR	0x80

#define USB_POWER           0
#define USB_IF_NUM          4
#define USB_EP_NUM          32
#define USB_MAX_PACKET0     64
#define USB_DMA             0
#define USB_DMA_EP          0x00000000

#define USB_POWER_EVENT     0
#define USB_RESET_EVENT     1
#define USB_WAKEUP_EVENT    0
#define USB_SOF_EVENT       0
#define USB_ERROR_EVENT     0
#define USB_EP_EVENT        0x0003
#define USB_CONFIGURE_EVENT 1
#define USB_INTERFACE_EVENT 0
#define USB_FEATURE_EVENT   0

#define EP_MSK_CTRL 0x0001      /* Control Endpoint Logical address Mask */
#define EP_MSK_BULK 0xC924      /* Bulk Endpoint Logical address Mask */
#define EP_MSK_INT  0x4492      /* Interrupt Endpoint Logical address Mask */
#define EP_MSK_ISO  0x1248      /* Isochronous Endpoint Logical address Mask */

static void LPC24_UsbDevice_WrCmd(uint32_t cmd) {
    USBDevIntClr = CCEMTY_INT | CDFULL_INT;
    USBCmdCode = cmd;
    while ((USBDevIntSt & CCEMTY_INT) == 0);
}
static void LPC24_UsbDevice_WrCmdDat(uint32_t cmd, uint32_t val) {
    USBDevIntClr = CCEMTY_INT;
    USBCmdCode = cmd;
    while ((USBDevIntSt & CCEMTY_INT) == 0);
    USBDevIntClr = CCEMTY_INT;
    USBCmdCode = val;
    while ((USBDevIntSt & CCEMTY_INT) == 0);
}
static uint32_t LPC24_UsbDevice_RdCmdDat(uint32_t cmd) {
    USBDevIntClr = CCEMTY_INT | CDFULL_INT;
    USBCmdCode = cmd;
    while ((USBDevIntSt & CDFULL_INT) == 0);
    return (USBCmdData);
}

static void LPC24_UsbDevice_SetAddress(uint32_t adr) {
    LPC24_UsbDevice_WrCmdDat(CMD_SET_ADDR, DAT_WR_BYTE(DEV_EN | adr)); /* Don't wait for next */
    LPC24_UsbDevice_WrCmdDat(CMD_SET_ADDR, DAT_WR_BYTE(DEV_EN | adr)); /*  Setup Status Phase */
}

static void LPC24_UsbDevice_HardwareReset(void) {
    USBEpInd = 0;
    USBEpMaxPSize = USB_MAX_PACKET0;
    USBEpInd = 1;
    USBEpMaxPSize = USB_MAX_PACKET0;

    while ((USBDevIntSt & EP_RLZED_INT) == 0);

    USBEpIntClr = 0xFFFFFFFF;
    USBEpIntEn = 0xFFFFFFFF ^ USB_DMA_EP;
    USBDevIntClr = 0xFFFFFFFF;
    USBDevIntEn = DEV_STAT_INT | EP_SLOW_INT |
        (USB_SOF_EVENT ? FRAME_INT : 0) |
        (USB_ERROR_EVENT ? ERR_INT : 0);

}

void LPC24_UsbDevice_Connect(bool con) {
    LPC24_UsbDevice_WrCmdDat(CMD_SET_DEV_STAT, DAT_WR_BYTE(con ? DEV_CON : 0));
}

uint32_t LPC24_UsbDevice_EPAdr(uint32_t EPNum, int8_t in) {
    uint32_t val;

    val = (EPNum & 0x0F) << 1;
    if (in) {
        val += 1;
    }
    return (val);
}

static uint32_t USB_WriteEP(uint32_t EPNum, uint8_t *pData, uint32_t cnt) {
    uint32_t n, g;
    USBCtrl = ((EPNum & 0x0F) << 2) | CTRL_WR_EN;

    USBTxPLen = cnt;

    for (n = 0; n < (cnt + 3) / 4; n++) {
        g = *(pData + 3);
        g <<= 8;
        g |= *(pData + 2);
        g <<= 8;
        g |= *(pData + 1);
        g <<= 8;
        g |= *pData;
        USBTxData = g;
        pData += 4;
    }

    USBCtrl = 0;

    LPC24_UsbDevice_WrCmd(CMD_SEL_EP(LPC24_UsbDevice_EPAdr(EPNum, 1)));
    LPC24_UsbDevice_WrCmd(CMD_VALID_BUF);

    return (cnt);
}

static uint32_t LPC24_UsbDevice_ReadEP(uint32_t EPNum, uint8_t *pData) {
    uint32_t cnt, n, d;

    USBCtrl = ((EPNum & 0x0F) << 2) | CTRL_RD_EN;

    do {
        cnt = USBRxPLen;
    } while ((cnt & PKT_RDY) == 0);

    cnt &= PKT_LNGTH_MASK;

    for (n = 0; n < (cnt + 3) / 4; n++) {
        d = USBRxData;
        *pData++ = d;
        *pData++ = d >> 8;
        *pData++ = d >> 16;
        *pData++ = d >> 24;
    }

    USBCtrl = 0;

    if (((EP_MSK_ISO >> EPNum) & 1) == 0)    /* Non-Isochronous Endpoint */
    {
        LPC24_UsbDevice_WrCmd(CMD_SEL_EP(LPC24_UsbDevice_EPAdr(EPNum, 0)));
        LPC24_UsbDevice_WrCmd(CMD_CLR_BUF);
    }

    return (cnt);
}

static void LPC24_UsbDevice_SetStallEP(uint32_t EPNum, int8_t in) {
    LPC24_UsbDevice_WrCmdDat(CMD_SET_EP_STAT(LPC24_UsbDevice_EPAdr(EPNum, in)), DAT_WR_BYTE(EP_STAT_ST));
}

void LPC24_UsbDevice_ProcessEndPoint(UsClientState* usClientState, int32_t ep, int32_t in) {
    int32_t val;

    if (in) {
        LPC24_UsbDevice_Enpoint_TxInterruptHandler(usClientState, ep);
    }
    else {
        USBEpIntClr = 1 << LPC24_UsbDevice_EPAdr(ep, in);
        while ((USBDevIntSt & CDFULL_INT) == 0);
        val = USBCmdData;
        LPC24_UsbDevice_Enpoint_RxInterruptHandler(usClientState, ep);
    }
}

void LPC24_UsbDevice_ConfigEP(uint8_t ep_addr, int8_t in, uint8_t size) {
    uint32_t num;

    num = LPC24_UsbDevice_EPAdr(ep_addr, in);
    USBReEp |= (1 << num);
    USBEpInd = num;
    USBEpMaxPSize = size;

    while ((USBDevIntSt & EP_RLZED_INT) == 0);

    USBDevIntClr = EP_RLZED_INT;
}
void LPC24_UsbDevice_EnableEP(uint32_t EPNum, int8_t in) {
    LPC24_UsbDevice_WrCmdDat(CMD_SET_EP_STAT(LPC24_UsbDevice_EPAdr(EPNum, in)), DAT_WR_BYTE(0));
}
void USB_DisableEP(int32_t EPNum, int8_t in) {
    LPC24_UsbDevice_WrCmdDat(CMD_SET_EP_STAT(LPC24_UsbDevice_EPAdr(EPNum, in)), DAT_WR_BYTE(EP_STAT_DA));
}
void LPC24_UsbDevice_ResetEP(uint32_t EPNum, int8_t in) {
    LPC24_UsbDevice_WrCmdDat(CMD_SET_EP_STAT(LPC24_UsbDevice_EPAdr(EPNum, in)), DAT_WR_BYTE(0));
}

void USB_HW_Configure(bool cfg) {
    LPC24_UsbDevice_WrCmdDat(CMD_CFG_DEV, DAT_WR_BYTE(cfg ? CONF_DVICE : 0));

    USBReEp = 0x00000003;
    while ((USBDevIntSt & EP_RLZED_INT) == 0);
    USBDevIntClr = EP_RLZED_INT;
}

void LPC24_UsbDevice_StartHardware() {
    *(uint32_t*)0xE01FC0C4 |= 0x80000000;
    USBClkCtrl = (1 << 1) | (1 << 3) | (1 << 4);

    LPC24_UsbDevice_PinConfiguration();

    LPC24_UsbDevice_HardwareReset();
    LPC24_UsbDevice_SetAddress(0);

    USBDevIntEn = DEV_STAT_INT;	/* Enable Device Status Interrupt */

    LPC24_UsbDevice_Connect(false);
    // delay if removed and then connected...
    LPC24_Time_Delay(nullptr, 120 * 1000);

    LPC24_UsbDevice_Connect(true);
}

void LPC24_UsbDevice_StopHardware() {
    LPC24_UsbDevice_Connect(false);
}

void LPC24_UsbDevice_TxPacket(UsClientState* usClientState, int32_t endpoint) {
    DISABLE_INTERRUPTS_SCOPED(irq);

    // transmit a packet on UsbPortNum, if there are no more packets to transmit, then die
    USB_PACKET64* Packet64;

    for (;;) {
        Packet64 = TinyCLR_UsbClient_TxDequeue(usClientState, endpoint);

        if (Packet64 == nullptr || Packet64->Size > 0) {
            break;
        }
    }

    if (Packet64) {

        USB_WriteEP(endpoint, Packet64->Buffer, Packet64->Size);

        usbDeviceDrivers[usClientState->controllerIndex].txNeedZLPS[endpoint] = false;
        if (Packet64->Size == 64)
            usbDeviceDrivers[usClientState->controllerIndex].txNeedZLPS[endpoint] = true;
    }
    else {
        // send the zero length packet since we landed on the FIFO boundary before
        // (and we queued a zero length packet to transmit)
        if (usbDeviceDrivers[usClientState->controllerIndex].txNeedZLPS[endpoint]) {
            USB_WriteEP(endpoint, (uint8_t*)nullptr, 0);
            usbDeviceDrivers[usClientState->controllerIndex].txNeedZLPS[endpoint] = false;
        }

        // no more data
        usbDeviceDrivers[usClientState->controllerIndex].txRunning[endpoint] = false;
    }
}
void LPC24_UsbDevice_ControlNext(UsClientState *usClientState) {
    if (usClientState->dataCallback) {
        // this call can't fail
        usClientState->dataCallback(usClientState);

        if (usClientState->dataSize == 0) {
            USB_WriteEP(CONTORL_EP_ADDR, (uint8_t*)nullptr, 0);
            usClientState->dataCallback = nullptr;                         // Stop sending stuff if we're done
        }
        else {
            USB_WriteEP(CONTORL_EP_ADDR, usClientState->ptrData, usClientState->dataSize);

            if (usClientState->dataSize < LPC24_USB_ENDPOINT_SIZE) // If packet is less than full length
            {
                usClientState->dataCallback = nullptr; // Stop sending stuff if we're done
            }

            // special handling the USB state set address test, cannot use the first descriptor as the ADDRESS state is handle in the hardware
            if (usbDeviceDrivers[usClientState->controllerIndex].firstDescriptorPacket) {
                usClientState->dataCallback = nullptr;
            }

        }
    }
}

#define USB_USBCLIENT_ID 0
void LPC24_UsbDevice_InterruptHandler(void* param) {
    DISABLE_INTERRUPTS_SCOPED(irq);
    int32_t disr, val, n, m;

    disr = USBDevIntSt;                      /* Device Interrupt Status */
    USBDevIntClr = disr;                       /* A known issue on LPC214x */

    UsClientState *usClientState = usbDeviceDrivers[USB_USBCLIENT_ID].usClientState;

    if (disr & DEV_STAT_INT) {
        LPC24_UsbDevice_WrCmd(CMD_GET_DEV_STAT);
        val = LPC24_UsbDevice_RdCmdDat(DAT_GET_DEV_STAT);       /* Device Status */

        if (val & DEV_RST)                     /* Reset */
        {
            LPC24_UsbDevice_ResetEvent(usClientState);
        }

        if (val & DEV_SUS_CH)                  /* Suspend/Resume */
        {
            if (val & DEV_SUS)                   /* Suspend */
            {
                LPC24_UsbDevice_SuspendEvent(usClientState);
            }
            else                               /* Resume */
            {
                LPC24_UsbDevice_ResumeEvent(usClientState);
            }
        }

        goto isr_end;
    }

    /* Endpoint's Slow Interrupt */
    if (disr & EP_SLOW_INT) {
        for (n = 0; n < USB_EP_NUM; n++)     /* Check All Endpoints */
        {
            if ((USBEpIntSt & (1 << n))) {
                m = n >> 1;

                if (m == 0)//EP0
                {
                    USBEpIntClr = 1 << n;
                    while ((USBDevIntSt & CDFULL_INT) == 0);
                    val = USBCmdData;

                    if (val & EP_SEL_STP)        /* Setup Packet */
                    {
                        LPC24_UsbDevice_ProcessEP0(usClientState, 0, 1);// out setup
                        continue;
                    }
                    if ((n & 1) == 0)                /* OUT Endpoint */
                    {
                        LPC24_UsbDevice_ProcessEP0(usClientState, 0, 0);// out not setup
                    }
                    else {
                        LPC24_UsbDevice_ProcessEP0(usClientState, 1, 0);// in not setup
                    }

                    continue;
                }
                if ((n & 1) == 0)                /* OUT Endpoint */
                {
                    LPC24_UsbDevice_ProcessEndPoint(usClientState, m, 0);//out
                }
                else                           /* IN Endpoint */
                {
                    LPC24_UsbDevice_ProcessEndPoint(usClientState, m, 1);//in
                }
            }
        }
    }
isr_end:
    return;
}

void LPC24_UsbDevice_ProcessEP0(UsClientState *usClientState, int32_t in, int32_t setup) {
    uint32_t EP_INTR;
    int32_t i;

    DISABLE_INTERRUPTS_SCOPED(irq);

    if (setup) {
        uint8_t   len = 0;

        len = LPC24_UsbDevice_ReadEP(0x00, usClientState->controlEndpointBuffer);

        // special handling for the very first SETUP command - Getdescriptor[DeviceType], the host looks for 8 bytes data only
        TinyCLR_UsbClient_SetupPacket* Setup = (TinyCLR_UsbClient_SetupPacket*)&usClientState->controlEndpointBuffer[0];
        if ((Setup->Request == USB_GET_DESCRIPTOR) && (((Setup->Value & 0xFF00) >> 8) == USB_DEVICE_DESCRIPTOR_TYPE) && (Setup->Length != 0x12))
            usbDeviceDrivers[usClientState->controllerIndex].firstDescriptorPacket = true;
        else
            usbDeviceDrivers[usClientState->controllerIndex].firstDescriptorPacket = false;

        // send it to the upper layer
        usClientState->ptrData = &usClientState->controlEndpointBuffer[0];
        usClientState->dataSize = len;

        uint8_t result = TinyCLR_UsbClient_ControlCallback(usClientState);

        switch (result) {
        case USB_STATE_ADDRESS:
            LPC24_UsbDevice_DeviceAddress = usClientState->address | 0x80;
            break;

        case USB_STATE_DONE:
            usClientState->dataCallback = nullptr;
            break;

        case USB_STATE_STALL:
            LPC24_UsbDevice_SetStallEP(0, 0);
            LPC24_UsbDevice_SetStallEP(0, 1);
            break;

        case USB_STATE_CONFIGURATION:
            USB_HW_Configure(true);
            for (i = 1; i < 16; i++) {
                // direction in
                LPC24_UsbDevice_ConfigEP(i, 1, 64);
                LPC24_UsbDevice_EnableEP(i, 1);
                LPC24_UsbDevice_ResetEP(i, 1);

                // direction out
                LPC24_UsbDevice_ConfigEP(i, 0, 64);
                LPC24_UsbDevice_EnableEP(i, 0);
                LPC24_UsbDevice_ResetEP(i, 0);
            }

            break;

        }

        if (result != USB_STATE_STALL) {
            LPC24_UsbDevice_ControlNext(usClientState);

            // If the port is configured, then output any possible withheld data
            if (result == USB_STATE_CONFIGURATION) {
                for (int32_t ep = 0; ep < LPC24_USB_ENDPOINT_COUNT; ep++) {
                    if (usClientState->isTxQueue[ep])
                        LPC24_UsbDevice_StartOutput(usClientState, ep);
                }
            }
        }
    }
    else if (in) {
        // If previous packet has been sent and UDC is ready for more
        LPC24_UsbDevice_ControlNext(usClientState);      // See if there is more to send

        if (LPC24_UsbDevice_DeviceAddress & 0x80) {
            LPC24_UsbDevice_DeviceAddress &= 0x7F;
            LPC24_UsbDevice_SetAddress(LPC24_UsbDevice_DeviceAddress);
        }
    }
}

void LPC24_UsbDevice_Enpoint_TxInterruptHandler(UsClientState *usClientState, uint32_t endpoint) {
    uint32_t EP_INTR;
    int32_t val;

    if (USBEpIntSt & (1 << LPC24_UsbDevice_EPAdr(endpoint, 1)))//done sending?
    {
        //clear interrupt flag
        USBEpIntClr = 1 << LPC24_UsbDevice_EPAdr(endpoint, 1);
        while ((USBDevIntSt & CDFULL_INT) == 0);
        val = USBCmdData;

        // successfully transmitted packet, time to send the next one
        LPC24_UsbDevice_TxPacket(usClientState, endpoint);
    }
}

void LPC24_UsbDevice_Enpoint_RxInterruptHandler(UsClientState *usClientState, uint32_t endpoint) {
    bool          DisableRx;
    USB_PACKET64* Packet64 = TinyCLR_UsbClient_RxEnqueue(usClientState, endpoint, DisableRx);

    /* copy packet in, making sure that Packet64->Buffer is never overflowed */
    if (Packet64) {
        uint8_t   len = 0;//USB.UDCBCRx[EPno] & LPC24xx_USB::UDCBCR_mask;
        uint32_t* packetBuffer = (uint32_t*)Packet64->Buffer;
        len = LPC24_UsbDevice_ReadEP(endpoint, Packet64->Buffer);

        // clear packet status
        nacking_rx_OUT_data[endpoint] = 0;
        Packet64->Size = len;
    }
    else {
        /* flow control should absolutely protect us from ever
        getting here, so if we do, it is a bug */
        nacking_rx_OUT_data[endpoint] = 1;//we will need to triger next interrupt
    }

}

void LPC24_UsbDevice_SuspendEvent(UsClientState *usClientState) {
    // SUSPEND event only happened when Host(PC) set the device to SUSPEND
    // as there is always SOF every 1ms on the BUS to keep the device from
    // suspending. Therefore, the REMOTE wake up is not necessary at the ollie side
    usbDeviceDrivers[usClientState->controllerIndex].previousDeviceState = usClientState->deviceState;

    usClientState->deviceState = USB_DEVICE_STATE_SUSPENDED;

    TinyCLR_UsbClient_StateCallback(usClientState);
}


void LPC24_UsbDevice_ResumeEvent(UsClientState *usClientState) {
    usClientState->deviceState = usbDeviceDrivers[usClientState->controllerIndex].previousDeviceState;

    TinyCLR_UsbClient_StateCallback(usClientState);
}

void LPC24_UsbDevice_ResetEvent(UsClientState *usClientState) {
    LPC24_UsbDevice_HardwareReset();
    LPC24_UsbDevice_DeviceAddress = 0;

    // clear all flags
    TinyCLR_UsbClient_ClearEvent(usClientState, 0xFFFFFFFF);

    for (int32_t ep = 0; ep < LPC24_USB_ENDPOINT_COUNT; ep++) {
        usbDeviceDrivers[usClientState->controllerIndex].txRunning[ep] = false;
        usbDeviceDrivers[usClientState->controllerIndex].txNeedZLPS[ep] = false;
    }

    usClientState->deviceState = USB_DEVICE_STATE_DEFAULT;
    usClientState->address = 0;
    TinyCLR_UsbClient_StateCallback(usClientState);
}

bool LPC24_UsbDevice_ProtectPins(int32_t controllerIndex, bool On) {
    UsClientState *usClientState = usbDeviceDrivers[controllerIndex].usClientState;

    DISABLE_INTERRUPTS_SCOPED(irq);

    if (usClientState) {
        if (On) {
            usClientState->deviceState = USB_DEVICE_STATE_ATTACHED;

            TinyCLR_UsbClient_StateCallback(usClientState);

            LPC24_UsbDevice_StartHardware();
        }
        else {
            LPC24_UsbDevice_HardwareReset();

            LPC24_UsbDevice_DeviceAddress = 0;

            LPC24_UsbDevice_StopHardware();
        }

        return true;
    }

    return false;
}

bool TinyCLR_UsbClient_Initialize(UsClientState* usClientState) {
    return LPC24_UsbDevice_Initialize(usClientState);
}

bool TinyCLR_UsbClient_Uninitialize(UsClientState* usClientState) {
    return LPC24_UsbDevice_Uninitialize(usClientState);
}

bool TinyCLR_UsbClient_StartOutput(UsClientState* usClientState, int32_t endpoint) {
    return LPC24_UsbDevice_StartOutput(usClientState, endpoint);
}

bool TinyCLR_UsbClient_RxEnable(UsClientState* usClientState, int32_t endpoint) {
    return LPC24_UsbDevice_RxEnable(usClientState, endpoint);
}

void TinyCLR_UsbClient_Delay(uint64_t microseconds) {
    LPC24_Time_Delay(nullptr, microseconds);
}

uint64_t TinyCLR_UsbClient_Now() {
    return LPC24_Time_GetCurrentProcessorTime();
}

void TinyCLR_UsbClient_InitializeConfiguration(UsClientState *usClientState) {
    LPC24_UsbDevice_InitializeConfiguration(usClientState);
}

uint32_t TinyCLR_UsbClient_GetEndpointSize(int32_t endpoint) {
    return endpoint == 0 ? LPC24_USB_ENDPOINT0_SIZE : LPC24_USB_ENDPOINT_SIZE;
}


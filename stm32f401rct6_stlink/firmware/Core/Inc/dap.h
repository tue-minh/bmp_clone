#ifndef __DAP_H__
#define __DAP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define DAP_PACKET_SIZE        64U
#define DAP_PACKET_COUNT       4U

/* CMSIS-DAP Command IDs */
#define ID_DAP_Info               0x00U
#define ID_DAP_HostStatus         0x01U
#define ID_DAP_Connect            0x02U
#define ID_DAP_Disconnect         0x03U
#define ID_DAP_TransferConfigure  0x04U
#define ID_DAP_Transfer           0x05U
#define ID_DAP_TransferBlock      0x06U
#define ID_DAP_TransferAbort      0x07U
#define ID_DAP_WriteABORT         0x08U
#define ID_DAP_Delay              0x09U
#define ID_DAP_ResetTarget        0x0AU
#define ID_DAP_SWJ_Pins           0x10U
#define ID_DAP_SWJ_Clock          0x11U
#define ID_DAP_SWJ_Sequence       0x12U
#define ID_DAP_SWD_Configure      0x13U
#define ID_DAP_SWD_Sequence       0x1DU
#define ID_DAP_QueueCommands      0x7EU
#define ID_DAP_ExecuteCommands    0x7FU
#define ID_DAP_Invalid            0xFFU

/* CMSIS-DAP Status Codes */
#define DAP_OK                    0x00U
#define DAP_ERROR                 0xFFU

/* CMSIS-DAP Debug Port Types */
#define DAP_PORT_AUTODETECT       0x00U
#define DAP_PORT_DISABLED         0x00U
#define DAP_PORT_SWD              0x01U
#define DAP_PORT_JTAG             0x02U

/* Initialize CMSIS-DAP state & queue */
void DAP_Init(void);

/* Execute a single CMSIS-DAP command buffer -> fills response (64 bytes) */
uint32_t DAP_ProcessCommand(const uint8_t *request, uint8_t *response);

/* Called from USB Custom HID OUT interrupt callback when a 64B packet arrives */
void DAP_USB_OutCallback(const uint8_t *reportBuf);

/* Poll & process queued CMSIS-DAP packets from main loop */
void DAP_Task(void);

/* Returns true if the RX queue has space for another 64-byte HID packet */
bool DAP_CanAcceptPacket(void);

/* Returns true if a PC debugger is currently connected over CMSIS-DAP */
bool DAP_IsConnected(void);

#ifdef __cplusplus
}
#endif

#endif /* __DAP_H__ */

#ifndef __USBD_CDC_IF_H__
#define __USBD_CDC_IF_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_cdc.h"

extern USBD_CDC_ItfTypeDef USBD_Interface_fops_FS;

void CDC_SetUsbClassId(uint8_t classId);
void CDC_UART_Task(void);

#ifdef __cplusplus
}
#endif

#endif /* __USBD_CDC_IF_H__ */

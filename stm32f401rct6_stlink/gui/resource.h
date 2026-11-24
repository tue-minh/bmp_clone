#pragma once

#define IDI_APP_ICON            101

// Menu IDs
#define IDM_FILE_OPEN           1001
#define IDM_FILE_DUMP_BIN       1002
#define IDM_FILE_EXIT           1003

#define IDM_TARGET_CONNECT      1101
#define IDM_TARGET_DISCONNECT   1102
#define IDM_TARGET_RESET        1103
#define IDM_TARGET_ERASE        1104
#define IDM_TARGET_HALT         1105
#define IDM_TARGET_RUN          1106
#define IDM_TARGET_STEP         1107
#define IDM_TARGET_REGS         1108

#define IDM_TOOLS_HEXVIEW       1201
#define IDM_TOOLS_UART          1202
#define IDM_TOOLS_OCD_CFG       1203
#define IDM_TOOLS_GDB_SERVER    1204
#define IDM_TOOLS_DEBUG         1205

#define IDM_HELP_LED            1301
#define IDM_HELP_ABOUT          1302

// Main Window Control IDs
#define IDC_LBL_USB_STATUS      2001
#define IDC_CHK_USE_OCD         2002
#define IDC_CMB_OCD_TARGET      2003
#define IDC_CMB_SWD_SPEED       2004
#define IDC_CHK_UNDER_RESET     2005

#define IDC_BTN_CONNECT         2010
#define IDC_BTN_DISCONNECT      2019
#define IDC_BTN_FLASH           2011
#define IDC_BTN_VERIFY          2012
#define IDC_BTN_HW_RESET        2013
#define IDC_BTN_SWD_RESET       2023
#define IDC_BTN_ERASE           2014
#define IDC_BTN_OPEN_DEBUG      2024

// Debug Window Control IDs
#define IDC_DBG_BTN_START       2101
#define IDC_DBG_BTN_EXIT        2102
#define IDC_DBG_BTN_HALT        2103
#define IDC_DBG_BTN_RUN         2104
#define IDC_DBG_BTN_STEP_INTO   2105
#define IDC_DBG_BTN_STEP_OUT    2106
#define IDC_DBG_BTN_REGS        2107
#define IDC_DBG_TEXT            2108

#define IDC_EDT_FILE_PATH       2020
#define IDC_BTN_BROWSE          2021
#define IDC_EDT_BASE_ADDR       2022

#define IDC_EDT_TARGET_INFO     2030
#define IDC_EDT_DEBUG_INFO      2031
#define IDC_EDT_LOG             2032
#define IDC_PROGRESS            2033
#define IDC_BTN_CLEAR_LOG       2034

// Hex Viewer Window Control IDs
#define IDC_HEX_ADDR            3001
#define IDC_HEX_LEN             3002
#define IDC_HEX_BTN_READ        3003
#define IDC_HEX_TEXT            3004
#define IDC_MEM32_ADDR          3005
#define IDC_MEM32_VAL           3006
#define IDC_MEM32_BTN_READ      3007
#define IDC_MEM32_BTN_WRITE     3008

// UART Window Control IDs
#define IDC_UART_COM_CMB        3101
#define IDC_UART_BAUD_CMB       3102
#define IDC_UART_BTN_CONN       3103
#define IDC_UART_LOG            3104
#define IDC_UART_INPUT          3105
#define IDC_UART_BTN_SEND       3106
#define IDC_UART_BTN_PROBE      3107
#define IDC_UART_BTN_CLEAR      3108
#define IDC_UART_CHK_ECHO       3109
#define IDC_UART_CHK_CR         3110
#define IDC_UART_CHK_LF         3111
#define IDC_UART_CHK_TAB        3112
#define IDC_UART_CHK_ESC        3113

// OpenOCD Settings Dialog IDs
#define IDC_OCD_PATH_EDT        3201
#define IDC_OCD_PATH_BROWSE     3202
#define IDC_OCD_BTN_SAVE        3203
#define IDC_OCD_BTN_CANCEL      3204

// Custom Window Messages for background worker -> UI thread
#define WM_APP_LOG              (WM_APP + 1)
#define WM_APP_PROGRESS         (WM_APP + 2)
#define WM_APP_CONNECT_DONE     (WM_APP + 3)
#define WM_APP_OP_DONE          (WM_APP + 4)
#define WM_APP_DEBUG_INFO       (WM_APP + 5)
#define WM_APP_HEX_DATA         (WM_APP + 6)
#define WM_APP_MEM32_VAL        (WM_APP + 7)
#define WM_APP_UART_RX          (WM_APP + 8)
#define WM_APP_UART_CLOSED      (WM_APP + 9)

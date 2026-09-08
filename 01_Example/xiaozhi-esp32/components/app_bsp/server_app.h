#pragma once

#include <freertos/FreeRTOS.h>
#include <esp_err.h>
#include "sdcard_bsp.h"
#include "traverse_nvs.h"

extern EventGroupHandle_t ServerPortGroups;


/* Only one of the AP/STA entry points is normally used per boot. */
void ServerPort_NetworkAPInit(void);
uint8_t ServerPort_NetworkSTAInit(wifi_credential_t creden);

void ServerPort_init(CustomSDPort *SDPort);
bool ServerPort_ready(void);
void ServerPort_SetNetworkSleep(void);
/* Request a runtime maintenance AP without rebooting or changing NVS. */
void ServerPort_EnterMaintenanceAp(void);

uint8_t Get_NetworkMode(void);
void Mdns_init_config(void);

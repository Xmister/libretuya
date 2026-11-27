/* BL602 flash chip ID for the common LibreTiny flash API (lt_flash_get_size() uses FLASH_LENGTH when the board sets it). */

#include <FreeRTOS.h>
#include <bl602_xip_sflash.h>
#include <bl_flash.h>
#include <libretiny.h>
#include <task.h>

lt_flash_id_t lt_flash_get_id() {
	lt_flash_id_t id = {0};
	uint8_t data[4]	 = {0};
	uint8_t aes		 = 0;
	// Same pattern as the SDK's own flash access: the *_Need_Lock routine leaves XIP, sends the command and restores
	// XIP; nothing else may fetch from flash meanwhile.
	taskENTER_CRITICAL();
	XIP_SFlash_Opt_Enter(&aes);
	XIP_SFlash_GetJedecId_Need_Lock((SPI_Flash_Cfg_Type *)bl_flash_get_flashCfg(), data);
	XIP_SFlash_Opt_Exit(aes);
	taskEXIT_CRITICAL();
	id.manufacturer_id = data[0];
	id.chip_id		   = data[1];
	id.chip_size_id	   = data[2];
	return id;
}

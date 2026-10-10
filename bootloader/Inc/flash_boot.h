#ifndef __FLASH_BOOT_H__
#define __FLASH_BOOT_H__

#include <stdint.h>
#include "boot_cfg.h"


/* 返回值统一约定：0 = 成功，非 0 = 失败 */
int  FLASH_Unlock(void);
void FLASH_Lock(void);

/* 地址 → 扇区号（0~11）；地址不在 Flash 范围返回 0xFFFFFFFF */
uint32_t FLASH_SectorOf(uint32_t addr);
uint32_t FLASH_SectorAddr(uint32_t sector);
uint32_t FLASH_SectorSize(uint32_t sector);

/* 擦除一段地址区间（内部自动按扇区拆，不会多擦一个字节） */
int  FLASH_EraseRange(uint32_t addr, uint32_t len);

/* 写入。内部按 4 字节对齐批量写，尾巴不足 4 字节的用字节写。
   ★ 目标区域必须已经擦成 0xFF，写只能把 1 改成 0 */
int  FLASH_Write(uint32_t addr, const uint8_t *data, uint32_t len);

/* ---------------- 启动标志（乒乓双份，S2 主 + S3 备） ----------------
   写：先擦备→写备→再擦主→写主。
       任何时刻掉电，至少有一份是完整的（旧的或新的），Boot 不会瞎跳。
   读：先读主，魔数/CRC 不对就退到备份；两份都坏返回非 0。 */
int  FLASH_ReadBootFlag(BootFlag_t *flag);
int  FLASH_WriteBootFlag(const BootFlag_t *flag);

#endif /* __FLASH_BOOT_H__ */

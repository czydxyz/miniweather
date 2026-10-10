#ifndef __FLASH_IF_H__
#define __FLASH_IF_H__

#include "main.h"           /* flash_if.c 靠它间接拿到 HAL 的 Flash 头文件 */
#include <stdint.h>

/* ★ 共享契约：分区地址 / BootFlag_t / 魔数 / UPGRADE_* 全在这里 */
#include "flash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 地址 ↔ 扇区 ----------------
   返回扇区号 0~11；地址不在 Flash 范围内返回 0xFFFFFFFF */
uint32_t FLASH_IF_SectorOf(uint32_t addr);
uint32_t FLASH_IF_SectorAddr(uint32_t sector);   /* 扇区起始地址 */
uint32_t FLASH_IF_SectorSize(uint32_t sector);   /* 扇区字节数 */

/* ---------------- 擦 / 写 / 读 ----------------
   返回值：0 = 成功，非 0 = 失败（1=参数非法 2=擦除失败 3=写入失败 4=校验失败）
   ★ 这三个函数内部已自带 解锁...上锁，调用方不要再自己加 */
int      FLASH_IF_EraseSectorByAddr(uint32_t addr);
int      FLASH_IF_EraseRange(uint32_t addr, uint32_t len);
int      FLASH_IF_Write(uint32_t addr, const uint8_t *data, uint32_t len);
void     FLASH_IF_Read(uint32_t addr, uint8_t *data, uint32_t len);

/* ---------------- 启动标志（乒乓双份） ----------------
   写：先擦备份块 S3 → 写 S3 → 再擦主块 S2 → 写 S2。
       任何时刻掉电，至少有一份是完整的（旧的或新的），Boot 不会瞎跳。
   读：先读主块，魔数/CRC 不对就退到备份块；两份都坏返回非 0。 */
int      FLASH_IF_WriteBootFlag(const BootFlag_t *flag);
int      FLASH_IF_ReadBootFlag(BootFlag_t *flag);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_IF_H__ */

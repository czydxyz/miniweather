/* CRC32/ISO-HDLC：poly 0x04C11DB7（反射 0xEDB88320），
 * init 0xFFFFFFFF，结果异或 0xFFFFFFFF，右移
 * [约束] 算法必须与 App 侧 BSP/Src/flash_if.c 的 Crc32Calc 完全一致。
 */
#ifndef __CRC32_H__
#define __CRC32_H__

#include <stdint.h>


uint32_t CRC32_Calc(const uint8_t *data, uint32_t len);          /* 内存数据 */
uint32_t CRC32_CalcFlash(uint32_t flash_addr, uint32_t len);     /* 直接算 Flash 内容 */

#endif /* __CRC32_H__ */

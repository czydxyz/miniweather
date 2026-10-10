/* 继电器驱动（IN = PE5）
 * [约束] 必须共地。
 * [约束] 严禁接 220V 市电。
 */
#include "relay.h"


/* 软件记录当前状态（0=断开 1=吸合），避免每次都去读 GPIO */
static uint8_t s_relay_state = 0;

/**
 * @brief  继电器初始化
 * @note   CubeMX 已经把引脚配成推挽输出、默认低电平了。
 *         这里再显式关一次做双保险 —— 上电瞬间 GPIO 是高阻的，
 *         万一模块是高电平触发，就会"啪"地吸合一下。
 */
void RELAY_Init(void)
{
    RELAY_OFF();
    s_relay_state = 0;
}

/**
 * @brief  设置继电器状态
 * @param  on  1=吸合（负载通电）  0=断开
 */
void RELAY_Set(uint8_t on)
{
    if (on)
    {
        RELAY_ON();
        s_relay_state = 1;
    }
    else
    {
        RELAY_OFF();
        s_relay_state = 0;
    }
}

/** @brief  翻转继电器状态 */
void RELAY_Toggle(void)
{
    RELAY_Set(!s_relay_state);
}

/** @brief  读当前状态（1=吸合） */
uint8_t RELAY_GetState(void)
{
    return s_relay_state;
}

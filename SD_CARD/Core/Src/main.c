/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "fatfs.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usbd_cdc_if.h"
#include <string.h>
#include<stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SPI_HandleTypeDef hspi2;

/* USER CODE BEGIN PV */
uint8_t dbg_rt = 0xFF, dbg_token = 0xFF;
uint8_t sector[512];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_SPI2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define SD_CS_LOW()   HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET)
#define SD_CS_HIGH()  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET)

// 送出 1 byte，同時收回 1 byte
static uint8_t spi_xchg(uint8_t tx)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1, 100);
    return rx;
}
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc)
{
	spi_xchg(0x40|cmd); //指令編號
	spi_xchg((uint8_t)(arg>>24)); //參數最高的8位
	spi_xchg((uint8_t)(arg>>16));
	spi_xchg((uint8_t)(arg>> 8));
	spi_xchg((uint8_t)(arg));//參數最低的8位
	spi_xchg(crc); //檢查碼

	for(int i=0;i<10;i++){
		uint8_t r =spi_xchg(0xFF);//最左邊是0
		if((r&0x80)==0)return r;
	}
	return 0xFF; //10次都沒回答，回傳0xFF表示失敗
}


// 叫醒並初始化 SD 卡。成功回傳 0；失敗回傳 1~4，代表卡在第幾步
static int sd_init(void)
{
    uint8_t r, buf[4];

    SD_CS_HIGH();
    for (int i = 0; i < 10; i++) spi_xchg(0xFF);      // 搖醒

    HAL_Delay(10);                                    // 等卡的電源穩定
    for (int t = 0; t < 10; t++) {                    // CMD0：最多試 10 次
        SD_CS_LOW();
        r = sd_send_cmd(0, 0, 0x95);
        SD_CS_HIGH(); spi_xchg(0xFF);
        if (r == 0x01) break;
    }
    if (r != 0x01) return 1;

    SD_CS_LOW();                                      // CMD8：新型卡嗎？
    r = sd_send_cmd(8, 0x1AA, 0x87);
    for (int i = 0; i < 4; i++) buf[i] = spi_xchg(0xFF);
    SD_CS_HIGH(); spi_xchg(0xFF);
    if (r != 0x01 || buf[2] != 0x01 || buf[3] != 0xAA) return 2;

    for (int i = 0; i < 1000; i++) {                  // ACMD41：準備好了沒？
        SD_CS_LOW(); sd_send_cmd(55, 0, 0x01); SD_CS_HIGH(); spi_xchg(0xFF);
        SD_CS_LOW(); r = sd_send_cmd(41, 0x40000000, 0x01); SD_CS_HIGH(); spi_xchg(0xFF);
        if (r == 0x00) break;
        HAL_Delay(1);
    }
    if (r != 0x00) return 3;

    SD_CS_LOW();                                      // CMD58：大容量卡嗎？
    sd_send_cmd(58, 0, 0x01);
    for (int i = 0; i < 4; i++) buf[i] = spi_xchg(0xFF);
    SD_CS_HIGH(); spi_xchg(0xFF);
    if ((buf[0] & 0x40) == 0) return 4;

    return 0;                                         // 全部成功
}

// 讀第 n 頁，放進 dst 指向的 512 格陣列。成功回傳 0，失敗回傳 1
static int sd_read_sector(uint32_t n, uint8_t *dst)
{
    uint8_t token = 0xFF;
    SD_CS_LOW();
    if (sd_send_cmd(17, n, 0x01) == 0x00) {
        for (int i = 0; i < 2000; i++) {              // 等公車
            token = spi_xchg(0xFF);
            if (token == 0xFE) break;
        }
        if (token == 0xFE) {                          // 上車收資料
            for (int i = 0; i < 512; i++) dst[i] = spi_xchg(0xFF);
            spi_xchg(0xFF); spi_xchg(0xFF);           // 檢查碼丟掉
        }
    }
    SD_CS_HIGH(); spi_xchg(0xFF);
    return (token == 0xFE) ? 0 : 1;
}

// 把 src 指向的 512 個位元組寫到第 n 頁。成功回傳 0，失敗回傳 1
static int sd_write_sector(uint32_t n, const uint8_t *src)
{
    int ok = 0;
    SD_CS_LOW();
    if (sd_send_cmd(24, n, 0x01) == 0x00) {
        // ① 送一個 0xFF，再送 0xFE
        spi_xchg(0xFF);
	spi_xchg(0xFE);

 	// ② 用迴圈送 512 個位元組：src[i]
        for(int i=0;i<512;i++){
		spi_xchg(src[i]);
	}
	// ③ 送兩個 0xFF（檢查碼）
        spi_xchg(0xFF);
	spi_xchg(0xFF);

	// ④ 收回應位元組：resp = spi_xchg(0xFF)
	uint8_t resp = spi_xchg(0xFF);

	// ⑤ 如果 (resp & 0x1F) == 0x05：
        //      用迴圈一直收，直到收到 0xFF（最多 100000 次）
        //      收到 0xFF 就把 ok 設成 1
	if((resp & 0x1F) == 0x05){
		for(int i=0;i<100000;i++){
			if(spi_xchg(0xFF)== 0xFF){
				ok=1;
				break;
				}
			}
		}
    }
    SD_CS_HIGH(); spi_xchg(0xFF);
    return ok ? 0 : 1;
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_SPI2_Init();
  MX_FATFS_Init();
  MX_USB_DEVICE_Init();
  /* USER CODE BEGIN 2 */
  int init_result = sd_init();

  memset(sector, 0, 512);
  strcpy((char*)sector, "Hello SD card from STM32!");
  int write_result = sd_write_sector(20000, sector);

  memset(sector, 0, 512);
  int read_result = sd_read_sector(20000, sector);   // 讀第 0 頁（答案已知，方便驗證）
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    HAL_Delay(2000);
    char line[64];

    // 先印出 rt 和 token
    int m = snprintf(line, sizeof(line), "init=%d write=%d read=%d\r\n", init_result, write_result, read_result);
    CDC_Transmit_FS((uint8_t*)line, m);
    HAL_Delay(5);

    // 再印 hex dump
    for (int row = 0; row < 32; row++) {
        int n = snprintf(line, sizeof(line), "%03X: ", row * 16);
        for (int col = 0; col < 16; col++) {
            n += snprintf(line + n, sizeof(line) - n, "%02X ", sector[row * 16 + col]);
        }
        n += snprintf(line + n, sizeof(line) - n, "\r\n");
        CDC_Transmit_FS((uint8_t*)line, n);
        HAL_Delay(5);
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI2_Init(void)
{

  /* USER CODE BEGIN SPI2_Init 0 */

  /* USER CODE END SPI2_Init 0 */

  /* USER CODE BEGIN SPI2_Init 1 */

  /* USER CODE END SPI2_Init 1 */
  /* SPI2 parameter configuration*/
  hspi2.Instance = SPI2;
  hspi2.Init.Mode = SPI_MODE_MASTER;
  hspi2.Init.Direction = SPI_DIRECTION_2LINES;
  hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi2.Init.NSS = SPI_NSS_SOFT;
  hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_128;
  hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi2.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI2_Init 2 */

  /* USER CODE END SPI2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : PA6 PA7 */
  GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : SD_CS_Pin */
  GPIO_InitStruct.Pin = SD_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(SD_CS_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

# 自我介紹
- 你的名字:一句話自我介紹
# 11/23進度
更新SD_CARD檔案
## 說明
- 開啟 USB 虛擬序列埠（CDC），STM32 可以透過 Mini USB 把文字傳到電腦
  （時鐘改用 HSE 8MHz → 168MHz，USB 48MHz）
- 用 VS Code Serial Monitor（COM 埠）查看輸出
- 把 SD 卡程式整理成三個函式：
  - `sd_init()`：初始化 SD 卡（CMD0 → CMD8 → ACMD41 → CMD58）
  - `sd_read_sector(n, buf)`：讀第 n 頁（CMD17）
  - `sd_write_sector(n, buf)`：寫第 n 頁（CMD24）
- 測試：寫入 "Hello SD card from STM32!" 到第 20000 頁，再讀回來，內容正確
- 解讀卡上資料：MBR（第 0 頁）、FAT32 開機磁區（第 8192 頁）、根目錄（第 16384 頁，目前是空的）

### 序列埠輸出格式
- `init=0 write=0 read=0`：全部成功（0 = 成功）
- `init` 不是 0：數字代表卡在第幾步（1=CMD0, 2=CMD8, 3=ACMD41, 4=CMD58）
- 下面接著印出讀到的 512 bytes（hex dump）

## 下一步
- 接上 FatFs（user_diskio.c），用檔案的方式讀寫 test.txt


# 11/16進度
上傳SD_CARD檔案
## 說明
透過SPI跟MicroSD卡溝通，完成初始化，讀出卡上第 0 頁的資料
並且透過LED閃爍呈現結果
### LED 燈號
- D2 + D3 都亮：成功
- 只有 D2：CMD8 或 ACMD41 失敗
- 只有 D3：CMD0 失敗或讀取失敗
---
陳依彤:中央資工大二

張庭瑜:化學系大二

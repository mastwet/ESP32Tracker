#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
// #include <pwm_audio.h>
#include "dac_audio.h"
#include <driver/gpio.h>
#include <math.h>
#include <esp_log.h>
#include "laamaa_-_saint_lager.h"
// #include "laamaa_-_it_is_a_synthwave.h"
// #include "herberts2.h"
// #include "8bit.h"
// #include "justice_96_remix.h"
#include "ssd1306.h"
// #include "font8x8_basic.h"
#include "vol_table.h"
#include <string.h>
#include "esp32-hal-cpu.h"
#include <Adafruit_ST7735.h>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#define MOUNT_POINT "/sdcard"

#define TFT_DC 27
#define TFT_CS 12
#define TFT_RST 33

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);   //-Just used for setup

class  aFrameBuffer : public Adafruit_GFX {
  public:
    uint16_t *buffer;
    aFrameBuffer(int16_t w, int16_t h): Adafruit_GFX(w, h)
    {
      buffer = (uint16_t*)malloc(2 * h * w);
      for (int i = 0; i < h * w; i++)
        buffer[i] = 0;
    }
    void drawPixel( int16_t x, int16_t y, uint16_t color)
    {
      if (x > 159)
        return;
      if (x < 0)
        return;
      if (y > 127)
        return;
      if (y < 0)
        return;
      buffer[x + y * _width] = color;
    }

    void display()
    {
      tft.setAddrWindow(0, 0, 160, 128);
      digitalWrite(TFT_DC, HIGH);
      digitalWrite(TFT_CS, LOW);
      SPI.beginTransaction(SPISettings(70000000, MSBFIRST, SPI_MODE0));
      for (uint16_t i = 0; i < 160 * 128; i++)
      {
        SPI.transfer16(buffer[i]);
      }
      SPI.endTransaction();
      digitalWrite(TFT_CS, HIGH);
    }
};

aFrameBuffer frame(160, 128);

#define BUFF_SIZE 2048
#define SMP_RATE 44100
#define SMP_BIT 8
int8_t buffer_ch[4][BUFF_SIZE];
int8_t buffer[BUFF_SIZE];
float time_step = 1.0 / SMP_RATE;

size_t wrin;
uint8_t stp;
bool display_stat = true;

#define CHL_NUM 4
#define TRACKER_ROW 64
uint8_t NUM_PATTERNS;
#define BUFFER_PATTERNS 2
#define NUM_ROWS 64
#define NUM_CHANNELS 4
#define PATTERN_SIZE (NUM_ROWS * NUM_CHANNELS * 4)

uint8_t part_table[128];
uint8_t part_point = 1;
int8_t tracker_point = 0;

#define BASE_FREQ 8267
bool dispRedy = false;
bool playStat = true;

const uint32_t patch_table[16] = {3546836, 3555123, 3563410, 3571697, 3579984, 3588271, 3596558, 3604845,
                         3538549, 3530262, 3521975, 3513688, 3505401, 3497114, 3488827, 3480540};

inline void hexToDecimal(uint8_t num, uint8_t *tens, uint8_t *ones) {
    *tens = (num >> 4) & 0x0F;
    *ones = num & 0x0F;
}
inline uint8_t hexToDecimalTens(uint8_t num) {
    return (num >> 4) & 0x0F;
}
inline uint8_t hexToDecimalOnes(uint8_t num) {
    return num & 0x0F;
}
inline float freq_up(float base_freq, uint8_t n) {
    return base_freq * powf(2.0f, (n / 12.0f));
}
// Unpacks 2bit DPCM data from an 8-bit variable
inline uint8_t unpackDPCM(uint8_t data, uint8_t cont) {
    return (data & (0x03 << (cont << 1))) >> (cont << 1);
}
// packs 2bit DPCM data from an 8-bit variable
inline uint8_t packDPCM(uint8_t input, uint8_t data, uint8_t cont) {
    return input |= ((data & 0x03) << (cont << 1));
}
inline int clamp(int value, int min, int max) {
    if (value < min)
        return min;
    else if (value > max)
        return max;
    else
        return value;
}
float lmt = 0;
uint16_t lmtP = 0;
float comper = 1;

void limit(float a) {
    lmt += 2 + (a > 0 ? a : -a) * 15 / 512.0f;
    lmtP++;
    comper = lmt / lmtP;
    if (lmtP >= 4096) {
        lmt = 0;
        lmtP = 0;
    }
}
// **************************INIT*END****************************

char ten[28];
int8_t vol[CHL_NUM] = {0};
int16_t period[4] = {1};
float data_index[CHL_NUM] = {0};
uint16_t data_index_int[CHL_NUM] = {0};
uint8_t smp_num[CHL_NUM] = {0};

int16_t temp;
uint16_t wave_info[33][5];
uint32_t wav_ofst[32];
uint8_t showPart = 0;
bool mute[4] = {false};

// OSC START -----------------------------------------------------
void display(void *arg) {
    SSD1306_t dev;
    i2c_master_init(&dev, 21, 22, -1);
    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_contrast(&dev, 0xff);
    // bool bs = 0;
    ssd1306_display_text(&dev, 2, "LOADING....", 14, false);
    vTaskDelay(2);
    for (;;) {
        vTaskDelay(2);
        if (dispRedy) {
            break;
        }
    }
    for (;;) {
        uint8_t x;
        uint8_t volTemp;
        uint8_t addr[4];
        for (uint8_t contr = 0; contr < 4; contr++) {
            ssd1306_clear_buffer(&dev);
            sprintf(ten, "  %2d %2d>%2d %.3f", tracker_point, showPart, part_table[showPart], comper);
            addr[0] = data_index[0] * (32.0f / wave_info[smp_num[0]][0]);
            addr[1] = data_index[1] * (32.0f / wave_info[smp_num[1]][0]);
            addr[2] = data_index[2] * (32.0f / wave_info[smp_num[2]][0]);
            addr[3] = data_index[3] * (32.0f / wave_info[smp_num[3]][0]);
            // ssd1306_display_text(&dev, 7, tet, 16, false);
            ssd1306_display_text(&dev, 0, "CH1 CH2 CH3 CH4", 16, false);
            ssd1306_display_text(&dev, 6, ten, 16, false);
            if (!mute[0]) {
            for (x = 0; x < 32; x++) {
                _ssd1306_pixel(&dev, x, ((buffer_ch[0][(x + (contr * 128)) * 2]) / 4) + 32, false);
                _ssd1306_pixel(&dev, x, (uint8_t)(period[0] * (64.0f / 743.0f))%64, false);
                // printf("DISPLAY %d\n", roundf(period[0] * (64.0f / 743.0f)));
                volTemp = (vol[0]/2) % 64;
                _ssd1306_line(&dev, addr[0], 8, addr[0], 47, false);
                for (uint8_t i = 58; i < 64; i++) {
                    _ssd1306_line(&dev, 0, i, volTemp, i, false);
                }
            }} else {
                _ssd1306_line(&dev, 0, 0, 31, 63, false);
                _ssd1306_line(&dev, 31, 0, 0, 63, false);
            }
            if (!mute[1]) {
            for (x = 32; x < 64; x++) {
                _ssd1306_pixel(&dev, x, ((buffer_ch[1][((x-32) + (contr * 128)) * 2]) / 4) + 32, false);
                _ssd1306_pixel(&dev, x, (uint8_t)(period[1] * (64.0f / 743.0f))%64, false);
                volTemp = vol[1]/2;
                _ssd1306_line(&dev, addr[1]+32, 8, addr[1]+32, 47, false);
                for (uint8_t i = 58; i < 64; i++) {
                    _ssd1306_line(&dev, 31, i, volTemp+31, i, false);
                }
            }} else {
                _ssd1306_line(&dev, 32, 0, 63, 63, false);
                _ssd1306_line(&dev, 63, 0, 32, 63, false);
            }
            if (!mute[2]) {
            for (x = 64; x < 96; x++) {
                _ssd1306_pixel(&dev, x, ((buffer_ch[2][((x-64) + (contr * 128)) * 2]) / 4) + 32, false);
                _ssd1306_pixel(&dev, x, (uint8_t)(period[2] * (64.0f / 743.0f))%64, false);
                volTemp = vol[2]/2;
                _ssd1306_line(&dev, addr[2]+64, 8, addr[2]+64, 47, false);
                for (uint8_t i = 58; i < 64; i++) {
                    _ssd1306_line(&dev, 63, i, volTemp+63, i, false);
                }
            }} else {
                _ssd1306_line(&dev, 64, 0, 95, 63, false);
                _ssd1306_line(&dev, 95, 0, 64, 63, false);
            }
            if (!mute[3]) {
            for (x = 96; x < 128; x++) {
                _ssd1306_pixel(&dev, x, ((buffer_ch[3][((x-96) + (contr * 128)) * 2]) / 4) + 32, false);
                _ssd1306_pixel(&dev, x, (uint8_t)(period[3] * (64.0f / 743.0f))%64, false);
                volTemp = vol[3]/2;
                _ssd1306_line(&dev, addr[3]+96, 8, addr[3]+96, 47, false);
                for (uint8_t i = 58; i < 64; i++) {
                    _ssd1306_line(&dev, 95, i, volTemp+95, i, false);
                }
            }} else {
                _ssd1306_line(&dev, 96, 0, 127, 63, false);
                _ssd1306_line(&dev, 127, 0, 96, 63, false);
            }
            ssd1306_show_buffer(&dev);
            // vTaskDelay(1);
        }
    }
}
// OSC END -------------------------------------------------------

void read_part_data(uint8_t* tracker_data, uint8_t pattern_index, uint16_t part_data[NUM_ROWS][NUM_CHANNELS][4]) {
//    int pattern_index = tracker_data[952 + part_number];
    uint8_t* pattern_data = tracker_data + 1084 + pattern_index * NUM_ROWS * NUM_CHANNELS * 4;

    for (int row_index = 0; row_index < NUM_ROWS; row_index++) {
        for (int channel_index = 0; channel_index < NUM_CHANNELS; channel_index++) {
            int byte_index = row_index * NUM_CHANNELS * 4 + channel_index * 4;
            uint8_t byte1 = pattern_data[byte_index];
            uint8_t byte2 = pattern_data[byte_index + 1];
            uint8_t byte3 = pattern_data[byte_index + 2];
            uint8_t byte4 = pattern_data[byte_index + 3];

            uint8_t sample_number = (byte1 & 0xF0) | (byte3 >> 4);
            uint16_t period = ((byte1 & 0x0F) << 8) | byte2;
            uint8_t effect1 = (byte3 & 0x0F);
            uint8_t effect2 = byte4;

            part_data[row_index][channel_index][0] = period;
            part_data[row_index][channel_index][1] = sample_number;
            part_data[row_index][channel_index][2] = effect1;
            part_data[row_index][channel_index][3] = effect2;
        }
    }
}

// AUDIO DATA COMP START ----------------------------------------
uint8_t arpNote[2][4] = {0};
float arpFreq[3][4];
int8_t sample1;
int8_t sample2;
int8_t make_data(uint16_t freq, uint8_t vole, uint8_t chl, bool isLoop, uint16_t loopStart, uint16_t loopLen, uint32_t smp_start, uint16_t smp_size) {
    if (vole <= 0 || freq < 0) {
        return 0;
    }
    // 更新通道的数据索引
    data_index_int[chl] = roundf(data_index[chl]);
    data_index[chl] += (float)freq / (SMP_RATE<<1);
    // 检查是否启用了循环
    if (isLoop) {
        // 如果启用了循环，则调整索引
        if (data_index_int[chl] >= (loopStart + loopLen)) {
            data_index[chl] = data_index[chl] - data_index_int[chl];
            data_index[chl] = loopStart;
        }
    } else {
        // 检查是否到达了样本的末尾
        if (data_index_int[chl] >= smp_size) {
            // 将音量标记为0并返回
            vol[chl] = 0;
            return 0;
        }
    }
    // 处理音频数据并应用音量调整
    return (int8_t)roundf((int8_t)tracker_data[data_index_int[chl] + smp_start] * vol_table[vole]);
}
// AUDIO DATA COMP END ------------------------------------------

// float frq[CHL_NUM] = {0};

uint8_t part_buffer_point = 0;
uint16_t part_buffer[BUFFER_PATTERNS][NUM_ROWS][NUM_CHANNELS][4];
bool loadOk = true;

bool skipToNextPart = false;
uint8_t skipToAnyPart = false;
/*
#define DELAY_LENGTH 6144
#define DECAY_FACTOR 0.45f

int8_t delay_buffer[DELAY_LENGTH] = {0};
uint32_t delay_index = 0;

void apply_delay(int8_t *buffer, uint16_t buf_size) {
    int8_t delayed_audio;

    for(uint16_t i = 0; i < buf_size; i++) {
        delayed_audio = delay_buffer[delay_index];
        buffer[i] += delayed_audio;
        delay_buffer[delay_index] = (int8_t)(buffer[i] * DECAY_FACTOR);
        delay_index = (delay_index + 1) % DELAY_LENGTH;
    }
}
*/
bool lcdOK = true;
uint8_t BPM = 125;
uint8_t SPD = 6;
bool keyOK = false;
bool keyUP = false;
bool keyDOWN = false;
bool keyL = false;
bool keyR = false;
int8_t ChlPos = 0;
// bool ChlMenu = false;
int8_t ChlMenuPos = 0;
bool windowsClose = true;

#define NUM_RANGES 3
#define NOTES_PER_RANGE 12
typedef struct {
    int frequency;
    char *note_name;
} Note;
Note period_table[NUM_RANGES][NOTES_PER_RANGE] = {
    // C-1 to B-1
    {{856, "C-1"}, {808, "C#1"}, {762, "D-1"}, {720, "D#1"}, {678, "E-1"}, {640, "F-1"},
        {604, "F#1"}, {570, "G-1"}, {538, "G#1"}, {508, "A-1"}, {480, "A#1"}, {453, "B-1"}},
    // C-2 to B-2
    {{428, "C-2"}, {404, "C#2"}, {381, "D-2"}, {360, "D#2"}, {339, "E-2"}, {320, "F-2"},
        {302, "F#2"}, {285, "G-2"}, {269, "G#2"}, {254, "A-2"}, {240, "A#2"}, {226, "B-2"}},
    // C-3 to B-3
    {{214, "C-3"}, {202, "C#3"}, {190, "D-3"}, {180, "D#3"}, {170, "E-3"}, {160, "F-3"},
        {151, "F#3"}, {143, "G-3"}, {135, "G#3"}, {127, "A-3"}, {120, "A#3"}, {113, "B-3"}}
};
char* findNote(int frequency) {
    for (int i = 0; i < NUM_RANGES; ++i) {
        for (int j = 0; j < NOTES_PER_RANGE; ++j) {
            if (period_table[i][j].frequency == frequency) {
                return period_table[i][j].note_name; // 找到对应的音符，返回音符名
            }
        }
    }
    return "???";
}

void drawMidRect(uint8_t w, uint8_t h, uint16_t color) {
    frame.drawRect(80-(w>>1), 64-(h>>1), (80+(w>>1))-(80-(w>>1)), (64+(h>>1))-(64-(h>>1)), color);
}

void fillMidRect(uint8_t w, uint8_t h, uint16_t color) {
    frame.fillRect(80-(w>>1), 64-(h>>1), (80+(w>>1))-(80-(w>>1)), (64+(h>>1))-(64-(h>>1)), color);
}

void setMidCusr(uint8_t w, uint8_t h, int8_t ofst) {
    frame.setCursor(80-(w>>1)+ofst, 64-(h>>1)+ofst);
}

void display_lcd(void *arg) {
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(3);
    tft.fillScreen(ST7735_BLACK);
    frame.setTextSize(1);
    frame.setTextColor(0x2945);
    frame.setCursor(2, 2);
    frame.print("ESP32Tracker");
    frame.setTextColor(0x7bcf);
    frame.setCursor(1, 1);
    frame.print("ESP32Tracker");
    frame.setCursor(100, 0);
    frame.print("libchara");
    frame.setTextColor(ST7735_WHITE);
    frame.setCursor(0, 0);
    frame.print("ESP32Tracker");
    frame.setTextColor(ST7735_WHITE);
    frame.setTextSize(0);
    frame.setTextWrap(true);
    char cbuf[24];
    uint16_t showTmpNote;
    uint8_t showTmpSamp;
    uint8_t showTmpEFX1;
    uint8_t showTmpEFX2_1;
    uint8_t showTmpEFX2_2;
    lcdOK = false;
    for (;;) {
//----------------------MAIN PAGH-------------------------
        frame.setCursor(0, 56);
        if (ChlPos == 0) {
            frame.fillRect(0, 56, 13, 72, 0x630c);
        } else {
            frame.fillRect(0, 56, 13, 72, 0x2945);
        }
        for (uint8_t x = 1; x < 5; x++) {
            if (ChlPos == x) {
                frame.fillRect(((x-1)*36)+15, 56, 33, 72, 0x630c);
            } else if (mute[x-1]) {
                frame.fillRect(((x-1)*36)+15, 56, 33, 72, 0xa514);
            } else {
                frame.fillRect(((x-1)*36)+15, 56, 33, 72, 0x2945);
            }
        }
        // frame.fillRect(0, 56, 155, 72, 0x2945);
        frame.drawRect(0, 88, 160, 8, 0x6b7e);
        // printf("%d %d\n", i, tracker_point+i);
        for (int8_t i = -4; i < 5; i++) {
            if ((tracker_point+i < 64) && (tracker_point+i >= 0)) {
                frame.setTextColor(0xf7be);
                frame.printf("%2d", tracker_point+i);
                for (uint8_t chl = 0; chl < 4; chl++) {
                    showTmpNote = part_buffer[part_buffer_point][tracker_point+i][chl][0];
                    showTmpSamp = part_buffer[part_buffer_point][tracker_point+i][chl][1];
                    showTmpEFX1 = part_buffer[part_buffer_point][tracker_point+i][chl][2];
                    showTmpEFX2_1 = hexToDecimalTens(part_buffer[part_buffer_point][tracker_point+i][chl][3]);
                    showTmpEFX2_2 = hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point+i][chl][3]);
                    frame.setCursor(frame.getCursorX()+4, frame.getCursorY());
                    if (showTmpNote) {
                        frame.setTextColor(0xa51f);
                        frame.printf("%s", findNote(showTmpNote));
                    } else {
                        frame.setTextColor(0x52aa);
                        frame.printf("...");
                    }
                    frame.setCursor(frame.getCursorX()+2, frame.getCursorY());
                    if (showTmpSamp) {
                        frame.setTextColor(0x2c2a);
                        frame.printf("%2d", showTmpSamp);
                    } else {
                        frame.setTextColor(0x52aa);
                        frame.printf("..");
                    }
                }
                frame.printf("\n");
                /*
                frame.printf("%2d %3d %2d %1X%1X%1X  %3d %2d %1X%1X%1X\n", tracker_point+i,
                    part_buffer[part_buffer_point][tracker_point+i][0][0],
                        part_buffer[part_buffer_point][tracker_point+i][0][1],
                            part_buffer[part_buffer_point][tracker_point+i][0][2],
                                hexToDecimalTens(part_buffer[part_buffer_point][tracker_point+i][0][3]),
                                hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point+i][0][3]),
                    part_buffer[part_buffer_point][tracker_point+i][1][0],
                        part_buffer[part_buffer_point][tracker_point+i][1][1],
                            part_buffer[part_buffer_point][tracker_point+i][1][2],
                                hexToDecimalTens(part_buffer[part_buffer_point][tracker_point+i][1][3]),
                                hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point+i][1][3]));
                */
            } else {
                frame.printf("\n");
            }
        }
        frame.setTextColor(ST7735_WHITE);
        frame.fillRect(1, 20, 64, 18, ST7735_BLACK);
        frame.setCursor(1, 20);
        frame.printf("BPM: %d", BPM);

        frame.setCursor(1, 29);
        frame.printf("SPD: %d", SPD);

        if (ChlMenuPos) {
            fillMidRect(90, 50, 0x528a);
            drawMidRect(90, 50, ST7735_WHITE);
            frame.setTextColor(0x7bcf);
            setMidCusr(90, 50, 3);
            frame.printf("CHL%d OPTION", ChlPos);
            frame.setTextColor(ST7735_WHITE);
            setMidCusr(90, 50, 2);
            frame.printf("CHL%d OPTION\n", ChlPos);
            setMidCusr(90, 50, 2);
            uint8_t CXTmp = frame.getCursorX()+4;
            frame.setCursor(CXTmp, frame.getCursorY()+14);
            frame.drawRect(frame.getCursorX()-2, (frame.getCursorY()-2)+((ChlMenuPos-1)*11), 72, 11, ST7735_WHITE);
            if (mute[ChlPos-1]) {
                frame.printf("unMute\n");
            } else {
                frame.printf("Mute\n");
            }
            frame.setCursor(CXTmp, frame.getCursorY()+3);
            frame.printf("CHL Editer\n");
            frame.setCursor(CXTmp, frame.getCursorY()+3);
            frame.printf("Close");
            // frame.setTextColor(0xf7be);
            if (keyUP) {
                keyUP = false;
                ChlMenuPos--;
                if (ChlMenuPos < 1) {
                    ChlMenuPos = 3;
                }
            }
            if (keyDOWN) {
                keyDOWN = false;
                ChlMenuPos++;
                if (ChlMenuPos > 3) {
                    ChlMenuPos = 1;
                }
            }
            if (keyOK) {
                keyOK = false;
                if (ChlMenuPos == 3) {
                    ChlMenuPos = 0;
                    windowsClose = true;
                }
                if (ChlMenuPos == 1) {
                    mute[ChlPos-1] = !mute[ChlPos-1];
                }
            }
        }

        if (windowsClose) {
            windowsClose = false;
            frame.fillRect(0, 10, 160, 44, ST7735_BLACK);
            frame.drawFastHLine(0, 54, 160, ST7735_WHITE);
            frame.drawFastHLine(0, 55, 160, 0xa514);
            frame.drawFastVLine(156, 56, 72, 0x4a51);
            frame.drawFastVLine(157, 56, 72, 0x6b7e);
            frame.drawFastVLine(158, 56, 72, 0xad7f);

            frame.drawFastVLine(12, 56, 72, 0x4a51);
            frame.drawFastVLine(13, 56, 72, 0x6b7e);
            frame.drawFastVLine(14, 56, 72, 0xad7f);

            frame.drawFastVLine(48, 56, 72, 0x4a51);
            frame.drawFastVLine(49, 56, 72, 0x6b7e);
            frame.drawFastVLine(50, 56, 72, 0xad7f);

            frame.drawFastVLine(84, 56, 72, 0x4a51);
            frame.drawFastVLine(85, 56, 72, 0x6b7e);
            frame.drawFastVLine(86, 56, 72, 0xad7f);

            frame.drawFastVLine(120, 56, 72, 0x4a51);
            frame.drawFastVLine(121, 56, 72, 0x6b7e);
            frame.drawFastVLine(122, 56, 72, 0xad7f);
            printf("ReDraw\n");
        }

        frame.display();
        vTaskDelay(1);
//----------------------MAIN PAGH-------------------------
//----------------------KEY STATUS------------------------
        if (!ChlMenuPos) {
            if (keyOK) {
                keyOK = false;
                if (ChlPos) {
                    ChlMenuPos = 1;
                }
            }
            if (keyUP) {
                keyUP = false;
                tracker_point--;
            }
            if (keyDOWN) {
                keyDOWN = false;
                tracker_point++;
            }
            if (keyL) {
                keyL = false;
                ChlPos--;
                if (ChlPos < 0) {
                    ChlPos = 4;
                }
            }
            if (keyR) {
                keyR = false;
                ChlPos++;
                if (ChlPos > 4) {
                    ChlPos = 0;
                }
            }
        }
    }
    vTaskDelete(NULL);
}

// COMP TASK START ----------------------------------------------
void comp(void *arg) {
    uint8_t tick_time = 0;
    uint8_t tick_speed = 6;
    uint8_t arp_p = 0;
    bool enbArp[4] = {false};
    uint8_t volUp[4] = {0};
    uint8_t volDown[4] = {0};
    uint16_t portToneSpeed[4] = {0};
    uint16_t portToneTemp[4] = {0};
    uint16_t portToneSource[4] = {0};
    uint16_t portToneTarget[4] = {0};
    bool enbPortTone[4] = {false};
    uint16_t lastNote[4] = {false};
    bool enbSlideUp[4] = {false};
    uint8_t SlideUp[4] = {false};
    bool enbSlideDown[4] = {false};
    uint8_t SlideDown[4] = {false};
    bool enbVibrato[4] = {false};
    uint8_t VibratoSpeed[4];
    uint8_t VibratoDepth[4];
    uint8_t VibratoPos[4] = {32};
    bool enbTremolo[4] = {false};
    uint8_t TremoloPos[4] = {32};
    uint8_t TremoloSpeed[4] = {32};
    uint8_t TremoloDepth[4] = {32};
    int VibratoItem[4] = {0};
    uint16_t Mtick = 0;
    uint32_t frq[4] = {0};
    uint16_t TICK_NUL = roundf((SMP_RATE<<1) / (125 * 0.4));
    uint8_t volTemp[4];
    uint16_t OfstCfg[4];
    uint8_t rowLoopStart = 0;
    int8_t rowLoopCont = 0;
    bool enbRowLoop = false;
    // uint16_t arpLastNote[4] = {0};
    /*
    pwm_audio_config_t pwm_audio_config = {
        .gpio_num_left = GPIO_NUM_26,
        .ledc_channel_left = LEDC_CHANNEL_0,
        .ledc_timer_sel = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .ringbuf_len = BUFF_SIZE
    };
    pwm_audio_init(&pwm_audio_config);
    pwm_audio_set_param(SMP_RATE, SMP_BIT, 1);
    pwm_audio_start();
    // pwm_audio_set_volume(0);
    pwm_audio_write(&buffer, BUFF_SIZE, &wrin, portMAX_DELAY);
    */
    dac_audio_config_t dac_audio_config = {
        .i2s_num = I2S_NUM_0,
        .sample_rate = SMP_RATE,
        .bits_per_sample = (i2s_bits_per_sample_t)SMP_BIT,
        .dac_mode = I2S_DAC_CHANNEL_BOTH_EN,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .max_data_size = BUFF_SIZE << 1
    };
    dac_audio_init(&dac_audio_config);
    dac_audio_set_param(SMP_RATE, SMP_BIT, 1);
    dac_audio_start();
    dac_audio_write((uint8_t*)&buffer, BUFF_SIZE, &wrin, portMAX_DELAY);
    vTaskDelay(128);
    dispRedy = true;
    uint8_t chl;
    bool enbRetrigger[4] = {false};
    // uint8_t RetriggerPos[4] = {1};
    uint8_t RetriggerConfig[4] = {0};
    int16_t audio_temp;
    while(true) { if (playStat) {
        for(uint16_t i = 0; i < BUFF_SIZE; i++) {
            for(chl = 0; chl < 4; chl++) {
                if (wave_info[smp_num[chl]][4] > 2) {
                    buffer_ch[chl][i] = make_data(frq[chl], vol[chl], chl, true, wave_info[smp_num[chl]][3]*2, wave_info[smp_num[chl]][4]*2, wav_ofst[smp_num[chl]], wave_info[smp_num[chl]][0]);
                } else {
                    buffer_ch[chl][i] = make_data(frq[chl], vol[chl], chl, false, 0, 0, wav_ofst[smp_num[chl]], wave_info[smp_num[chl]][0]);
                }
            }
            audio_temp = (mute[0] ? 0 : buffer_ch[0][i])
                            + (mute[1] ? 0 : buffer_ch[1][i])
                                + (mute[2] ? 0 : buffer_ch[2][i])
                                    + (mute[3] ? 0 : buffer_ch[3][i]);
            limit(audio_temp);
            buffer[i] = (int8_t)clamp((audio_temp / comper), -127, 126);//limit(audio_temp);
            Mtick++;
            if (Mtick == TICK_NUL) {
                Mtick = 0;
                tick_time++;
                arp_p++;
                if (tick_time != tick_speed) {
                    for(chl = 0; chl < 4; chl++) {
                        // period[chl] += enbSlideDown[chl] ? SlideDown[chl] : (enbSlideUp[chl] ? -SlideUp[chl] : 0);
                        if (enbSlideUp[chl]) {
                            // printf("SLIDE UP %d", period[chl]);
                            period[chl] -= SlideUp[chl];
                            enbPortTone[chl] = false;
                            // printf(" - %d = %d\n", SlideUp[chl], period[chl]);
                        }
                        if (enbSlideDown[chl]) {
                            // printf("SLIDE DOWN %d", period[chl]);
                            period[chl] += SlideDown[chl];
                            enbPortTone[chl] = false;
                            // printf(" + %d = %d\n", SlideDown[chl], period[chl]);
                        }
                        vol[chl] += (volUp[chl] - volDown[chl]);
                        vol[chl] = (vol[chl] > 63) ? 63 : (vol[chl] < 1) ? 0 : vol[chl];

                        VibratoItem[chl] = enbVibrato[chl] ? (sine_table[VibratoPos[chl]] * VibratoDepth[chl]) >> 7 : 0;
                        VibratoPos[chl] = enbVibrato[chl] ? (VibratoPos[chl] + VibratoSpeed[chl]) & 63 : 0;

                        if (enbTremolo[chl]) {
                            int tremoloValue = (sine_table[TremoloPos[chl]] * TremoloDepth[chl]) >> 7;
                            vol[chl] += tremoloValue;
                            vol[chl] = (vol[chl] > 64) ? 64 : (vol[chl] < 1) ? 0 : vol[chl];
                            TremoloPos[chl] = (TremoloPos[chl] + TremoloSpeed[chl]) & 63;
                        } else {
                            TremoloPos[chl] = 0;
                        }

                        if (enbPortTone[chl]) {
                            int portToneSpeedValue = portToneSpeed[chl];
                            if (portToneSource[chl] > portToneTarget[chl]) {
                                period[chl] -= portToneSpeedValue;
                                period[chl] = (period[chl] < portToneTarget[chl]) ? portToneTarget[chl] : period[chl];
                                // printf("SIL %d + %d\n", portToneTemp[chl], portToneSpeed[chl]);
                            } else if (portToneSource[chl] < portToneTarget[chl]) {
                                period[chl] += portToneSpeedValue;
                                period[chl] = (period[chl] > portToneTarget[chl]) ? portToneTarget[chl] : period[chl];
                                // printf("SID %d - %d\n", portToneTemp[chl], portToneSpeed[chl]);
                            }
                            // printf("PORTTONE %d to %d. speeed=%d\n", portToneSource[chl], portToneTarget[chl], portToneSpeed[chl]);
                        }
                    }
                } else if (tick_time == tick_speed) {
                    tick_time = 0;
                    for (chl = 0; chl < 4; chl++) {
                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 13) {
                            if (part_buffer[part_buffer_point][tracker_point][chl][3] == 0) {
                                skipToNextPart = true;
                            }
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 11) {
                            if (part_buffer[part_buffer_point][tracker_point][chl][3]) {
                                skipToAnyPart = part_buffer[part_buffer_point][tracker_point][chl][3];
                            }
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][1]) {
                            smp_num[chl] = part_buffer[part_buffer_point][tracker_point][chl][1];
                            vol[chl] = wave_info[smp_num[chl]][2];
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][0]) {
                            if (part_buffer[part_buffer_point][tracker_point][chl][2] == 3
                                || part_buffer[part_buffer_point][tracker_point][chl][2] == 5) {
                                enbPortTone[chl] = true;
                                if (part_buffer[part_buffer_point][tracker_point][chl][0]) {
                                    portToneTarget[chl] = part_buffer[part_buffer_point][tracker_point][chl][0];
                                    portToneSource[chl] = lastNote[chl];
                                    lastNote[chl] = part_buffer[part_buffer_point][tracker_point][chl][0];
                                }
                                if (enbSlideDown[chl] || enbSlideUp[chl]) {
                                    portToneSource[chl] = period[chl];
                                }
                                // printf("PT TARGET SET TO %d. SOURCE IS %d\n", portToneTarget[chl], portToneSource[chl]);
                                if ((part_buffer[part_buffer_point][tracker_point][chl][3])
                                        && (part_buffer[part_buffer_point][tracker_point][chl][2] != 5)) {
                                    portToneSpeed[chl] = part_buffer[part_buffer_point][tracker_point][chl][3];
                                }
                            } else {
                                data_index[chl] = 0;
                                lastNote[chl] = part_buffer[part_buffer_point][tracker_point][chl][0];
                                period[chl] = lastNote[chl];
                                enbPortTone[chl] = false;
                            }
                            if (!(part_buffer[part_buffer_point][tracker_point][chl][2] == 12) 
                                && part_buffer[part_buffer_point][tracker_point][chl][1]) {
                                vol[chl] = wave_info[smp_num[chl]][2];
                            }
                            if (part_buffer[part_buffer_point][tracker_point][chl][1]) {
                                smp_num[chl] = part_buffer[part_buffer_point][tracker_point][chl][1];
                            }
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 10
                                || part_buffer[part_buffer_point][tracker_point][chl][2] == 6
                                    || part_buffer[part_buffer_point][tracker_point][chl][2] == 5) {
                            if (part_buffer[part_buffer_point][tracker_point][chl][2] == 10) {
                                enbPortTone[chl] = false;
                            }
                            if (part_buffer[part_buffer_point][tracker_point][chl][2] == 5) {
                                enbPortTone[chl] = true;
                            }
                            hexToDecimal(part_buffer[part_buffer_point][tracker_point][chl][3], &volUp[chl], &volDown[chl]);
                            // printf("VOL+=%d -=%d\n", volUp[chl], volDown[chl]);
                        } else {
                            volUp[chl] = volDown[chl] = 0;
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 1) {
                            SlideUp[chl] = part_buffer[part_buffer_point][tracker_point][chl][3];
                            // printf("SET SLIDEUP IS %d\n", part_buffer[part_buffer_point][tracker_point][chl][3]);
                            enbSlideUp[chl] = true;
                        } else {
                            enbSlideUp[chl] = false;
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 2) {
                            SlideDown[chl] = part_buffer[part_buffer_point][tracker_point][chl][3];
                            // printf("SET SLIDEDOWN IS %d\n", part_buffer[part_buffer_point][tracker_point][chl][3]);
                            enbSlideDown[chl] = true;
                        } else {
                            enbSlideDown[chl] = false;
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 4
                                || part_buffer[part_buffer_point][tracker_point][chl][2] == 6) {
                            enbVibrato[chl] = true;
                            if ((part_buffer[part_buffer_point][tracker_point][chl][2] == 4)) {
                                if (hexToDecimalTens(part_buffer[part_buffer_point][tracker_point][chl][3])) {
                                    VibratoSpeed[chl] = hexToDecimalTens(part_buffer[part_buffer_point][tracker_point][chl][3]);
                                }
                                if (hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3])) {
                                    VibratoDepth[chl] = hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]);
                                }
                                // printf("VIBRATO SPD %d DPH %d\n", VibratoSpeed[chl], VibratoDepth[chl]);
                            }
                        } else {
                            enbVibrato[chl] = false;
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 7) {
                            enbVibrato[chl] = true;
                            if (hexToDecimalTens(part_buffer[part_buffer_point][tracker_point][chl][3])) {
                                TremoloSpeed[chl] = hexToDecimalTens(part_buffer[part_buffer_point][tracker_point][chl][3]);
                            }
                            if (hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3])) {
                                TremoloDepth[chl] = hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]);
                            }
                            // printf("TREMOLO SPD %d DPH %d\n", VibratoSpeed[chl], VibratoDepth[chl]);
                        } else {
                            enbTremolo[chl] = false;
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 9) {
                            OfstCfg[chl] = part_buffer[part_buffer_point][tracker_point][chl][3] << 8;
                            // printf("SMP OFST %d\n", OfstCfg[chl]);
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 12) {
                            vol[chl] = part_buffer[part_buffer_point][tracker_point][chl][3];
                            enbPortTone[chl] = false;
                        }
                        enbRetrigger[chl] = false;
                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 14) {
                            uint8_t decimalTens = hexToDecimalTens(part_buffer[part_buffer_point][tracker_point][chl][3]);
                            vol[chl] += (decimalTens == 10) ? hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]) : ((decimalTens == 11) ? -hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]) : 0);
                            vol[chl] = (vol[chl] > 64) ? 64 : ((vol[chl] < 1) ? 0 : vol[chl]);
                            period[chl] += (decimalTens == 2) ? hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]) : ((decimalTens == 1) ? -hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]) : 0);
                            if (decimalTens == 9) {
                                enbRetrigger[chl] = true;
                                RetriggerConfig[chl] = hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]);
                                // printf("RETRIGGER %d\n", RetriggerConfig[chl]);
                                volTemp[chl] = vol[chl];
                            } else if (decimalTens == 6) {
                                if (hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]) == 0) {
                                    rowLoopStart = tracker_point;
                                    // printf("SET LOOP START %d\n", rowLoopStart);
                                } else {
                                    if (rowLoopCont == 0) {
                                        rowLoopCont = hexToDecimalOnes(part_buffer[part_buffer_point][tracker_point][chl][3]);
                                        // printf("SET LOOP CONT %d\n", rowLoopCont);
                                    } else {
                                        rowLoopCont--;
                                        // printf("LOOP CONT - 1 %d\n", rowLoopCont);
                                    }
                                    if (rowLoopCont > 0) {
                                        // printf("ROWLOOP %d\n", rowLoopCont);
                                        enbRowLoop = true;
                                    }
                                }
                            }
                            // printf("LINE VOL %s TO %d\n", (decimalTens == 10) ? "UP" : ((decimalTens == 11) ? "DOWN" : "UNCHANGED"), vol[chl]);
                        }

                        if (part_buffer[part_buffer_point][tracker_point][chl][2] == 15) {
                            if (part_buffer[part_buffer_point][tracker_point][chl][3] < 32) {
                                tick_speed = part_buffer[part_buffer_point][tracker_point][chl][3];
                                SPD = part_buffer[part_buffer_point][tracker_point][chl][3];
                                // printf("SPD SET TO %d\n", tick_speed);
                            } else {
                                TICK_NUL = roundf((SMP_RATE << 1) / (part_buffer[part_buffer_point][tracker_point][chl][3] * 0.4));
                                BPM = part_buffer[part_buffer_point][tracker_point][chl][3];
                                // printf("MTICK SET TO %d\n", TICK_NUL);
                            }
                        }

                        if ((!part_buffer[part_buffer_point][tracker_point][chl][2])
                                && part_buffer[part_buffer_point][tracker_point][chl][3]) {
                            arp_p = 0;
                            hexToDecimal(part_buffer[part_buffer_point][tracker_point][chl][3], &arpNote[0][chl], &arpNote[1][chl]);
                            arpFreq[0][chl] = patch_table[wave_info[smp_num[chl]][1]] / period[chl];
                            arpFreq[1][chl] = freq_up(arpFreq[0][chl], arpNote[0][chl]);
                            arpFreq[2][chl] = freq_up(arpFreq[0][chl], arpNote[1][chl]);
                            // printf("ARP CTRL %d %d %f %f %f\n", arpNote[0][chl], arpNote[1][chl], arpFreq[0][chl], arpFreq[1][chl], arpFreq[2][chl]);
                            enbArp[chl] = true;
                        } else {
                            if (enbArp[chl]) {
                                frq[chl] = arpFreq[0][chl];
                                enbArp[chl] = false;
                            }
                        }
                        //if (chl == 3) {
                        //printf("PART=%d CHL=%d PINT=%d SPED=%d VOLE=%d VOL_F=%f FREQ=%f EFX1=%d EFX2=%d NOTE=%d SMPL=%d\n",
                        //    part_point, chl, tracker_point, tick_speed, vol[chl], vol_table[vol[chl]], frq[chl], part_buffer[part_buffer_point][tracker_point][chl][2], part_buffer[part_buffer_point][tracker_point][chl][3], part_buffer[part_buffer_point][tracker_point][chl][0], smp_num[chl]);
                        //}
                    }
                    tracker_point++;
                    if (enbRowLoop) {
                        tracker_point = rowLoopStart;
                        enbRowLoop = false;
                        printf("SKIP TO %d\n", rowLoopStart);
                    }
                    if ((tracker_point > 63) || skipToNextPart || skipToAnyPart) {
                        tracker_point = 0;
                        showPart++;
                        if (showPart >= NUM_PATTERNS) {
                            showPart = 0;
                        }
                        if (skipToAnyPart) {
                            part_point = showPart = skipToAnyPart;
                            printf("SKIP TO %d\n", part_point);
                            skipToAnyPart = false;
                            read_part_data((uint8_t*)tracker_data, part_table[part_point], part_buffer[!part_buffer_point]);
                            part_point++;
                        }
                        printf("%d\n", part_buffer_point);
                        skipToNextPart = false;
                        part_buffer_point++;
                        if (part_buffer_point > 1){
                            part_buffer_point = 0;
                        }
                        loadOk = true;
                    }
                }
                for (chl = 0; chl < 4; chl++) {
                    if (period[chl] != 0) {
                        frq[chl] = patch_table[wave_info[smp_num[chl]][1]] / (period[chl] + VibratoItem[chl]);
                    } else {
                        frq[chl] = 0;
                    }
                    if (enbRetrigger[chl]) {
                        // printf("RETPOS %d\n", RetriggerPos[chl]);
                        // if (RetriggerPos[chl] > RetriggerConfig[chl]) {
                        if (!(tick_time % RetriggerConfig[chl])) {
                            // printf("EXEC RET %d\n", RetriggerPos[chl]);
                            data_index[chl] = 0;
                            vol[chl] = volTemp[chl];
                        }
                    }
                    if (OfstCfg[chl]) {
                        data_index[chl] = OfstCfg[chl];
                        OfstCfg[chl] = 0;
                    }
                    if (arp_p > 2) {arp_p = 0;}
                    if (enbArp[chl]) {
                        frq[chl] = arpFreq[arp_p][chl];
                    }
                }
            }
        }
        // apply_delay(&buffer, BUFF_SIZE);
        dac_audio_write((uint8_t*)&buffer, BUFF_SIZE, &wrin, portMAX_DELAY);
        if (!playStat) {
            memset(&buffer, 0, BUFF_SIZE);
            memset(&buffer_ch[0], 0, BUFF_SIZE);
            memset(&buffer_ch[1], 0, BUFF_SIZE);
            memset(&buffer_ch[2], 0, BUFF_SIZE);
            memset(&buffer_ch[3], 0, BUFF_SIZE);
            vTaskDelay(32);
            dac_audio_write((uint8_t*)&buffer, BUFF_SIZE, &wrin, portMAX_DELAY);
        }
        // vTaskDelay(1);
        //ESP_LOGI("STEP_SIZE", "%d %d", wrin, BUFF_SIZE);
    } else {
        vTaskDelay(64);
    }}
}
// COMP TASK END ------------------------------------------------
FILE *f;
void load(void *arg) {
    while(true) {
        if (part_buffer_point == 0 && loadOk) {
            printf("LOADING BUF1\n");
            read_part_data((uint8_t*)tracker_data, part_table[part_point], part_buffer[1]);
            part_point++;
            if (part_point >= NUM_PATTERNS) {
                part_point = 0;
            }
            loadOk = false;
        } else if (part_buffer_point == 1 && loadOk) {
            printf("LOADING BUF0\n");
            read_part_data((uint8_t*)tracker_data, part_table[part_point], part_buffer[0]);
            part_point++;
            if (part_point >= NUM_PATTERNS) {
                part_point = 0;
            }
            loadOk = false;
        }
        vTaskDelay(64);
    }
}

void read_pattern_table() {
    NUM_PATTERNS = tracker_data[950];
    for (uint8_t i = 0; i < NUM_PATTERNS; i++) {
        part_table[i] = tracker_data[952 + i];
        printf("%d ", part_table[i]);
    }
    printf("\n");
}

int find_max(int size) {
    if (size <= 0) {
        return -1;
    }
    int max = part_table[0];
    for (int i = 1; i < size; i++) {
        if (part_table[i] > max) {
            max = part_table[i];
        }
    }
    return max;
}

void read_wave_info() {
    for (uint8_t i = 1; i < 33; i++) {
        uint8_t* sample_data = (uint8_t*)tracker_data + 20 + (i - 1) * 30;
        wave_info[i][0] = ((sample_data[22] << 8) | sample_data[23]) * 2;  // 采样长度
        wave_info[i][1] = sample_data[24];  // 微调值
        wave_info[i][2] = sample_data[25];  // 音量
        wave_info[i][3] = (sample_data[26] << 8) | sample_data[27];  // 重复点
        wave_info[i][4] = (sample_data[28] << 8) | sample_data[29];  // 重复长度
        ESP_LOGI("WAVE INFO", "NUM=%d LEN=%d PAT=%d VOL=%d LOOPSTART=%d LOOPLEN=%d TRK_MAX=%d", i, wave_info[i][0], wave_info[i][1], wave_info[i][2], wave_info[i][3], wave_info[i][4], find_max(NUM_PATTERNS)+1);
    }
}

void comp_wave_ofst() {
    wav_ofst[1] = 1084 + ((find_max(NUM_PATTERNS)+1) * 1024);
    ESP_LOGI("FIND MAX", "%d OFST %ld", find_max(NUM_PATTERNS), wav_ofst[1]);
/*
    for (uint8_t i = 2; i < 33; i++) {
        printf("%d %d\n", i, wav_ofst[i]);
        wav_ofst[i] += wave_info[i][0];
        wav_ofst[i+1] = wav_ofst[i];
    }
*/
    for (uint8_t i = 1; i < 33; i++) {
        printf("1 %ld %ld %d\n", wav_ofst[i+1], wav_ofst[i], wave_info[i+1][0]);
        wav_ofst[i+1] += (wav_ofst[i] + wave_info[i][0]);
    }
}
/*
void read_wave_data(uint8_t (*wave_info)[5], uint8_t* tracker_data, uint8_t** wave_data) {
    for (int i = 0; i < 32; i++) {
        uint16_t sample_length = wave_info[i][0] * 2;
        wave_data[i] = (uint8_t*)malloc(sample_length * sizeof(uint8_t));
        if (wave_data[i] == NULL) {
            ESP_LOGE("WAVE READ", "MEMRY MALLOC FAIL!");
            exit(EXIT_FAILURE);
        }
        for (int j = 0; j < sample_length; j++) {
            wave_data[i][j] = tracker_data[20 + i * 30 + j];
        }
    }
}
*/

void input(void *arg) {
    Serial.begin(115200);
    while (true) {
        if (Serial.available() > 0) {
            uint16_t received = Serial.read();
            printf("INPUT: %d\n", received);
            if (received == 32) {
                playStat = !playStat;
                printf("START/STOP\n");
            }
            if (received == 119) {
                keyUP = true;
                printf("UP\n");
            }
            if (received == 115) {
                keyDOWN = true;
                printf("DOWN\n");
            }
            if (received == 97) {
                keyL = true;
                printf("L\n");
            }
            if (received == 100) {
                keyR = true;
                printf("R\n");
            }
            if (received == 108) {
                keyOK = true;
                printf("PLS OK\n");
            }
            Serial.flush();
        }
        vTaskDelay(6);
    }
}

#define PIN_NUM_MOSI 13
#define PIN_NUM_MISO 32
#define PIN_NUM_CLK 14
#define PIN_NUM_CS (gpio_num_t)15

void setup()
{
    read_pattern_table();
    read_wave_info();
    comp_wave_ofst();
    uint8_t rows = 32;
    uint16_t dlen;
    // 分配每行的空间
    /*
    for (int i = 0; i < rows; i++) {
        dlen = wave_info[i+1][0];
        wave_data[i] = (int8_t *)malloc(dlen * sizeof(int8_t));
        if (wave_data[i] == NULL) {
            printf("内存分配失败！%d %d\n", i, dlen);
            // exit(1);
        }
    }
    */
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";
    printf("Initializing SD card...\n");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        printf("Failed to initialize bus. Code %d\n", ret);
    } else {
        printf("Card Mount OK");
    }
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;
    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);
    xTaskCreatePinnedToCore(&input, "input", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(&display_lcd, "tracker_ui", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(&display, "wave_view", 7000, NULL, 5, NULL, 0);
    // xTaskCreatePinnedToCore(&Cpu_task, "cpu_task", 4096, NULL, 0, NULL, 0);
/*
    for (int i = 0; i < NUM_PATTERNS; i++) {
        uint8_t* pattern_data = tracker_data + 1084 + i * PATTERN_SIZE;
        decode_pattern_data(pattern_data, part_data);
    }
*/
    // read_wave_data(wave_info, tracker_data, wave_data);
//    for (int i = 0; i <= 100; i++) {
//        midi_notes[i] = midi_note_frequency(i);
//    }
    for (uint8_t i = 1; i < 33; i++) {
        ESP_LOGI("WAVE INFO", "NUM=%d LEN=%d PAT=%d VOL=%d LOOPSTART=%d LOOPLEN=%d TRK_MAX=%d", i, wave_info[i][0], wave_info[i][1], wave_info[i][2], wave_info[i][3]*2, (wave_info[i][3]*2)+(wave_info[i][4]*2), find_max(NUM_PATTERNS)+1);
    }
    read_part_data((uint8_t*)tracker_data, part_table[0], part_buffer[0]);
    while(lcdOK) {
        vTaskDelay(4);
    }
    vTaskDelay(512);
    xTaskCreate(&comp, "Play", 9000, NULL, 6, NULL);
    xTaskCreatePinnedToCore(&load, "Load", 2048, NULL, 0, NULL, 1);
}

void loop() {
    printf("DELETE LOOP\n");
    vTaskDelete(NULL);
}
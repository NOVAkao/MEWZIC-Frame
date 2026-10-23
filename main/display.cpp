#include "display.h"
#include "config.h"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include "driver/gpio.h"
#include "dog_bitmap_296x128.h"
#include "JPEGDEC.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// External Bluetooth helpers to retrieve title, artist, and playback state
extern const char *a2dp_sink_get_title(unsigned max_len);
extern const char *a2dp_sink_get_artist(unsigned max_len);
extern bool a2dp_sink_get_playing(void);

static const char *TAG = "DISPLAY";

#define LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)

DRAM_ATTR WORD_ALIGNED_ATTR static uint8_t framebuffer[DISPLAY_BUFFER_SIZE];
static spi_device_handle_t displaySPI = nullptr;
static bool displayAvailable = false;
static bool isHibernated = false;

// Dynamic sizing based on configured cover art dimensions
#define COVER_ART_PIXEL_COUNT (DISPLAY_COVER_ART_WIDTH * DISPLAY_COVER_ART_HEIGHT)
#define COVER_ART_BITMAP_SIZE ((COVER_ART_PIXEL_COUNT + 7) / 8)

// Packed 1-bit monochrome bitmap buffer
static uint8_t coverArtBitmap[COVER_ART_BITMAP_SIZE];

static bool coverArtReady = false;
static JPEGDEC coverArtJpeg;

// Context passed to JPEGDEC draw callback
struct CoverArtDecodeContext {
    uint8_t *grayBuf;
    int decodedWidth;
    int decodedHeight;
    int targetWidth;
    int targetHeight;
};

// Callback: Decode JPEG MCU blocks directly into the grayscale array with proportional resampling
static int coverArtDrawCallback(JPEGDRAW *draw) {
    if (!draw || !draw->pPixels || !draw->pUser) return 0;

    CoverArtDecodeContext *ctx = static_cast<CoverArtDecodeContext *>(draw->pUser);
    if (!ctx->grayBuf || ctx->decodedWidth <= 0 || ctx->decodedHeight <= 0) return 0;

    const uint8_t *pixels8 = reinterpret_cast<const uint8_t *>(draw->pPixels);
    const uint16_t *pixels16 = static_cast<const uint16_t *>(draw->pPixels);
    bool isGray8 = (draw->iBpp == 8);

    for (int row = 0; row < draw->iHeight; ++row) {
        int y_src = draw->y + row;
        for (int col = 0; col < draw->iWidthUsed; ++col) {
            int x_src = draw->x + col;
            int srcIdx = row * draw->iWidth + col;

            uint8_t gray;
            if (isGray8) {
                gray = pixels8[srcIdx];
            } else {
                uint16_t pixel = pixels16[srcIdx];
                uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
                uint8_t g = ((pixel >> 5)  & 0x3F) * 255 / 63;
                uint8_t b = ( pixel        & 0x1F) * 255 / 31;
                gray = (r * 38 + g * 75 + b * 15) >> 7;
            }

            // Map source pixel coordinate proportionally into 120x120 target space
            int x_dst_start = (x_src * ctx->targetWidth) / ctx->decodedWidth;
            int x_dst_end   = ((x_src + 1) * ctx->targetWidth) / ctx->decodedWidth;
            int y_dst_start = (y_src * ctx->targetHeight) / ctx->decodedHeight;
            int y_dst_end   = ((y_src + 1) * ctx->targetHeight) / ctx->decodedHeight;

            if (x_dst_end <= x_dst_start) x_dst_end = x_dst_start + 1;
            if (y_dst_end <= y_dst_start) y_dst_end = y_dst_start + 1;

            for (int yd = y_dst_start; yd < y_dst_end; ++yd) {
                if (yd < 0 || yd >= ctx->targetHeight) continue;
                for (int xd = x_dst_start; xd < x_dst_end; ++xd) {
                    if (xd < 0 || xd >= ctx->targetWidth) continue;
                    ctx->grayBuf[yd * ctx->targetWidth + xd] = gray;
                }
            }
        }
    }
    return 1;
}

// Perform Floyd-Steinberg dithering over DISPLAY_COVER_ART_WIDTH x DISPLAY_COVER_ART_HEIGHT
static void applyFloydSteinbergDithering(const uint8_t *grayBuf) {
    if (!grayBuf) return;

    int16_t *ditherBuf = static_cast<int16_t *>(malloc(COVER_ART_PIXEL_COUNT * sizeof(int16_t)));
    if (!ditherBuf) {
        LOGE("Failed to allocate dithering buffer");
        return;
    }

    for (int i = 0; i < COVER_ART_PIXEL_COUNT; ++i) {
        ditherBuf[i] = grayBuf[i];
    }

    // Default output bitmap to White (0xFF)
    memset(coverArtBitmap, 0xFF, sizeof(coverArtBitmap));

    for (int y = 0; y < DISPLAY_COVER_ART_HEIGHT; ++y) {
        for (int x = 0; x < DISPLAY_COVER_ART_WIDTH; ++x) {
            int idx = y * DISPLAY_COVER_ART_WIDTH + x;
            int oldPixel = ditherBuf[idx];

            if (oldPixel < 0) oldPixel = 0;
            if (oldPixel > 255) oldPixel = 255;

            int newPixel = (oldPixel >= 128) ? 255 : 0;
            int quantError = oldPixel - newPixel;

            // Pack bit: 0 = Black, 1 = White (MSB-first)
            if (newPixel == 0) {
                coverArtBitmap[idx >> 3] &= static_cast<uint8_t>(~(0x80 >> (idx & 7)));
            }

            // Floyd-Steinberg Error Distribution
            if (x + 1 < DISPLAY_COVER_ART_WIDTH) {
                ditherBuf[idx + 1] += (quantError * 7) / 16;
            }
            if (y + 1 < DISPLAY_COVER_ART_HEIGHT) {
                if (x > 0) {
                    ditherBuf[idx + DISPLAY_COVER_ART_WIDTH - 1] += (quantError * 3) / 16;
                }
                ditherBuf[idx + DISPLAY_COVER_ART_WIDTH] += (quantError * 5) / 16;
                if (x + 1 < DISPLAY_COVER_ART_WIDTH) {
                    ditherBuf[idx + DISPLAY_COVER_ART_WIDTH + 1] += (quantError * 1) / 16;
                }
            }
        }
    }

    free(ditherBuf);
}

static void delayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static bool busy() { return gpio_get_level((gpio_num_t)DISPLAY_BUSY) == DISPLAY_BUSY_ACTIVE_LEVEL; }
static bool waitReady() {
    uint32_t elapsed = 0;
    while (busy()) {
        if (elapsed >= DISPLAY_BUSY_TIMEOUT_MS) { LOGW("BUSY timeout"); return false; }
        delayMs(DISPLAY_BUSY_POLL_INTERVAL_MS); elapsed += DISPLAY_BUSY_POLL_INTERVAL_MS;
    }
    return true;
}

static esp_err_t spiWrite(const uint8_t *data, size_t len) {
    if (!displaySPI || !data || !len) return ESP_ERR_INVALID_ARG;
#if WOKWI_SIMULATION
    for (size_t i = 0; i < len; ++i) {
        spi_transaction_t byteTransaction = {};
        byteTransaction.length = 8;
        byteTransaction.tx_buffer = data + i;
        esp_err_t e = spi_device_transmit(displaySPI, &byteTransaction);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
#else
    size_t offset = 0;
    const size_t max_dma_chunk = 4092;
    while (offset < len) {
        size_t chunk = (len - offset > max_dma_chunk) ? max_dma_chunk : (len - offset);
        spi_transaction_t t = {};
        t.length = chunk * 8;
        t.tx_buffer = data + offset;
        esp_err_t e = spi_device_transmit(displaySPI, &t);
        if (e != ESP_OK) return e;
        offset += chunk;
    }
    return ESP_OK;
#endif
}

static bool cmd(uint8_t c) { gpio_set_level((gpio_num_t)DISPLAY_DC,0); return spiWrite(&c,1)==ESP_OK; }
static bool data(const uint8_t *p,size_t n) { gpio_set_level((gpio_num_t)DISPLAY_DC,1); return spiWrite(p,n)==ESP_OK; }
static bool data8(uint8_t v) { return data(&v,1); }
static void clearFB(){ memset(framebuffer,0xFF,sizeof(framebuffer)); }

static void resetDisplay(){
    gpio_set_level((gpio_num_t)DISPLAY_RST,1); delayMs(DISPLAY_RESET_HIGH_MS);
    gpio_set_level((gpio_num_t)DISPLAY_RST,0); delayMs(DISPLAY_RESET_LOW_MS);
    gpio_set_level((gpio_num_t)DISPLAY_RST,1); delayMs(DISPLAY_RESET_RELEASE_MS);
    delayMs(DISPLAY_POST_RESET_DELAY_MS); waitReady();
}

static bool setWindow(){
    if(!cmd(EPD_CMD_SET_RAM_X_START_END)) return false;
    if(!data8(0x00) || !data8(0x0F)) return false;
    if(!cmd(EPD_CMD_SET_RAM_Y_START_END)) return false;
    if(!data8(0x00) || !data8(0x00) || !data8(0x27) || !data8(0x01)) return false;
    return true;
}

static bool setCursor(){
    if(!cmd(EPD_CMD_SET_RAM_X_COUNTER) || !data8(0x00)) return false;
    if(!cmd(EPD_CMD_SET_RAM_Y_COUNTER) || !data8(0x00) || !data8(0x00)) return false;
    return true;
}

static bool initSSD1680Registers(void) {
    resetDisplay();
    if(!cmd(EPD_CMD_SOFTWARE_RESET)) return false;
    delayMs(DISPLAY_POST_SOFTWARE_RESET_DELAY_MS);
    if(!waitReady()) return false;
    if(!cmd(EPD_CMD_DRIVER_OUTPUT_CONTROL)||!data8(EPD_DRIVER_OUTPUT_0)||!data8(EPD_DRIVER_OUTPUT_1)||!data8(EPD_DRIVER_OUTPUT_2)) return false;
    if(!cmd(EPD_CMD_DATA_ENTRY_MODE)||!data8(EPD_DATA_ENTRY_MODE)) return false;
    if(!setWindow()) return false;
    if(!cmd(EPD_CMD_DISPLAY_UPDATE_CTRL_1)||!data8(EPD_DISPLAY_UPDATE_CTRL_1_A)||!data8(EPD_DISPLAY_UPDATE_CTRL_1_B)) return false;
    if(!cmd(EPD_CMD_BORDER_WAVEFORM)||!data8(EPD_BORDER_WAVEFORM)) return false;
    if(!cmd(0x18)||!data8(0x80)) return false;
    isHibernated = false;
    return true;
}

static bool ensureAwake(void) {
    if (!isHibernated) return true;
    return initSSD1680Registers();
}

static void hibernate(void) {
    if (isHibernated) return;
    if (cmd(EPD_CMD_DEEP_SLEEP) && data8(0x01)) {
        isHibernated = true;
    } else {
        LOGW("Hibernate command failed");
    }
}

static bool writeFB(){
    if(!ensureAwake()) return false;
    return setWindow() && setCursor() && cmd(EPD_CMD_WRITE_RAM) && data(framebuffer, sizeof(framebuffer));
}

static bool refresh(){
    if(!waitReady()) return false;
    if(!cmd(EPD_CMD_DISPLAY_UPDATE_CTRL_2) || !data8(EPD_FULL_REFRESH_CONTROL) || !cmd(EPD_CMD_MASTER_ACTIVATION)) return false;
    delayMs(10);
    bool ok = waitReady();
    hibernate();
    return ok;
}

static void px(int x,int y,bool colored){
    if(x<0||x>=DISPLAY_WIDTH||y<0||y>=DISPLAY_HEIGHT)return;
    int cx=DISPLAY_HEIGHT-1-y, cy=x;
    size_t idx=(size_t)cy*16+(cx/8);
    if (idx >= sizeof(framebuffer)) {
        LOGW("px out of bounds: x=%d y=%d idx=%u", x, y, idx);
        return;
    }
    uint8_t mask=(uint8_t)(0x80>>(cx&7));
    if(colored) framebuffer[idx]&=(uint8_t)~mask; else framebuffer[idx]|=mask;
}

static void rect(int x,int y,int w,int h){ for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)px(xx,yy,true); }
static const uint8_t seg[10]={0b1111110,0b0110000,0b1101101,0b1111001,0b0110011,0b1011011,0b1011111,0b1110000,0b1111111,0b1111011};

static void digit(int d,int x,int y,int s){
    if(d<0||d>9) return;
    uint8_t q=seg[d]; int w=5*s,h=9*s,t=s;
    if(q&64){ rect(x+s,y,w-2*s,t); }
    if(q&32){ rect(x+w-t,y+s,t,h/2-s); }
    if(q&16){ rect(x+w-t,y+h/2,t,h/2-s); }
    if(q&8){ rect(x+s,y+h-t,w-2*s,t); }
    if(q&4){ rect(x,y+h/2,t,h/2-s); }
    if(q&2){ rect(x,y+s,t,h/2-s); }
    if(q&1){ rect(x+s,y+h/2-t/2,w-2*s,t); }
}

static void colon(int x,int y,int s){rect(x,y+2*s,s,s);rect(x,y+6*s,s,s);}

static const uint8_t font5x7[36][5] = {
 {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x62,0x51,0x49,0x49,0x46},{0x22,0x49,0x49,0x49,0x36},{0x18,0x14,0x12,0x7F,0x10},{0x2F,0x49,0x49,0x49,0x31},{0x3E,0x49,0x49,0x49,0x32},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x26,0x49,0x49,0x49,0x3E},
 {0x7E,0x09,0x09,0x09,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};

static int glyphIndex(char c){ c=(char)toupper((unsigned char)c); if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='Z')return 10+(c-'A'); return -1; }

static int textWidth(const char *s,int scale,int maxChars){
    int width=0, count=0;
    for(;*s && count<maxChars;s++,count++) width += 6*scale;
    return width ? width-scale : 0;
}

static void text5x7(const char *s,int x,int y,int scale,int maxChars){
    int count=0; for(;*s && count<maxChars;s++,count++){
        char c=*s; if(c==' '){x+=6*scale;continue;}
        int gi=glyphIndex(c); if(gi<0){x+=6*scale;continue;}
        for(int col=0;col<5;col++) for(int row=0;row<7;row++) if(font5x7[gi][col]&(1<<row)) for(int yy=0;yy<scale;yy++)for(int xx=0;xx<scale;xx++)px(x+col*scale+xx,y+row*scale+yy,true);
        x+=6*scale; if(x>=DISPLAY_WIDTH-5*scale)break;
    }
}

static void centeredText(const char *s,int y,int scale,int maxChars){
    text5x7(s,(DISPLAY_WIDTH-textWidth(s,scale,maxChars))/2,y,scale,maxChars);
}

void displayInit(void){
    LOGI("Init: %dx%d, SPI MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BUSY=%d", DISPLAY_WIDTH, DISPLAY_HEIGHT,
         DISPLAY_MOSI, DISPLAY_SCLK, DISPLAY_CS, DISPLAY_DC, DISPLAY_RST, DISPLAY_BUSY);
    gpio_config_t out={}; out.pin_bit_mask=(1ULL<<DISPLAY_CS)|(1ULL<<DISPLAY_DC)|(1ULL<<DISPLAY_RST); out.mode=GPIO_MODE_OUTPUT; gpio_config(&out);
    gpio_config_t in={}; in.pin_bit_mask=(1ULL<<DISPLAY_BUSY); in.mode=GPIO_MODE_INPUT; in.pull_up_en=GPIO_PULLUP_ENABLE; gpio_config(&in);
    gpio_set_level((gpio_num_t)DISPLAY_CS,1);gpio_set_level((gpio_num_t)DISPLAY_DC,1);gpio_set_level((gpio_num_t)DISPLAY_RST,1);
    spi_bus_config_t bus={}; bus.mosi_io_num=DISPLAY_MOSI;bus.miso_io_num=-1;bus.sclk_io_num=DISPLAY_SCLK;bus.quadwp_io_num=-1;bus.quadhd_io_num=-1;bus.max_transfer_sz=DISPLAY_SPI_MAX_TRANSFER_SIZE;
    esp_err_t e=spi_bus_initialize(DISPLAY_SPI_HOST,&bus,SPI_DMA_CH_AUTO); if(e!=ESP_OK&&e!=ESP_ERR_INVALID_STATE){LOGE("SPI bus: %s",esp_err_to_name(e));return;} LOGI("SPI bus ready");
    spi_device_interface_config_t dev={};dev.clock_speed_hz=DISPLAY_SPI_CLOCK_HZ;dev.mode=0;dev.spics_io_num=DISPLAY_CS;dev.queue_size=DISPLAY_SPI_QUEUE_SIZE;
    e=spi_bus_add_device(DISPLAY_SPI_HOST,&dev,&displaySPI);if(e!=ESP_OK){LOGE("SPI device: %s",esp_err_to_name(e));return;} LOGI("SPI device ready");
    LOGI("Resetting e-paper; BUSY=%d", gpio_get_level((gpio_num_t)DISPLAY_BUSY));
    
    if(!initSSD1680Registers()){LOGE("SSD1680 init failed");return;}
    clearFB();
    setWindow(); setCursor(); cmd(EPD_CMD_WRITE_RAM); data(framebuffer, sizeof(framebuffer));
    setWindow(); setCursor(); cmd(0x26); data(framebuffer, sizeof(framebuffer));
    if(!refresh()){LOGE("Initial refresh failed");return;}
    displayAvailable=true;LOGI("E-paper ready");
}

void displayClock(int hour,int minute,int day,int month,int year,int dayOfWeek){
    (void)year;if(!displayAvailable)return;clearFB();
    int scale=CLOCK_SCALE,dw=CLOCK_DIGIT_WIDTH,sp=CLOCK_DIGIT_SPACING;int total=dw*4+sp*3+scale;int sx=(DISPLAY_WIDTH-total)/2,sy=CLOCK_START_Y;
    digit(hour/10,sx,sy,scale);digit(hour%10,sx+dw+sp,sy,scale);int cx=sx+dw*2+sp*2;colon(cx,sy+20,scale);int mx=cx+CLOCK_COLON_X_OFFSET;digit(minute/10,mx,sy,scale);digit(minute%10,mx+dw+sp,sy,scale);
    char date[16]; snprintf(date,sizeof(date),"%02d/%02d/%04d",day,month,year);
    centeredText(date,DATE_Y - 14, DATE_SCALE, 15);
    static const char *days[] = {"SUNDAY","MONDAY","TUESDAY","WEDNESDAY","THURSDAY","FRIDAY","SATURDAY"};
    int dayIndex = (dayOfWeek >= 1 && dayOfWeek <= 7) ? (dayOfWeek - 1) : ((dayOfWeek >= 0 && dayOfWeek <= 6) ? dayOfWeek : 0);
    centeredText(days[dayIndex],DATE_Y, 1, 10);
    if(!writeFB()||!refresh())LOGW("Clock refresh failed");
}

static void drawBitmap1bit(int x0, int y0, int w, int h, const uint8_t *bits) {
    if (!bits) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int byteIdx = (y * w + x) >> 3;
            int bitPos  = 7 - ((y * w + x) & 7);
            bool isBlack = !((bits[byteIdx] >> bitPos) & 1);
            if (isBlack) px(x0 + x, y0 + y, true);
        }
    }
}

static void drawPlayIcon(int x, int y, int w, int h) {
    int mid = h / 2;
    for (int row = 0; row < h; row++) {
        int dist = (row <= mid) ? (mid - row) : (row - mid);
        int lit = mid + 1 - dist;
        if (lit < 1) lit = 1;
        for (int col = 0; col < lit; col++) {
            px(x + col, y + row, true);
        }
    }
}

static void drawPauseIcon(int x, int y, int w, int h) {
    int barW = w / 3;
    if (barW < 2) barW = 2;
    int gap  = w - 2 * barW;
    if (gap < 2) gap = 2;
    rect(x, y, barW, h);
    rect(x + barW + gap, y, barW, h);
}

void displayMediaWithArt(const char *title, const char *artist, bool playing,
                         const uint8_t *artBitmap, unsigned artW, unsigned artH) {
    if (!displayAvailable) return;
    clearFB();

    int artX = DISPLAY_COVER_ART_X;
    int artY = DISPLAY_COVER_ART_Y;
    int expectedArtW = DISPLAY_COVER_ART_WIDTH;
    int expectedArtH = DISPLAY_COVER_ART_HEIGHT;
    int artDrawW = (artW > 0) ? (int)artW : expectedArtW;
    int artDrawH = (artH > 0) ? (int)artH : expectedArtH;

    if (artBitmap && artW > 0 && artH > 0) {
        drawBitmap1bit(artX, artY, artDrawW, artDrawH, artBitmap);
    }

    int textX = artX + artDrawW + DISPLAY_COVER_ART_PADDING + 4;
    int textW = DISPLAY_WIDTH - textX - 4;
    if (textW < 32) textW = 32;

    char titleLine[128];
    strncpy(titleLine, title ? title : "", sizeof(titleLine) - 1);
    titleLine[sizeof(titleLine) - 1] = '\0';
    int titleY = MEDIA_TITLE_Y;
    int lineH  = 7 * MEDIA_FONT_SCALE + 4;
    int charsPerLine = textW / (6 * MEDIA_FONT_SCALE);
    if (charsPerLine < 1) charsPerLine = 1;
    int len = (int)strlen(titleLine);
    int y = titleY;
    for (int i = 0; i < len && y < DISPLAY_HEIGHT - lineH; i += charsPerLine, y += lineH) {
        char buf[64];
        int chunk = len - i;
        if (chunk > charsPerLine) chunk = charsPerLine;
        memcpy(buf, titleLine + i, chunk);
        buf[chunk] = '\0';
        text5x7(buf, textX, y, MEDIA_FONT_SCALE, chunk);
    }

    text5x7(artist ? artist : "", textX, MEDIA_ARTIST_Y, 2, (textW - 4) / 12);

    int iconSize = 20;
    int iconX = textX + textW - iconSize;
    int iconY = DISPLAY_HEIGHT - iconSize - 4;
    if (playing) {
        drawPlayIcon(iconX, iconY, iconSize, iconSize);
    } else {
        drawPauseIcon(iconX, iconY, iconSize, iconSize);
    }

    if (!writeFB() || !refresh()) LOGW("Media+art refresh failed");
}

void displaySpotifyLogo(void){
    if(!displayAvailable) return;
    clearFB();
    int cx=DISPLAY_WIDTH/2,cy=DISPLAY_HEIGHT/2-12,r=45;
    for(int y=-r;y<=r;y++)for(int x=-r;x<=r;x++){int d=x*x+y*y;if(d>=(r-2)*(r-2)&&d<=r*r)px(cx+x,cy+y,true);}
    for(int x=-30;x<=30;x++){int y=-12+(x*x)/150;px(cx+x,cy+y,true);px(cx+x,cy+y+3,true);}for(int x=-25;x<=25;x++)px(cx+x,cy+(x*x)/160,true);for(int x=-18;x<=18;x++)px(cx+x,cy+10+(x*x)/180,true);
    text5x7("SPOTIFY",106,110,2,7);
    writeFB();refresh();
}

static bool dogPxAt(int bitmapX, int bitmapY) {
    if (bitmapX < 0 || bitmapX >= DOG_BITMAP_WIDTH) return false;
    if (bitmapY < 0 || bitmapY >= DOG_BITMAP_HEIGHT) return false;
    static const int bytesPerRow = DOG_BITMAP_WIDTH / 8;
    int byteIdx = bitmapY * bytesPerRow + (bitmapX >> 3);
    int bitPos  = 7 - (bitmapX & 0x7);
    return (dogBitmap[byteIdx] >> bitPos) & 1;
}

void displayCassette(void){
    if(!displayAvailable) return;
    clearFB();
    for (int dy = 0; dy < DISPLAY_HEIGHT; dy++) {
        for (int dx = 0; dx < DISPLAY_WIDTH; dx++) {
            bool isBlack = dogPxAt(dx, dy);
            px(dx, dy, isBlack);
        }
    }
    writeFB(); refresh();
}

void display_set_cover_art_jpeg(const uint8_t *jpeg, size_t jpeg_len) {
    coverArtReady = false;
    memset(coverArtBitmap, 0xFF, sizeof(coverArtBitmap));

    if (!jpeg || jpeg_len == 0) return;

    // Allocate temporary grayscale decoding buffer matching current target dimensions
    uint8_t *grayBuf = static_cast<uint8_t *>(malloc(COVER_ART_PIXEL_COUNT));
    if (!grayBuf) {
        LOGE("Failed to allocate gray buffer for cover art decoding");
        return;
    }
    memset(grayBuf, 0xFF, COVER_ART_PIXEL_COUNT);

    if (coverArtJpeg.openRAM(const_cast<uint8_t *>(jpeg), jpeg_len, coverArtDrawCallback)) {
        // Request direct 8-bit grayscale output for faster decoding
        coverArtJpeg.setPixelType(EIGHT_BIT_GRAYSCALE);

        int sourceWidth = coverArtJpeg.getWidth();
        int sourceHeight = coverArtJpeg.getHeight();

        // Calculate JPEGDEC hardware scale factor
        int scale = 0;
        if (sourceWidth >= DISPLAY_COVER_ART_WIDTH * 8 || sourceHeight >= DISPLAY_COVER_ART_HEIGHT * 8) {
            scale = JPEG_SCALE_EIGHTH;
        } else if (sourceWidth >= DISPLAY_COVER_ART_WIDTH * 4 || sourceHeight >= DISPLAY_COVER_ART_HEIGHT * 4) {
            scale = JPEG_SCALE_QUARTER;
        } else if (sourceWidth >= DISPLAY_COVER_ART_WIDTH * 2 || sourceHeight >= DISPLAY_COVER_ART_HEIGHT * 2) {
            scale = JPEG_SCALE_HALF;
        } else {
            scale = 0;
        }

        int scaleDiv = (scale == JPEG_SCALE_HALF) ? 2 : (scale == JPEG_SCALE_QUARTER) ? 4 : (scale == JPEG_SCALE_EIGHTH) ? 8 : 1;
        int decodedWidth = sourceWidth / scaleDiv;
        int decodedHeight = sourceHeight / scaleDiv;
        if (decodedWidth <= 0) decodedWidth = 1;
        if (decodedHeight <= 0) decodedHeight = 1;

        CoverArtDecodeContext ctx;
        ctx.grayBuf = grayBuf;
        ctx.decodedWidth = decodedWidth;
        ctx.decodedHeight = decodedHeight;
        ctx.targetWidth = DISPLAY_COVER_ART_WIDTH;
        ctx.targetHeight = DISPLAY_COVER_ART_HEIGHT;

        coverArtJpeg.setUserPointer(&ctx);

        if (coverArtJpeg.decode(0, 0, scale)) {
            applyFloydSteinbergDithering(grayBuf);
            coverArtReady = true;

            // Refresh display immediately with the new cover art and track metadata
            displayMediaWithArt(a2dp_sink_get_title(128), a2dp_sink_get_artist(128), a2dp_sink_get_playing(),
                                coverArtBitmap, DISPLAY_COVER_ART_WIDTH, DISPLAY_COVER_ART_HEIGHT);
        }

        coverArtJpeg.close();
    }

    free(grayBuf);
}

bool display_has_cover_art(void) {
    return coverArtReady;
}

unsigned display_get_cover_art_width(void) {
    return coverArtReady ? DISPLAY_COVER_ART_WIDTH : 0;
}

unsigned display_get_cover_art_height(void) {
    return coverArtReady ? DISPLAY_COVER_ART_HEIGHT : 0;
}

unsigned display_get_cover_art_data(uint8_t *buffer, unsigned bufsize) {
    if (!coverArtReady || !buffer) return 0;
    unsigned bytes = sizeof(coverArtBitmap) < bufsize ? sizeof(coverArtBitmap) : bufsize;
    memcpy(buffer, coverArtBitmap, bytes);
    return bytes;
}
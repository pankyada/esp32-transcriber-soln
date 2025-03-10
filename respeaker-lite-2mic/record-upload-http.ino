#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>

// #define I2S_WS 18
// #define I2S_SD 4
// #define I2S_SCK 22
// respeaker config
#define I2S_WS 7
#define I2S_SD 43
#define I2S_SCK 8
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE   (16000)
#define I2S_SAMPLE_BITS   (16)
#define I2S_READ_LEN      (16 * 1024)
#define RECORD_TIME       (10) // Seconds
#define I2S_CHANNEL_NUM   (1)
#define FLASH_RECORD_SIZE (I2S_CHANNEL_NUM * I2S_SAMPLE_RATE * I2S_SAMPLE_BITS / 8 * RECORD_TIME)

File file;
const char filename[] = "/recording.wav";
const int headerSize = 44;
bool isWIFIConnected;

void setup() {
    Serial.begin(115200);
    SPIFFSInit();
    i2sInit();
    xTaskCreate(wifiConnect, "wifi_Connect", 4096, NULL, 0, NULL);
}

void loop() {
    Serial.println("\n*** Starting New Recording Cycle ***");
    
    recordAudio();
    
    if (isWIFIConnected) {
        Serial.println("Wifi seems to be connected");
        uploadFile();
        deleteFile();  // Ensure file is deleted after upload
    } else {
        Serial.println("WiFi not connected! Skipping upload.");
    }
    
    Serial.println("*** Restarting Recording ***");
    delay(2000);  // Small delay before next recording cycle
}

void SPIFFSInit() {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS initialization failed!");
        while (1) yield();
    }
}

void i2sInit() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = i2s_bits_per_sample_t(I2S_SAMPLE_BITS),
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = 64,
        .dma_buf_len = 1024,
        .use_apll = 1
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,
        .data_in_num = I2S_SD
    };

    i2s_set_pin(I2S_PORT, &pin_config);
}

void i2s_adc_data_scale(uint8_t * d_buff, uint8_t* s_buff, uint32_t len)
{
    uint32_t j = 0;
    uint32_t dac_value = 0;
    for (int i = 0; i < len; i += 2) {
        dac_value = ((((uint16_t) (s_buff[i + 1] & 0xf) << 8) | ((s_buff[i + 0]))));
        d_buff[j++] = 0;
        d_buff[j++] = dac_value * 256 / 2048; // this is already
    }
}

void recordAudio() {
    SPIFFS.remove(filename);
    file = SPIFFS.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("File is not available!");
        return;
    }

    byte header[headerSize];
    wavHeader(header, FLASH_RECORD_SIZE);
    file.write(header, headerSize);

    int i2s_read_len = I2S_READ_LEN;
    int flash_wr_size = 0;
    size_t bytes_read;

    char *i2s_read_buff = (char *)calloc(i2s_read_len, sizeof(char));
    uint8_t *flash_write_buff = (uint8_t *)calloc(i2s_read_len, sizeof(char));

    Serial.println(" *** Recording Start *** ");
    while (flash_wr_size + i2s_read_len <= FLASH_RECORD_SIZE) {
        i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
        i2s_adc_data_scale(flash_write_buff, (uint8_t *)i2s_read_buff, i2s_read_len);
        file.write((const byte *)flash_write_buff, i2s_read_len);
        flash_wr_size += i2s_read_len;
        Serial.printf("Recording %u%%\n", (flash_wr_size * 100) / FLASH_RECORD_SIZE);
    }
    file.close();

    free(i2s_read_buff);
    free(flash_write_buff);
    
    Serial.println("Recording complete!");
}

void uploadFile() {
    file = SPIFFS.open(filename, FILE_READ);
    if (!file) {
        Serial.println("FILE IS NOT AVAILABLE!");
        return;
    }

    Serial.println("===> Uploading FILE to Flask Server");

    HTTPClient client;
    client.begin("http://<FILE_SERVER_HOST>:<FILE_SERVER_PORT>/upload");
    client.addHeader("Content-Type", "audio/wav");
    client.setTimeout(15000);
    
    int httpResponseCode = client.sendRequest("POST", &file, file.size());
    Serial.printf("HTTP Response Code: %d\n", httpResponseCode);

    if (httpResponseCode == 200) {
        String response = client.getString();
        Serial.println("==================== Transcription ====================");
        Serial.println(response);
        Serial.println("====================      End      ====================");
    } else {
        Serial.println("File upload failed!");
    }

    file.close();
    client.end();
}

void deleteFile() {
    Serial.println("Deleting recorded file...");
    if (SPIFFS.remove(filename)) {
        Serial.println("File deleted successfully.");
    } else {
        Serial.println("Failed to delete file.");
    }
}

void wifiConnect(void *pvParameters) {
    isWIFIConnected = false;
    char *ssid = "<YOUR_SSID>";
    char *password = "<YOUR_SSID_PASSWORD>";

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500);
        Serial.print(".");
    }
    isWIFIConnected = true;
    Serial.println("\nWiFi Connected!");
    while (true) {
        vTaskDelay(1000);
    }
}

void wavHeader(byte *header, int wavSize) {
    header[0] = 'R';
    header[1] = 'I';
    header[2] = 'F';
    header[3] = 'F';
    unsigned int fileSize = wavSize + headerSize - 8;
    header[4] = (byte)(fileSize & 0xFF);
    header[5] = (byte)((fileSize >> 8) & 0xFF);
    header[6] = (byte)((fileSize >> 16) & 0xFF);
    header[7] = (byte)((fileSize >> 24) & 0xFF);
    header[8] = 'W';
    header[9] = 'A';
    header[10] = 'V';
    header[11] = 'E';
    header[12] = 'f';
    header[13] = 'm';
    header[14] = 't';
    header[15] = ' ';
    header[16] = 0x10;
    header[17] = 0x00;
    header[18] = 0x00;
    header[19] = 0x00;
    header[20] = 0x01;
    header[21] = 0x00;
    header[22] = 0x01;
    header[23] = 0x00;
    header[24] = 0x80;
    header[25] = 0x3E;
    header[26] = 0x00;
    header[27] = 0x00;
    header[28] = 0x00;
    header[29] = 0x7D;
    header[30] = 0x01;
    header[31] = 0x00;
    header[32] = 0x02;
    header[33] = 0x00;
    header[34] = 0x10;
    header[35] = 0x00;
    header[36] = 'd';
    header[37] = 'a';
    header[38] = 't';
    header[39] = 'a';
    header[40] = (byte)(wavSize & 0xFF);
    header[41] = (byte)((wavSize >> 8) & 0xFF);
    header[42] = (byte)((wavSize >> 16) & 0xFF);
    header[43] = (byte)((wavSize >> 24) & 0xFF);
}


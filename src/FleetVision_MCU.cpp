/* 
 * Project Fleet Vision MCU
 * Author: Naumaan Sheikh
 * Date: 16 Jul, 2024
 */

#include "Particle.h"

SYSTEM_MODE(MANUAL);
SYSTEM_THREAD(ENABLED);

TCPClient client;
// Hide server IP and port number in a .gitignore file since repo is public
const char* server = "192.168.1.18";
const int port = 5555; 

const int RAW_IMAGE_SIZE = 230400; // bytes
const int BUFFER_SIZE = 230400*2;
const int TCP_CHUNK_SIZE = 65535; // max transfer size over TCP (64 kB)

// Frame buffer data
uint8_t frameBuffer1[RAW_IMAGE_SIZE];
uint8_t frameBuffer2[RAW_IMAGE_SIZE];

// BLE Connections to ESP32CAM boards.
BlePeerDevice peer1;
BlePeerDevice peer2;
BleCharacteristic imageDataChar1;
BleCharacteristic imageDataChar2;

BleAddress espCam1Address(""); //xx:xx:xx:xx:xx:xx address of ESP32CAM-1
BleAddress espCam2Address(""); //yy:yy:yy:yy:yy:yy address of ESP32CAM-2

// ensure that when scanning BLE devices that the devices are the desired ESP32CAM boards
void foundDeviceCallback(BleScanResult scanResult) {
  if (scanResult.address() == espCam1Address || scanResult.address() == espCam2Address) {
    Serial.println("Found device: " + scanResult.address().toString());

    // maybe store some extra device info if needed
  }
}

// Receive data from ESP32CAM boards
// bufferSize will likely need to be RAW_IMAGE_SIZE
void receiveBLEData(uint8_t* buffer, int bufferSize, const BleCharacteristic& imageDataChar) {
  int bytesRead = 0;
  while (bytesRead < bufferSize) {
    uint8_t chunk[512]; // 512 is MTU over BLE
    size_t length = imageDataChar.getValue(chunk, sizeof(chunk));

    if (length > 0) {
      if (bytesRead + length <= bufferSize) {
        memcpy(buffer + bytesRead, chunk, length);
        bytesRead += length;
      } else {
        Serial.println("Buffer overflow when reading BLE data.");
        break;
      }
    } else {
      Serial.println("Failed to read from BLE Characteristic");
      break;
    }
  }

  Serial.print("Received " + bytesRead);
  Serial.println(" bytes of image data.");
}

// send data to TCP server
// 'buffer' holds data that needs to be sent
void transmitTCPData(uint8_t* buffer, int bufferSize, TCPClient& client) {
  int bytesSent = 0;

  while(bytesSent < bufferSize) {
    int chunkSize = min(TCP_CHUNK_SIZE, bufferSize - bytesSent);
    client.write(buffer + bytesSent, chunkSize);
    bytesSent += chunkSize;

    // wait for server ack
    unsigned long timeout = millis();
    while (client.available() == 0) {
      delay(10);
      if (millis() - timeout > 10000) {
        Serial.println("Timed out waiting for response from server.");
        break;
      }
    }

    if (client.available() > 0) {
      char ack[4];
      client.readBytes(ack, 4); // may need to look at this
      if (strncmp(ack, "ACK", 3) != 0) {
        Serial.println("Failed to receive acknowledgement");
        break;
      }
    } else {
      Serial.println("No response from server.");
      break;
    }
  }

  // if transmission failed
  if (bytesSent < bufferSize) {
    Serial.println("Transmission to TCP server failed.");
  }

  Serial.print("Sent " + bytesSent);
  Serial.println( " bytes of image data.");
}

void setup() {
  Serial.begin(9600);
  
  // ============== BLE init ==============
  BLE.on();
  BLE.scan(foundDeviceCallback); // start BLE scan (need to make sure this works correctly)



  // Connect to ESP32CAM-1 and -2
  peer1 = BLE.connect(espCam1Address);
  peer2 = BLE.connect(espCam2Address);

  if(peer1.connected()) {
    BleUuid imageDataCharUUID1(""); // UUID of ESP32CAM-1
    peer1.getCharacteristicByUUID(imageDataChar1, imageDataCharUUID1);
    if (!imageDataChar1.valid()) {
      Serial.println("Failed to find image data characteristic for ESP32CAM-1");
      peer1.disconnect();
    }
  } else {
    Serial.println("Failed to connect to ESP32CAM-1");
  }

  if(peer2.connected()) {
    BleUuid imageDataCharUUID2(""); // UUID of ESP32CAM-2
    peer2.getCharacteristicByUUID(imageDataChar2, imageDataCharUUID2);
    if (!imageDataChar2.valid()) {
      Serial.println("Failed to find image data characteristic for ESP32CAM-2");
      peer2.disconnect();
    }
  } else {
    Serial.println("Failed to connect to ESP32CAM-2");
  }

  // ============== Connect to web server ==============
  if (client.connect(server, port)) {
    Serial.println("Connected to server");
  } else {
    Serial.println("Failed to connect to server");
  }
}

void loop() {
  // ============== BLE Connections ==============
  if(peer1.connected() && imageDataChar1.valid()) {
    receiveBLEData(frameBuffer1, RAW_IMAGE_SIZE, imageDataChar1);
  } else {
    Serial.println("Reconnecting to ESP32CAM-1...");
    peer1 = BLE.connect(espCam1Address);
    if(peer1.connected()) {
      BleUuid imageDataCharUUID1(""); // UUID of ESP32CAM-1
      peer1.getCharacteristicByUUID(imageDataChar1, imageDataCharUUID1);
    } else {
      Serial.println("Couldn't reconnect to ESP32CAM-1.");
    }
  }

  if(peer2.connected() && imageDataChar2.valid()) {
    receiveBLEData(frameBuffer2, RAW_IMAGE_SIZE, imageDataChar2);
  } else {
    Serial.println("Reconnecting to ESP32CAM-2...");
    peer2 = BLE.connect(espCam2Address);
    if(peer2.connected()) {
      BleUuid imageDataCharUUID2(""); // UUID of ESP32CAM-2
      peer2.getCharacteristicByUUID(imageDataChar2, imageDataCharUUID2);
    } else {
      Serial.println("Couldn't reconnect to ESP32CAM-2.");
    }
  }

  // combine data into one buffer
  uint8_t completeFrameBuffer[RAW_IMAGE_SIZE*2];
  memcpy(completeFrameBuffer, frameBuffer1, RAW_IMAGE_SIZE);
  memcpy(completeFrameBuffer + RAW_IMAGE_SIZE, frameBuffer2, RAW_IMAGE_SIZE);

  // ============== Web server ==============
  // send data to client over TCP
  // Can't compress any files/images on boron. If we want to compress, will need to compress on esp32 board
  // and read images with varying sizes
  if (client.connected()) {
    transmitTCPData(completeFrameBuffer, BUFFER_SIZE, client);
  } else {
    Serial.println("Failed to connect to server.");
    Serial.println("Attempting to reconnect...");

    if (client.connect(server, port)) {
      Serial.println("Recconnected to server!");
    } else {
      Serial.println("Failed to reconnect to server.");
    }
  }
  
  // delay(1000/30);
}

// Camera resolution = 320 x 240 = 76.8 pixels = 0.2304 MB
// https://www.omnicalculator.com/other/image-file-size
// Total data sent per frame = 0.2304 * 2 + 1 byte (string)
//                           = 0.4608 MB

/* 
TODO :
-> For failed transmission and connection requests, add retries
-> Return value of BLE.scan() to ensure the scan was successful
-> Handle errors/disconnections from TCP server better
-> Serial.print("" + x); may lead to memory allocation issues. better to separate into Serial.print(""); Serial.print(x);
->
*/
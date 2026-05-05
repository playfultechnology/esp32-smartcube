/**
 * ESP32-QiYi-SmartCube
 * Copyright (c) 2026 Playful Technology
 * 
 * ESP32 sketch to connect to QiYi Smart Rubik's Cube via Bluetooth LE
 * and decode notification messages containing puzzle state.
 *
 * Protocol information from https://github.com/Flying-Toast/qiyi_smartcube_protocol
 */

// INCLUDES
// ESP32 library for Bluetooth LE
#include "BLEDevice.h"
// "Crypto" library by Rhys Weatherley
#include "Crypto.h"
#include "AES.h"

// CONSTANTS
// The MAC address of the Rubik's Cube
// This can be discovered by starting a scan BEFORE touching the cube. Then twist any face
// to wake the cube up and see what new device appears
static BLEAddress *pServerAddress = new BLEAddress("CC:A3:00:00:C4:30"); // Qiyi-Cube
// The remote service we wish to connect to
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
// The characteristic of the remote service we want to track
static BLEUUID charUUID("0000fff6-0000-1000-8000-00805f9b34fb");
// AES decryption key from https://github.com/Flying-Toast/qiyi_smartcube_protocol
constexpr uint8_t aes_key[] = {0x57, 0xb1, 0xf9, 0xab, 0xcd, 0x5a, 0xe8, 0xa7, 0x9c, 0xb9, 0x8c, 0xe7, 0x57, 0x8c, 0x51, 0x08};
// Some cubisms needed to translate the cubeState definition to the usual notation etc..
constexpr char* moves[]={"L\'", "L", "R\'", "R", "D\'", "D", "U\'", "U", "F\'", "F", "B\'", "B"};
constexpr unsigned char movedefs[6][12] = // which facelets are involved in these moves? 
   {{0,3,6,18,21,24,27,30,33,47,50,53},    // L
    {8,5,2,26,23,20,35,32,29,55,52,49},    // R
    {24,25,26,15,16,17,51,52,53,42,43,44}, // D
    {20,19,18,38,37,36,47,46,45,11,10,9},  // U
    {6,7,8,9,12,15,29,28,27,44,41,38},     // F
    {2,1,0,36,39,42,33,34,35,17,14,11}};   // B
constexpr char sides[] = "LRDUFB";  // 0=orange, 1=red etc. - translate QiYi numbering to standard notation


// GLOBALS
// Have we found a cube with the right MAC address to connect to?
static boolean deviceFound = false;
// Are we currently connected to the cube?
static boolean connected = false;
// Properties of the device found via scan
static BLEAdvertisedDevice* myDevice;
// Characteristic of the connected device
static BLERemoteCharacteristic* pRemoteCharacteristic;
// The state of the cube as represented internally
unsigned char cubeState[27];
// A string representation of the side of each facelet. e.g. "UUUUUUUUURRRRRRRRRFFFFFFFFFDDDDDDDDDLLLLLLLLLBBBBBBBBB"
char cubeString[55] = {0};
// The last message ID to be received from the cube
unsigned char lastMessageID[5];
// The last message ID acknowledgment sent to the cube
unsigned char ACKdMessageID[5];
// What was the last move made
uint8_t lastMove;
// The battery level of the device
uint8_t batteryLevel;
// Object to access AES encryption/decryption methods
AES128 aes128;

// Implementation of MODBus CRC
uint16_t MODBUS_CRC16( const unsigned char *buf, unsigned int len ) { 
  static const uint16_t table[2] = { 0x0000, 0xA001 };
	uint16_t crc = 0xFFFF;
	unsigned int i = 0;
	char bit = 0;
	unsigned int Xor = 0;
	for(i=0; i<len; i++) {
		crc ^= buf[i];
		for(bit=0; bit<8; bit++) {
			Xor = crc & 0x01;
			crc >>= 1;
			crc ^= table[Xor];
		}
	}
	return crc;
}

// Internal cubeState is represented as 27 nibbles
// Convert to 54 letters for standard cube notation
void cubeState2cubeString(char* cubeString, const unsigned char* cubeState){
  for(int i=0; i<27; i++) {
    // Sides are given as nibbles, lower value first!
    cubeString[2*i]  = sides[cubeState[i]&0x0f];
    cubeString[2*i+1] = sides[cubeState[i]>>4];
  }
}

/*
 * Callback for parsing data received from cube
 */
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  // Serial.print("Data received from Cube, length: "); Serial.println(length);
  // Serial.print("Raw data from cube: ");
  // for (int i=0; i<length; i++) {Serial.print(pData[i], HEX); Serial.print(" ");  } Serial.println();

  // Decrypt message
  unsigned char plain[16];
  unsigned char cypher[16];
  unsigned char decmessage[256]={0};
    for(int block=0; block<length/16; block++){
      memcpy(cypher, pData+16*block, 16); 
      // Serial.print("cypher:");for(int i=0;i<16;i++){Serial.printf(" %02x", cypher[i]);}Serial.println();
      aes128.decryptBlock(plain,cypher);
      // Serial.print("plain:");for(int i=0;i<16;i++){Serial.printf(" %02x", plain[i]);}Serial.println();
      memcpy(decmessage+16*block, plain, 16);
    }
  // Serial.print("Decrypted message:");for(int i=0;i<length;i++){Serial.printf(" %02x", decmessage[i]);}Serial.println();
 
  // Parse elements of the message
  memcpy(lastMessageID,decmessage+2, 5);   // message type and timestamp, needed for ACK
  memcpy(cubeState,decmessage+7, 27); 
  Serial.print("lastMessageID");for(int i=0;i<5;i++)Serial.printf(" %02x", lastMessageID[i]);
  if (decmessage[2]==03){  // state change message
    lastMove=decmessage[34];
    batteryLevel=decmessage[35];
    Serial.printf(", Last move %02x/%s, batteryLevel %i\%", lastMove, moves[lastMove-1], batteryLevel);
    // Serial.printf("\nFacelets involved: %i,%i,...", movedefs[(lastMove-1)/2][0],movedefs[(lastMove-1)/2][1]);
  }
  Serial.println();
  //Serial.print("cubeState:");for(int i=0;i<27;i++){Serial.printf(" %02x", cubeState[i]);}Serial.println();
  cubeState2cubeString(cubeString, cubeState);
  Serial.printf("cubeString: %s\n", cubeString);
  if(strcmp(cubeString,"UUUUUUUUURRRRRRRRRFFFFFFFFFDDDDDDDDDLLLLLLLLLBBBBBBBBB")==0 && decmessage[2]==03)
    Serial.println("Congratulations! :-)");
}

/* 
 * The first thing sent to the cube needs to be the AppHello message as explained
 *  in https://github.com/Flying-Toast/qiyi_smartcube_protocol/tree/master
 */
bool sendAppHello() {
  // "App Hello" message template
  unsigned char message[32]="\xfe\x15\x00\x6b\x01\x00\x00\x22\x06\x00\x02\x08\x00";  // c.f. protocol

  // Bytes 14-20 are the MAC address of the cube, reversed
  uint8_t cubeaddress[6];
  memcpy(cubeaddress, pServerAddress->getNative(), 6);
  for(int i=0; i<6; i++){
    message[i+13] = cubeaddress[5-i];
  }
  
  // Calculate and append 2-byte CRC
  uint16_t crc = MODBUS_CRC16(message, 19);
  message[19] = crc & 0xff; 
  message[20] = crc >> 8;
    
  // Pad to 32 bytes
  for(int i=21; i<32; i++) { message[i] = 0; }

  // Encrypt the message
  unsigned char plain[16];
  unsigned char cypher[17] = {0}; 
  unsigned char encmessage[33] = {0};
  for(int block=0; block<2; block++) {
    memcpy(plain, message+16*block, 16);
    aes128.encryptBlock(cypher,plain);
    memcpy(encmessage+16*block, cypher, 16); 
  }
  // Send the encrypted value
  pRemoteCharacteristic->writeValue(encmessage, 32);

  return true;
}

/* 
 * ACK needs to be sent after receiving the CubeHello message from the cube
 */
bool sendACK(unsigned char messageID[5]) {
  unsigned char ackmessage[16] = {0};
  ackmessage[0] = 0xfe; 
  ackmessage[1] = 0x09; // Length always 9 for ACKs 
  memcpy(ackmessage+2, messageID, 5); // Message being ACK'ed
  // CRC
  uint16_t crc = MODBUS_CRC16(ackmessage, 7);
  ackmessage[7] = crc & 0xff; ackmessage[8]=crc >> 8;
  // Encrypt the message
  unsigned char encmessage[16] = {0};
  aes128.encryptBlock(encmessage,ackmessage);
  // Send it
  pRemoteCharacteristic->writeValue(encmessage, 16);
  return true;
}

/**
 * Callbacks for devices found via a Bluetooth scan of advertised devices
 */
class AdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  // The onResult callback is called for every advertised device found by the scan  
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // Print the MAC address of this device
    Serial.print(" - ");
    Serial.print(advertisedDevice.getAddress().toString().c_str());
    // Does this device match the MAC address we're looking for?
    // if(advertisedDevice.getAddress().equals(*pServerAddress)) {
    // SR: match on the first three bytes is enough! (Use any QiYi cube)
    if (memcmp(advertisedDevice.getAddress().getNative(), pServerAddress->getNative(), 3)==0){ // first three bytes are enough!
    // Stop scanning for further devices
      advertisedDevice.getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      deviceFound = true;
      Serial.println(F(" - Connecting!"));
    }
    else {
      Serial.println(F("... this is not the cube that we are looking for!"));
    }
  }
};

/**
 * Search for any advertised devices
 */
void scanForDevices(){
  Serial.println("Scanning for Bluetooth devices...");
  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device.  Specify that we want active scanning and start the
  // scan to run for 30 seconds.
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(30);
}

// Callbacks for device we connect to
class ClientCallbacks : public BLEClientCallbacks {
  // Called when a new connection is established
  void onConnect(BLEClient* pclient) {
    connected = true;
  }
  // Called when a connection is lost
  void onDisconnect(BLEClient* pclient) {
    connected = false;
  }
};

// Connect to the cube once we found it
bool connectToServer() {
    Serial.print(F("Creating BLE client... "));
    BLEClient* pClient = BLEDevice::createClient();
    delay(250);
    Serial.println(F("Done."));

    Serial.print(F("Assigning callbacks... "));
    pClient->setClientCallbacks(new ClientCallbacks());
    delay(250);
    Serial.println(F(" - Done."));

    Serial.print(F("Connecting to "));
    Serial.print(myDevice->getAddress().toString().c_str());
    Serial.print(F("... "));
    pClient->connect(myDevice);
    delay(250);
    Serial.println(" - Done.");
    
    // The standard MTU is 23 bytes, for messages larger than 20 bytes -> increase MTU!
    // Serial.println("Requesting larger MTU."); 
    pClient->setMTU(256);
    
// Obtain a reference to the service we are after in the remote BLE server.
    Serial.print(F("Finding service "));
    Serial.print(serviceUUID.toString().c_str());
    Serial.print(F("... "));
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    delay(250);
    if (pRemoteService == nullptr) {
      Serial.println(F("FAILED."));
      return false;
    }
    Serial.println(" - Done.");
    delay(250);

    // Obtain a reference to the characteristic in the service of the remote BLE server.
    Serial.print(F("Finding characteristic "));
    Serial.print(charUUID.toString().c_str());
    Serial.print(F("... "));
    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic == nullptr) {
      Serial.println(F("FAILED."));
      return false;
    }
    Serial.println(" - Done.");
    delay(250);
    
    Serial.print(F("Registering for notifications... "));
    if(pRemoteCharacteristic->canNotify()) {
      pRemoteCharacteristic->registerForNotify(notifyCallback);
      Serial.println(" - Done.");
    }
    else {
      Serial.println(F("FAILED."));
      return false;
    }

    Serial.println("READY!");
    return true;
}

// Initial setup
void setup() {
  // Start the serial connection to be able to track debug data
  Serial.begin(115200);
  Serial.println(__FILE__ __DATE__);
  
  // set the AES key for communication with the cube
  aes128.setKey(aes_key, 16) ;

  Serial.print("Initialising BLE...");
  BLEDevice::init("");
  delay(250);
  Serial.println(F("Done."));
}

// Main loop
void loop() {
  // If the cube has been found, connect to it and send Hello
  if (deviceFound) {
    if(!connected) {
      if(strlen(cubeString)!=0){
        Serial.println("Lost connection, resetting ESP!");
        ESP.restart();  // needed because we cannot reconnect to the sleeping cube
      }
      else {
        connectToServer();
        sendAppHello();   }
    }
  }
  else scanForDevices();
    
  // If we try to send the ACK from the callback, the system crashes, therefore send it from here
  if (memcmp(lastMessageID, ACKdMessageID, 5) != 0){ 
    sendACK(lastMessageID);  // not always necessary, but ACK can't do harm
    memcpy(ACKdMessageID, lastMessageID, 5);
  }
  
  delay(250);
  }
/**
 * ESP32-QiYi-SmartCube
 * Copyright (c) 2026 Playful Technology
 * 
 * ESP32 sketch to connect to QiYi Smart Rubik's Cube via Bluetooth LE
 * and decode notification messages containing puzzle state.
 *
 * Protocol information from https://github.com/Flying-Toast/qiyi_smartcube_protocol
 */

// ESP32 library for Bluetooth LE
#include "BLEDevice.h"
// "Crypto" library by Rhys Weatherley
#include "Crypto.h"
#include "AES.h"

// The MAC address of the Rubik's Cube
static BLEAddress *pServerAddress = new BLEAddress("CC:A3:00:00:C4:30"); // Qiyi-Cube
static BLEUUID serviceUUID("0000fff0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID("0000fff6-0000-1000-8000-00805f9b34fb");

// The key from https://github.com/Flying-Toast/qiyi_smartcube_protocol
byte aes_key[] = {0x57, 0xb1, 0xf9, 0xab, 0xcd, 0x5a, 0xe8, 0xa7, 0x9c, 0xb9, 0x8c, 0xe7, 0x57, 0x8c, 0x51, 0x08};
AES128 aes128;

// Have we found a cube with the right MAC address to connect to?
static boolean deviceFound = false;
// Are we currently connected to the cube?
static boolean connected = false;
// Properties of the device found via scan
static BLEAdvertisedDevice* myDevice;
// Characteristic of the connected device
static BLERemoteCharacteristic* pRemoteCharacteristic;

// Some cubisms needed to translate the cubeState to the usual notation etc..
const char* moves[]={"L\'", "L", "R\'", "R", "D\'", "D", "U\'", "U", "F\'", "F", "B\'", "B"};
const unsigned char movedefs[6][12]= // which facelets are involved in these moves? 
   {{0,3,6,18,21,24,27,30,33,47,50,53},    // L
    {8,5,2,26,23,20,35,32,29,55,52,49},    // R
    {24,25,26,15,16,17,51,52,53,42,43,44}, // D
    {20,19,18,38,37,36,47,46,45,11,10,9},  // U
    {6,7,8,9,12,15,29,28,27,44,41,38},     // F
    {2,1,0,36,39,42,33,34,35,17,14,11}};   // B
const char colours[]="LRDUFB";  // 0=orange, 1=red etc. - translate QiYi numbering to standard notation

char cubeString[55]={0};
unsigned char cubeState[27];
unsigned char lastMessageID[5];
unsigned char ACKdMessageID[5];
uint8_t lastMove;
uint8_t batteryLevel;

// MODBUS CRC - not the fastest algorithm but good enough for us
uint16_t MODBUS_CRC16( const unsigned char *buf, unsigned int len )
{ static const uint16_t table[2] = { 0x0000, 0xA001 };
	uint16_t crc = 0xFFFF;
	unsigned int i = 0;
	char bit = 0;
	unsigned int Xor = 0;
	for( i = 0; i < len; i++ ) {
		crc ^= buf[i];
		for( bit = 0; bit < 8; bit++ ) {
			Xor = crc & 0x01;
			crc >>= 1;
			crc ^= table[Xor];
		}
	}
	return crc;
}

// cubeState is given as 27 nibbles, we want 54 letters for the standard cube notation
void cubeState2cubeString(char* cubeString, const unsigned char* cubeState){
  for(int i=0;i<27;i++){
    // colours are given as nibbles, lower value first!
    cubeString[2*i]  =colours[cubeState[i]&0x0f];
    cubeString[2*i+1]=colours[cubeState[i]>>4];
  }
}

/*
 * Callback for parsing data received from cube
 */
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  // Serial.print("Data received from Cube, length: "); Serial.println(length);
  // Serial.print("Raw data from cube: ");
  // for (int i=0; i<length; i++) {Serial.print(pData[i], HEX); Serial.print(" ");  } Serial.println();

  // decrypt message
  unsigned char plain[16];
  unsigned char cypher[16];
  unsigned char decmessage[256]={0};
    for(int block=0;block<length/16;block++){
      memcpy(cypher, pData+16*block, 16); 
      // Serial.print("cypher:");for(int i=0;i<16;i++){Serial.printf(" %02x", cypher[i]);}Serial.println();
      aes128.decryptBlock(plain,cypher);
      // Serial.print("plain:");for(int i=0;i<16;i++){Serial.printf(" %02x", plain[i]);}Serial.println();
      memcpy(decmessage+16*block, plain, 16);
    }
  //Serial.print("Decrypted message:");for(int i=0;i<length;i++){Serial.printf(" %02x", decmessage[i]);}Serial.println();
 
  // parse elements of the message
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
bool sendAppHello(){
    uint8_t cubeaddress[6];
    // obtain the address of the cube, reverse it for the hello message
    // memcpy(pServerAddress->getNative(), cubeaddress, 6); // this should not work, but it also does :-)
    memcpy(cubeaddress, pServerAddress->getNative(), 6);
    unsigned char message[32]="\xfe\x15\x00\x6b\x01\x00\x00\x22\x06\x00\x02\x08\x00";  // c.f. protocol
    for(int i=0;i<6;i++){message[i+13]=cubeaddress[5-i];}  // add reverse MAC to message
    uint16_t crc=MODBUS_CRC16(message,19);
    // Serial.printf("CRC is %x %x\n",crc & 0xff, crc >> 8);
    message[19]=crc & 0xff; message[20]=crc >> 8;
    for(int i=21;i<32;i++)message[i]=0;
    Serial.print("Sending App Hello message: ");
    for(int i=0;i<32;i++){Serial.printf(" %02x", message[i]);} Serial.println();
    
    // encrypt the message and send it
    unsigned char plain[16];
    unsigned char cypher[17]={0}; 
    unsigned char encmessage[33]={0};
    for(int block=0;block<2;block++){
      memcpy(plain, message+16*block, 16);
      // Serial.print("plain:");for(int i=0;i<16;i++){Serial.printf(" %02x", plain[i]);}Serial.println();
      aes128.encryptBlock(cypher,plain);
      memcpy(encmessage+16*block, cypher, 16); 
    }
    // Serial.print("After Encryption:");for(int j=0;j<32;j++){Serial.printf(" %02x", encmessage[j]);}Serial.println();
    pRemoteCharacteristic->writeValue(encmessage, 32);

    return true;
}

/* 
 * ACK needs to be sent after receiving the CubeHello message from the cube
 */
bool sendACK(unsigned char messageID[5]){
    unsigned char ackmessage[16]={0};
    ackmessage[0]=0xfe; ackmessage[1]=0x09;  // c.f. protocol 
    memcpy(ackmessage+2, messageID, 5);
    uint16_t crc=MODBUS_CRC16(ackmessage,7);
    // Serial.printf("CRC is %x %x\n",crc & 0xff, crc >> 8);
    ackmessage[7]=crc & 0xff; ackmessage[8]=crc >> 8;
    // Serial.print("Sending ACK message: "); for(int i=0;i<16;i++){Serial.printf(" %02x", ackmessage[i]);} Serial.println();
    
    // encrypt the message and send it
    unsigned char encmessage[16]={0};
    aes128.encryptBlock(encmessage,ackmessage);
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
    
    /* if(pRemoteCharacteristic->canRead()) {
      Serial.print("Initial characteristic value: ");
      Serial.println(pRemoteCharacteristic->readValue());
    } */
    
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

/**
 * ESP32-SmartCube
 * Copyright (c) 2019 Playful Technology
 * 
 * ESP32 sketch to connect to Xiaomi "Smart Magic Cube" Rubik's Cube via Bluetooth LE
 * and decode notification messages containing puzzle state.
 * Prints output to serial connection of the last move made, and when the cube is fully
 * solved, triggers a relay output
 */

// INCLUDES
// ESP32 library for Bluetooth LE
#include "BLEDevice.h"

// CONSTANTS
// The MAC address of the Rubik's Cube
// This can be discovered by starting a scan BEFORE touching the cube. Then twist any face
// to wake the cube up and see what new device appears
static BLEAddress *pServerAddress = new BLEAddress("d9:47:6f:3b:f4:e1");
// The remote service we wish to connect to
static BLEUUID serviceUUID("0000aadb-0000-1000-8000-00805f9b34fb");
// The characteristic of the remote service we want to track
static BLEUUID charUUID("0000aadc-0000-1000-8000-00805f9b34fb");
// The following constants are used to decrypt the data representing the state of the cube
// see https://github.com/cs0x7f/cstimer/blob/master/src/js/bluetooth.js
const uint8_t decryptionKey[] = {176, 81, 104, 224, 86, 137, 237, 119, 38, 26, 193, 161, 210, 126, 150, 81, 93, 13, 236, 249, 89, 235, 88, 24, 113, 81, 214, 131, 130, 199, 2, 169, 39, 165, 171, 41};
// This pin will have a HIGH pulse sent when the cube is solved
const byte relayPin = 33;
// This is the data array representing a solved cube
const byte solution[16] = {0x12,0x34,0x56,0x78,0x33,0x33,0x33,0x33,0x12,0x34,0x56,0x78,0x9a,0xbc,0x00,0x00};

// GLOBALS
// Have we found a cube with the right MAC address to connect to?
static boolean deviceFound = false;
// Are we currently connected to the cube?
static boolean connected = false;
// Properties of the device found via scan
static BLEAdvertisedDevice* myDevice;
// BT characteristic of the connected device
static BLERemoteCharacteristic* pRemoteCharacteristic;

// HELPER FUNCTIONS
/**
 * Return the ith bit from an integer array
 */
int getBit(uint8_t* val, int i) {
  int n = ((i / 8) | 0);
  int shift = 7 - (i % 8);
  return (val[n] >> shift) & 1;    
}

/**
 * Return the ith nibble (half-byte, i.e. 16 possible values)
 */
uint8_t getNibble(uint8_t val[], int i) {
  if(i % 2 == 1) {
    return val[(i/2)|0] % 16;
  }
  return 0|(val[(i/2)|0] / 16);
}

// CALLBACKS
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
    if(advertisedDevice.getAddress().equals(*pServerAddress)) {
      // Stop scanning for further devices
      advertisedDevice.getScan()->stop();
      // Create a new device based on properties of advertised device
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      // Set flag
      deviceFound = true;
      Serial.println(F(" - Connecting!"));
    }
    else {
      Serial.println(F("... MAC address does not match"));
    }
  }
};

/**
 * Callbacks for device we connect to
 */
class ClientCallbacks : public BLEClientCallbacks {
  // Called when a new connection is established
  void onConnect(BLEClient* pclient) {
    digitalWrite(LED_BUILTIN, HIGH);
    connected = true;
  }
  // Called when a connection is lost
  void onDisconnect(BLEClient* pclient) {
    digitalWrite(LED_BUILTIN, LOW);
    connected = false;
  }
};

/**
 * Called whenever a notication is received that the tracked BLE characterisic has changed
 */
static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  // DECRYPT DATA
  // Early Bluetooth cubes used an unencrypted data format that sent the corner/edge indexes as a 
  // simple raw string, e.g. pData for a solved cube would be 1234567833333333123456789abc000041414141
  // However, newer cubes e.g. Giiker i3s encrypt data with a rotating key, so that same state might be
  // 706f6936b1edd1b5e00264d099a4e8a19d3ea7f1 then d9f67772c3e9a5ea6e84447abb527156f9dca705 etc.
  
  // To find out whether the data is encrypted, we first read the penultimate byte of the characteristic data. 
  // As in the two examples above, if this is 0xA7, we know it's encrypted
  bool isEncrypted = (pData[18] == 0xA7);

  // If it *is* encrypted...
  if(isEncrypted) {
    // Split the last byte into two 4-bit values 
    int offset1 = getNibble(pData, 38);
    int offset2 = getNibble(pData, 39);

    // Retrieve a pair of offset values from the decryption key 
    for (int i=0; i<20; i++) {
      // Apply the offset to each value in the data
      pData[i] += (decryptionKey[offset1 + i] + decryptionKey[offset2 + i]);
    }
  }

  // First 16 bytes represent state of the cube - 8 corners (with 3 orientations), and 12 edges (can be flipped)
  Serial.print("Current State: ");
    for (int i=0; i<16; i++) {
      Serial.print(pData[i], HEX);
      Serial.print(" ");
    }
  Serial.println("");

  // Byte 17 represents the last twist made - first half-byte is face, and second half-byte is direction of rotation
  int lastMoveFace = getNibble(pData, 32);
  int lastMoveDirection = getNibble(pData, 33);
  char* faceNames[6] = {"Front", "Bottom", "Right", "Top", "Left", "Back"};
  Serial.print("Last Move: ");
  Serial.print(faceNames[lastMoveFace-1]);
  Serial.print(lastMoveDirection == 1 ? " Face Clockwise" : " Face Anti-Clockwise" );
  Serial.println("");
  
  Serial.println("----");

  if(memcmp(pData, solution, 16) == 0) {
    digitalWrite(relayPin, HIGH);
    delay(100);
    digitalWrite(relayPin, LOW);
  }
  
}

/*
 * Connect to the BLE server of the correct MAC address
 */
bool connectToServer() {
    Serial.print(F("Creating BLE client... "));
    BLEClient* pClient = BLEDevice::createClient();
    delay(500);
    Serial.println(F("Done."));

    Serial.print(F("Assigning callbacks... "));
    pClient->setClientCallbacks(new ClientCallbacks());
    delay(500);
    Serial.println(F(" - Done."));

    // Connect to the remove BLE Server.
    Serial.print(F("Connecting to "));
    Serial.print(myDevice->getAddress().toString().c_str());
    Serial.print(F("... "));
    pClient->connect(myDevice);
    delay(500);
    Serial.println(" - Done.");
    
    // Obtain a reference to the service we are after in the remote BLE server.
    Serial.print(F("Finding service "));
    Serial.print(serviceUUID.toString().c_str());
    Serial.print(F("... "));
    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    delay(500);
    if (pRemoteService == nullptr) {
      Serial.println(F("FAILED."));
      return false;
    }
    Serial.println(" - Done.");
    delay(500);

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
    delay(500);
    
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
}

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

// Initial setup
void setup() {
  // Start the serial connection to be able to track debug data
  Serial.begin(115200);
  
  Serial.print("Initialising BLE...");
  BLEDevice::init("");
  delay(500);
  Serial.println(F("Done."));

  // relayPin will be set HIGH when the cube is solved
  pinMode(relayPin, OUTPUT);
  digitalWrite(relayPin, LOW);

  // ledPin will be set HIGH when cube is connected 
  pinMode(LED_BUILTIN, OUTPUT);
}

// Main program loop function
void loop() {
  // If the cube has been found, connect to it
  if (deviceFound) {
    if(!connected) {
      connectToServer();
    }
  }
  else {
    scanForDevices();
  }
  // Introduce a little delay
  delay(1000);
}

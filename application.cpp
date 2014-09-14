/**
 * Copyright 2014  Michael Straßburger
 * http://github.com/rastapasta/spark-bouncer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * ************************************************************
 *  Connect RC522 module to following Spark Core pins:
 *    RST   -> A1
 *    SDA  ->  A2
 *    SCK  ->  A3
 *    MISO ->  A4
 *    MOSI ->  A5
 *
 *  Connect the door relais signal line to following default pin:
 *    D0
 *
 *  Connect the door open button to following default pins:
 *    D1  +  3.3V
 * ************************************************************
 */

// Optimized for local compilation - https://github.com/spark/core-firmware#1-download-and-install-dependencies
// Might need minimal adaptions to compile in the cloud.
#include "MFRC522.h"
#include "flashee-eeprom.h"
using namespace Flashee;

/*************** Configuration ***************/
#define TIME_OPEN_MILLIS 2000
#define TIME_SYNC_MILLIS (6 * 60 * 60 * 1000)

#define RFID_BLOCK   1                          // In which data block should the OTP get stored?
#define RFID_INTERVAL_MILLIS 250                // Let it rest a bit
#define RFID_SS_PIN  SS
#define RFID_RST_PIN A1

#define RELAIS_PIN   D0
#define BUTTON_PIN   D1

/*************** Event types ***************/
#define EVENT_NOT_FOUND     0
#define EVENT_OPEN          1
#define EVENT_OUT_OF_HOURS  2
#define EVENT_DISABLED      3
#define EVENT_LOST          4
#define EVENT_OTP_MISSMATCH 5

#define EVENT_STORAGE_FULL  8
#define EVENT_UPDATED       9

/*************** Internal thingies ***************/
#define KEY_SIZE            10
#define OTP_SIZE            16

#define FLASH_CONFIG_BEGIN  0
#define FLASH_KEYS_BEGIN    128
#define FLASH_LOG_BEGIN     4096*8
#define FLASH_DATA_BEGIN    4096*40

#define FLASH_KEYS_MAX      ((FLASH_LOG_BEGIN-FLASH_KEYS_BEGIN)/KEY_SIZE)
#define FLASH_LOG_MAX       ((FLASH_DATA_BEGIN-FLASH_LOG_BEGIN)/sizeof(log_t))
#define KEY_NOT_FOUND       -1

#define DEBUG_PRINT(x)      if(debugMode)Serial.print(x)
#define DEBUG_PRINTLN(x)    if(debugMode)Serial.println(x)

/*************** Data structures ***************/
typedef struct config_struct {
	uint16_t storedKeys;
	uint16_t logEntries;
} config_t;

typedef struct user_struct {
	bool supportsOTP;
	byte OTP[OTP_SIZE];

	uint32_t days[7];

	int lastUpdated;
	int lastSeen;

	bool isActive;
	bool isLost;
} user_t;

typedef struct log_struct {
	int time;
	byte key[KEY_SIZE];
	int event;
} log_t;

/*************** Prototypes ***************/
void checkRFID();           // Loop function, checking for new RFID card
void checkButton();         // Loop function, handling of the door opening buzzer
void checkDoor();           // Loop function, relais handling
void checkTime();           // Loop function, re-syncing time with the Spark Cloud

int checkAccess(user_t &);  // Checks if the given user struct can open the door right now

void openDoor();            // Triggers the relais and sets a timeout to stay open
void closeDoor();           // Pulls the relais down

void rfidSetup();           // Starts the SPI handler and RC522 communication
void rfidIdentify();        // Handles a new card, reads ID and matches it to keyId

bool rfidAuth(int);         // Authenticates access to given block
bool rfidRead(byte (&)[16], int);  // Reads a block and stores it in first arg
bool rfidWrite(int, byte (&)[16]); // Writes 16 bytes of data into given block

void cloudSetup();          // Setup Spark.function* calls
int cloudUpdate(String);    // update: send ab:cd:ef:ff;*;active,otp to store key
int cloudOpen(String);      // open: opens the door/relais
int cloudDebug(String);     // debug: 1-> enable  0->disable
int cloudReset(String);     // reset: (careful) removes all keys

void cloudEvent(byte (&)[KEY_SIZE], int); // comunicate an access attempt + result
void logEvent(byte (&)[KEY_SIZE], byte);  // stores an event in its designated flash area
void updateLogBuffer();                 // Updates the buffer which is exposed via the Cloud
long findKey(byte (&)[KEY_SIZE]);       // Locate a given key in flash
uint16_t addKey(byte (&)[KEY_SIZE]);    // Add a given key to flash

void saveUser(user_t &, uint16_t);      // Store a user in flash
user_t readUser(uint16_t);              // Read a user from flash
void dumpUser(user_t &);                // Serial dump an user struct in a readable way

void readConfig();                      // Reads the config from flash
void saveConfig();                      // Stores the config in flash

void blink();                           // Blink the LED
void printlnHex16(byte (&)[16]);        // Help to hexify 16byes in hex
String keyToString(byte (&)[KEY_SIZE]); // Converts a key to a minimalized aa:bb:cc:.. string

/*************** Globals ***************/
unsigned long openUntil = 0;
unsigned long lastSync = millis();
unsigned long nextRFID = 0;

char logBuffer[622];
char serialBuffer[622];
bool debugMode = true;

config_t config;

FlashDevice* flash;
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

/*************** Main Functions ***************/
void setup() {
	pinMode(RELAIS_PIN, OUTPUT);
	pinMode(BUTTON_PIN, INPUT_PULLDOWN);

	Serial.begin(9600);

	flash = Devices::createWearLevelErase();
	readConfig();
	
	cloudSetup();
	rfidSetup();
}

void loop() {
    checkRFID();
    checkButton();
    checkDoor();
    checkTime();
}

/*************** Cloud Communication ***************/
void cloudSetup() {
	Spark.function("update", cloudUpdate);
	Spark.function("reset", cloudReset);
	Spark.function("debug", cloudDebug);
	Spark.function("open", cloudOpen);

	Spark.variable("log", logBuffer, STRING);
	updateLogBuffer();
}

int cloudOpen(String foo) {
	Spark.publish("call", NULL, 60, PRIVATE);
	openDoor();
	return 1;
}

// Receives a String in the format  aa:bb:cc:dd;FF 0 0 FF00 AAFF;active,otp
// Parse it!
int cloudUpdate(String param) {
	int position = 0;
	
	byte key[KEY_SIZE];
	memset(&key, 0, KEY_SIZE);

	// Parse the key ID, can be up to 10 hex fields seperated by :, like aa:bb:cc:33
	for (byte i=0; i<KEY_SIZE; i++) {
		key[i] = strtoul(&param[position], NULL, 16);
		if (param.indexOf(":", position) != -1)
			position = param.indexOf(":", position)+1;
		else
			break;
	}
	position = param.indexOf(";", position)+1;
	if (!position)
		return -1;

	user_t user;

	// Find or setup the key and its data
	long keyId = findKey(key);
	if (keyId == KEY_NOT_FOUND) {
		if (config.storedKeys+1 > FLASH_KEYS_MAX) {
			Spark.publish(F("error"), F("can't add new key, storage is full"), 60, PRIVATE);
			return -1;
		}
		memset(&user, 0, sizeof(user));
		keyId = addKey(key);
	} else
		user = readUser(keyId);

	// Parse the hour information - 16 byte longs coded bitwise, each bit maps to one hour
	// Using * as argument sets all hours as valid
	// Using - as argument, all hours are removed
	if (param[position] != ';') {
		if (param[position] == '*')
			memset(&user.days, 0xFF, sizeof(user.days));
		else if(param[position] == '-')
			memset(&user.days, 0, sizeof(user.days));
		else {
			memset(&user.days, 0, sizeof(user.days));
			for (byte i=0; i<7; i++) {
				user.days[i] = strtoul(&param[position], NULL, 16);
				if (param.indexOf(" ", position) != -1)
					position = param.indexOf(" ", position)+1;
				else
					break;
			}
		}
	}

	// There should be a semicolon right ahead, otherwise #fail
	position = param.indexOf(";", position)+1;
	if (!position)
		return -1;

	// Set status based on string appearance
	user.supportsOTP = param.indexOf("otp", position) != -1;
	user.isActive = param.indexOf("active", position) != -1;
	user.isLost = param.indexOf("lost", position) != -1;

	// Reset the OTP if the reset flag is set
	if (param.indexOf("reset", position) != -1)
		memset(&user.OTP, 0, OTP_SIZE);

	// Update the last changed time
	user.lastUpdated = Time.now();

	if (debugMode)
		dumpUser(user);

	saveUser(user, keyId);
	
	cloudEvent(key, EVENT_UPDATED);
	return 1;
}

// Enable debugging?
int cloudDebug(String foo) {
	debugMode = foo.toInt() == 1;
	return debugMode;
}

// Uh oh, reset our key storage
int cloudReset(String foo) {
	memset(&config, 0, sizeof(config));
	config.storedKeys = 0;
	config.logEntries = 0;
	saveConfig();
	return 1;
}

// Communicate that a card scan got handled - published format:
//   timestamp:xx:xx:xx...:EVENT_CODE (-> see header)
void cloudEvent(byte (&key)[KEY_SIZE], int eventCode) {
	String event = String(Time.now());
	event += ";";
	event += keyToString(key);
	event += F(",");
	event += eventCode;

	logEvent(key, eventCode);
	Spark.publish(F("card"), event, 60, PRIVATE);
}

/*************** Flash logging and cloud buffer handling ***************/
// Stores the event döner style
void logEvent(byte (&key)[KEY_SIZE], byte eventCode) {
	int position = config.logEntries++ % FLASH_LOG_MAX;
	
	// Setup a log record ..
	log_t entry;
	memcpy(entry.key, key, KEY_SIZE);
	entry.time = Time.now();
	entry.event = eventCode;

	// .. and store it in flash
	flash->write(&entry, FLASH_LOG_BEGIN+position*sizeof(entry), sizeof(entry));

	saveConfig();
	updateLogBuffer();
}

// Fill up the 622 bytes buffer the Cloud has access to
void updateLogBuffer() {
	log_t entry;
	int position = 0;

	memset(&logBuffer, 0, sizeof(logBuffer));	
	for(int i=config.logEntries-1; i>=0; i--) {
		flash->read(&entry, FLASH_LOG_BEGIN+(i%FLASH_LOG_MAX)*sizeof(entry), sizeof(entry));

		String str = String(entry.time);
		str += ";";
		str += keyToString(entry.key);
		str += ";";
		str += entry.event;
		str += "\n";

		if (position + str.length() < sizeof(logBuffer)-1) {
			for (byte j=0; j<str.length(); j++)
				logBuffer[position++] = str[j];
		} else
			break;
	}
}

/*************** Door Relais handling ***************/
void checkDoor() {
	if (!openUntil || millis() < openUntil)
		return;
	
	openUntil = 0;
	closeDoor();
}

void openDoor() {
	DEBUG_PRINTLN(F("[door] opening"));
	openUntil = millis() + TIME_OPEN_MILLIS;
	digitalWrite(RELAIS_PIN, HIGH);
	blink();
}

void closeDoor() {
	DEBUG_PRINTLN(F("[door] closing"));
	digitalWrite(RELAIS_PIN, LOW);
}

// Check if the door buzzer is pressed
void checkButton() {
	if (digitalRead(BUTTON_PIN) == LOW) {
		if (!openUntil)
			Spark.publish("button", NULL, 60, PRIVATE);
		openDoor();
	}
}

/*************** RFID handling and helpers ***************/
void checkRFID() {
    if (openUntil || millis() < nextRFID)
        return;
    nextRFID = millis() + RFID_INTERVAL_MILLIS;

    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial())
		return;

	rfidIdentify();

	mfrc522.PICC_HaltA();
	mfrc522.PCD_StopCrypto1();
}

void rfidIdentify() {
	byte uid[KEY_SIZE];

	// pad the scanned uid to KEY_SIZE
	memset(uid, 0, KEY_SIZE);
	memcpy(uid, mfrc522.uid.uidByte, mfrc522.uid.size);

	DEBUG_PRINT(F("[rfid] identifying "));
	DEBUG_PRINTLN(keyToString(uid));

	// check if we know this key
	long keyId = findKey(uid);
	if (keyId == KEY_NOT_FOUND) {
		cloudEvent(uid, EVENT_NOT_FOUND);
		nextRFID = millis() + TIME_OPEN_MILLIS;
		return;
	}

	user_t user = readUser(keyId);
	
	if (debugMode)
		dumpUser(user);

	// Check the OTP in case its activated for the given card
	if (user.supportsOTP) {
		if (!rfidAuth(RFID_BLOCK))
			return;

		byte newOTP[OTP_SIZE];
		memset(&newOTP, 0, OTP_SIZE);

		// Has the OTP already been set once?
		if (memcmp(user.OTP, newOTP, OTP_SIZE) != 0) {

			// Read block
			byte OTP[16];
			if (!rfidRead(OTP, RFID_BLOCK))
				return;

			if (debugMode) {
				Serial.print(F("OTP on Chip:"));
				printlnHex16(OTP);
			}

			// Compare RFID OTP vs User OTP
			if (memcmp(user.OTP, OTP, OTP_SIZE) != 0) {
				DEBUG_PRINTLN(F("[rfid] OTP missmatch - possible card highjack. disabling user."));
				user.isActive = false;
				saveUser(user, keyId);
				cloudEvent(uid, EVENT_OTP_MISSMATCH);
				return;
			}
		}

		// Create a new random OTP
		for (byte i=0; i<OTP_SIZE; i++)
			newOTP[i] = random(0,256);

		if (debugMode) {
			Serial.print(F("New OTP:    "));
			printlnHex16(newOTP);
		}

		// Save new OTP on card
		if (!rfidWrite(RFID_BLOCK, newOTP))
			return;

		// Save the new OTP in the user record
		memcpy(&user.OTP, newOTP, sizeof(newOTP));
	}

	// Check what this user may do - open the gate?
	int access = checkAccess(user);
	
	// Communicate what just happend here
	cloudEvent(uid, access);

	// Save the updated OTP and lastSeen field - in case we need to
	user.lastSeen = Time.now();
	saveUser(user, keyId);		
}

// Setup the SPI + RFID module
void rfidSetup() {
	SPI.begin();
	SPI.setClockDivider(SPI_CLOCK_DIV8);
	mfrc522.PCD_Init();
}

// Authenticate a block access
bool rfidAuth(int block) {
	MFRC522::MIFARE_Key key;
	
	// All Mifare chips have their factory default keys set to HIGH
	memset(&key, 0xFF, sizeof(key));

	byte status = mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, block, &key, &(mfrc522.uid));
	if (status != MFRC522::STATUS_OK) {
		DEBUG_PRINTLN(F("[rfid] auth failed"));
		return false;
	}

	// All good!
	return true;
}

bool rfidRead(byte (&target)[16], int block) {
	byte buffer[18];
	byte byteCount = sizeof(buffer);
	int status = mfrc522.MIFARE_Read(RFID_BLOCK, buffer, &byteCount);
	if (status != MFRC522::STATUS_OK) {
		DEBUG_PRINTLN(F("[rfid] read failed"));
		return false;
	}

	// <numnumnum>
	memcpy(target, buffer, 16);
	return true;
}

bool rfidWrite(int block, byte (&data)[16]) {
	if (mfrc522.MIFARE_Write(block, data, 16) != MFRC522::STATUS_OK) {
		DEBUG_PRINTLN(F("[rfid] write failed"));
        return false;
	}
	return true;
}

/*************** Access control ***************/
// Handle a recognized users access - abracadabra, may it open!
int checkAccess(user_t &user) {
	int event;
	if (user.isLost) {

		DEBUG_PRINTLN(F("[card] marked as lost.."));
		event = EVENT_LOST;
	
	} else if (!user.isActive) {

		DEBUG_PRINTLN(F("[card] not marked as active.."));
		event = EVENT_DISABLED;

	} else if ((user.days[(Time.weekday()+5)%7] & 1<<((Time.hour()+2)%24)) == 0) {
		
		DEBUG_PRINTLN(F("[card] usage out of hours.."));
		event = EVENT_OUT_OF_HOURS;
	
	} else {

		DEBUG_PRINTLN(F("[card] hours match, opening!"));
		openDoor();
		event = EVENT_OPEN;

	}
	return event;
}

/*************** Key indexing handlers ***************/
// Allocate a given key ID in our storage and return its position (or KEY_NOT_FOUND)
long findKey(byte (&key)[KEY_SIZE]) {
	bool found = false;
	uint16_t keyId = 0;

	byte buf[KEY_SIZE];

	for (uint16_t i=0; i<config.storedKeys; i++) {
		flash->read(buf, FLASH_KEYS_BEGIN+i*KEY_SIZE, KEY_SIZE);
		if (memcmp(buf, key, KEY_SIZE) == 0) {
			found = true;
			keyId = i;
			break;
		}
	}

	if (!found) {
		DEBUG_PRINTLN(F("[flash] Key not found."));
		return KEY_NOT_FOUND;
	}

	DEBUG_PRINT(F("[flash] Key found, index #"));
	DEBUG_PRINTLN(keyId);

	return keyId;
}

// Add a new RFID key into the key storage and increment the storage counter
// Returns the new key's position
uint16_t addKey(byte (&key)[KEY_SIZE]) {
	uint16_t keyId = config.storedKeys++;
	saveConfig();
	
	flash->write(&key, FLASH_KEYS_BEGIN+keyId*KEY_SIZE, KEY_SIZE);
	return keyId;
}


/*************** Flash readers/writers ***************/
// Write a user struct into the designated data storage area
void saveUser(user_t &user, uint16_t keyId) {
	flash->write(&user, FLASH_DATA_BEGIN+keyId*sizeof(user_t), sizeof(user_t));
}

// Restore a user struct from flash
user_t readUser(uint16_t keyId) {
	user_t user;
	flash->read(&user, FLASH_DATA_BEGIN+keyId*sizeof(user_t), sizeof(user_t));
	return user;
}

// Restores the system configuration from flash
void readConfig() {
	flash->read(&config, 0, sizeof(config));
}

// Dumps the configuration struct into the flash
void saveConfig() {
	flash->write(&config, 0, sizeof(config));
}

/*************** Sync + Debugging helpers ***************/
// Request time synchronization from the Spark Cloud to keep in sync
void checkTime() {
    if (millis() - lastSync < TIME_SYNC_MILLIS)
    	return;

    Spark.syncTime();
    lastSync = millis();
}

// Make a beautiful dump.
void printlnHex16(byte (&data)[16]) {
	for (byte i=0; i<16; i++) {
		Serial.print(data[i] < 0x10 ? " 0" : " ");
		Serial.print(data[i], HEX);
	}
	Serial.println();
}

// Create a smart version of a RFID key
String keyToString(byte (&key)[KEY_SIZE]) {
	String str;
	bool stop;
	for (byte i = 0; i < KEY_SIZE; i++) {
		// Look ahead to see if only empty fields are following
		// -> if so, shorten the stringified key to its minimum
		stop = true;
		for (byte j=i; j < KEY_SIZE; j++) {
			if (key[i]) {
				stop = false;
				break;
			}
		}
		if (stop)
			break;

		if (i>0)
			str += ":";
		if (key[i] < 0xF)
			str += "0";

		str += String(key[i], HEX);
	}
	return str;
}

// Flash 'em
void blink()
{
    RGB.control(true);
    RGB.color(0, 0, 0);
    delay(50);
    RGB.color(0, 255, 0);
    delay(200);
    RGB.color(0, 0, 0);
    delay(50);
    RGB.control(false);
}

// Pretty output of a user struct
void dumpUser(user_t &user) {
	Serial.print(F("-- Active? "));
	Serial.println(user.isActive ? F("yes") : F("no"));
	Serial.print(F("-- Lost? "));
	Serial.println(user.isLost ? F("yes") : F("no"));
	Serial.println(F("-- Times:"));

	Serial.println(F("          Monday   Tuesday  Wednesday  Thursday   Friday   Saturday   Sunday"));

	for(byte hour=0; hour<24; hour++) {
		if (hour < 10)
			Serial.print(F(" "));
		Serial.print(hour);
		Serial.print(F(" h"));

		for(byte day=0; day<7; day++) {
			Serial.print(F("       "));
			if((Time.weekday()+5)%7 == day && (Time.hour()+2)%24 == hour)
				Serial.print(user.days[day] & 1<<hour ? F("(*)") : F("( )"));
			else
				Serial.print(user.days[day] & 1<<hour ? F(" * ") : F("   "));
		}
		Serial.println();
	}
	Serial.println();
	Serial.print(F("-- last update of user configuration: "));
	Serial.print(Time.timeStr(user.lastUpdated));

	Serial.print(F("-- last seen: "));
	Serial.print(Time.timeStr(user.lastSeen));
	Serial.println();

	Serial.print(F("-- OTP:     "));
	
	if (user.supportsOTP)
		printlnHex16(user.OTP);
	else
		Serial.println(F("not actived for this card"));
}


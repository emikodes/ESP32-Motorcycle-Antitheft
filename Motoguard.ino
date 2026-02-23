#include <Wire.h> 
#include <SPI.h> 
#include <MFRC522.h> 
#include <WiFi.h> 
#include <Preferences.h>

const int SENSITIVITY = 3000;
const unsigned long ALARM_DURATION = 30000;
const uint8_t MPU_ADDR = 0x68;

#define PIN_RFID_SDA  5
#define PIN_RFID_RST  4
#define PIN_GSM_RX    16
#define PIN_GSM_TX    17
#define PIN_RELAY_HORN   32
#define PIN_RELAY_BLOCK  33
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

MFRC522 rfid(PIN_RFID_SDA, PIN_RFID_RST);
HardwareSerial gsmSerial(2); 
Preferences prefs;

String phoneNumber = "";
String masterTag = "";
unsigned long alarmStartTime = 0;
unsigned long lastHornToggle = 0;
unsigned long lastTagTime = 0;
unsigned long lastMotionCheck = 0;
bool isArmed = false;
bool isAlarming = false;
bool hasGSM = false;
bool hornState = false;
bool firstReading = true;
const int TAG_DELAY = 3000; 
int16_t lastX, lastY, lastZ;

void setup() {
	WiFi.mode(WIFI_OFF);
	btStop();
	setCpuFrequencyMhz(80);
	Serial.begin(115200);
	Serial.setTimeout(100); 
	
	gsmSerial.begin(9600, SERIAL_8N1, PIN_GSM_RX, PIN_GSM_TX);
	gsmSerial.setTimeout(100); 
	
	pinMode(PIN_RELAY_HORN, OUTPUT);
	pinMode(PIN_RELAY_BLOCK, OUTPUT);
	digitalWrite(PIN_RELAY_HORN, RELAY_OFF);
	digitalWrite(PIN_RELAY_BLOCK, RELAY_ON);
	
	prefs.begin("motoguard", false); 
	phoneNumber = prefs.getString("master_num", ""); 
	masterTag = prefs.getString("master_tag", ""); 
	
	Wire.begin(21, 22);
	Wire.setClock(100000);
	delay(500);
	Wire.beginTransmission(MPU_ADDR);
	Wire.write(0x6B); 
	Wire.write(0);
	Wire.endTransmission();
	
	SPI.begin();
	rfid.PCD_Init();
	
	initGSM();
	
	Serial.println("Numero di telefono memorizzato: " + (phoneNumber != "" ? phoneNumber : String("Nessuno")));
  	Serial.println("TAG memorizzato: " + (masterTag != "" ? masterTag : String("Nessuno")));
	sendSMS("MOTOGUARD attivo.\nComandi:\nARM - Attiva l'antifurto\nDISARM - Disattiva l'antifurto\nRESET PHONE - Resetta il numero di telefono\nRESET TAG - Resetta il tag RFID\nRESET ALL - Resetta tutti i dati");
}

void initGSM() {
	hasGSM = false;
	
	gsmSerial.println("ATE0"); 
	delay(500); 
	while(gsmSerial.available()) gsmSerial.read(); 
	
	gsmSerial.println("AT"); 
	delay(500);
	if(gsmSerial.readString().indexOf("OK") == -1) {
		Serial.println("Errore durante la comunicazione con SIM800L");
		return;
	}
	
	gsmSerial.println("AT+CPIN?");
	delay(1000);
	if (gsmSerial.readString().indexOf("READY") == -1) {
		Serial.println("Errore SIM");
		return;
	}
	
	gsmSerial.println("AT+CMGD=1,4"); 
	delay(500);
	gsmSerial.println("AT+CMGF=1");   
	delay(500);
	gsmSerial.println("AT+CNMI=2,2,0,0,0"); 
	delay(500);
	
	while(gsmSerial.available()) gsmSerial.read();
	
	hasGSM = true;
}

void loop() {
	checkIncomingCommands();
	
	if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
		if (millis() - lastTagTime >= TAG_DELAY) {
			lastTagTime = millis();
			String tagID = readRFIDTag();
			handleTag(tagID);
		}
		rfid.PICC_HaltA(); 
		rfid.PCD_StopCrypto1();
	}
	
	if (isArmed && !isAlarming) {
		checkMotionRaw();
	}
	
	if (isAlarming) {
		manageAlarm();
	}
}

String readRFIDTag() {
	String tagID = "";
	for (byte i = 0; i < rfid.uid.size; i++) {
		tagID += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
		tagID += String(rfid.uid.uidByte[i], HEX);
	}
	tagID.toUpperCase();
	return tagID;
}

void armSystem() {
	isArmed = true;
	firstReading = true;
	isAlarming = false;
	digitalWrite(PIN_RELAY_BLOCK, RELAY_OFF);
	Serial.println("ARMED");
	beepHorn(20);
}

void disarmSystem() {
	isArmed = false;
	isAlarming = false;
	digitalWrite(PIN_RELAY_HORN, RELAY_OFF); 
	digitalWrite(PIN_RELAY_BLOCK, RELAY_ON);  

	if (hasGSM) {
		gsmSerial.println("ATH"); 
	}
	
	Serial.println("DISARMED");
	beepHorn(20); 
	delay(100); 
	beepHorn(20);
}

void checkIncomingCommands() {
	String incomingMsg = "";
	
	if (gsmSerial.available()) {
		incomingMsg = gsmSerial.readString();
	} else if (Serial.available()) {
		incomingMsg = Serial.readString();
	}
	
	if (incomingMsg.length() == 0) return;
	
	incomingMsg.toUpperCase(); 
	
	if (incomingMsg.indexOf("+CMT:") != -1) {
		int firstQuote = incomingMsg.indexOf('"');
		int secondQuote = incomingMsg.indexOf('"', firstQuote + 1);
		
		if (firstQuote != -1 && secondQuote != -1) {
			String senderNumber = incomingMsg.substring(firstQuote + 1, secondQuote);
			
			if (phoneNumber == "") {
				phoneNumber = senderNumber;
				prefs.putString("master_num", phoneNumber);
				Serial.print("Numero di telefono salvato: " + senderNumber);
				sendSMS("Numero di telefono registrato. D'ora in poi potrai utilizzare i seguenti comandi per interagire con MOTOGUARD.\nComandi:\nARM - Attiva l'antifurto\nDISARM - Disattiva l'antifurto\nRESET PHONE - Resetta il numero di telefono\nRESET TAG - Resetta il tag RFID\nRESET ALL - Resetta tutti i dati");
				return; 
			} 
			else if (senderNumber != phoneNumber) {
				Serial.println("Numero sconosciuto: " + senderNumber);
				return;
			}
		}
	}
	processCommand(incomingMsg);
}

void processCommand(String cmd) {
	if (cmd.indexOf("DISARM") != -1) {
		disarmSystem();
		sendSMS("Antifurto disattivato da remoto");
	} 
	else if (cmd.indexOf("ARM") != -1) {
		armSystem();
		sendSMS("Antifurto attivato da remoto");
	}
	else if (cmd.indexOf("RESET PHONE") != -1) {
		Serial.println("Cancellazione numero master");
		sendSMS("Cancellazione numero master eseguita. Puoi registrarne un'altro, inviando un nuovo SMS");
		phoneNumber = "";
		prefs.putString("master_num", ""); 
	}
	else if (cmd.indexOf("RESET TAG") != -1) {
		Serial.println("Cancellazione tag master");
		sendSMS("Cancellazione TAG RFID eseguita con successo. Puoi registrarne un'altro passandolo sul lettore");
		masterTag = "";
		prefs.putString("master_tag", ""); 
	}
	else if (cmd.indexOf("RESET ALL") != -1) {
		Serial.println("Reset di fabbrica in corso");
		sendSMS("RESET eseguito con successo.");
		phoneNumber = "";
		masterTag = "";
		disarmSystem();
		prefs.putString("master_num", ""); 
		prefs.putString("master_tag", ""); 
	}
}

void handleTag(String tag) {
	if (masterTag == "") {
		masterTag = tag;
		prefs.putString("master_tag", masterTag); 
		Serial.println("TAG RFID Registrato");
		beepHorn(50); 
		return;
	}else if (tag == masterTag) {
		if (!isArmed) {
			armSystem();
			delay(2000); 
		} else {
			disarmSystem();
		}
	} else {
		Serial.println("TAG non riconosciuto");
		beepHorn(30);
	}
}

void beepHorn(int duration) {
	digitalWrite(PIN_RELAY_HORN, RELAY_ON); 
	delay(duration);
	digitalWrite(PIN_RELAY_HORN, RELAY_OFF);
}

void checkMotionRaw() {
	if (millis() - lastMotionCheck < 200) return; 
	lastMotionCheck = millis();
	
	Wire.beginTransmission(MPU_ADDR); 
	Wire.write(0x3B); 
	Wire.endTransmission(false);
	Wire.requestFrom(MPU_ADDR, (uint8_t)6, true);
	
	if (Wire.available() < 6) return;
	
	int16_t AcX = Wire.read() << 8 | Wire.read();
	int16_t AcY = Wire.read() << 8 | Wire.read();
	int16_t AcZ = Wire.read() << 8 | Wire.read();
	
	if (firstReading) { 
		lastX = AcX; 
		lastY = AcY; 
		lastZ = AcZ; 
		firstReading = false; 
		return; 
	}
	
	int32_t delta = abs(AcX - lastX) + abs(AcY - lastY) + abs(AcZ - lastZ);
	if (delta > SENSITIVITY) { 
		triggerAlarm(); 
	}
	
	lastX = AcX; 
	lastY = AcY; 
	lastZ = AcZ; 
}

void triggerAlarm() {
	isAlarming = true; 
	alarmStartTime = millis();
	digitalWrite(PIN_RELAY_BLOCK, RELAY_OFF); 
	
	digitalWrite(PIN_RELAY_HORN, RELAY_ON);
	hornState = true;
	lastHornToggle = millis();
	
	if (hasGSM && phoneNumber != "") { 
		makeCall(phoneNumber); 
	} else {
		Serial.println("Numero di telefono non ancora registrato");
	}
}

void manageAlarm() {
	if (millis() - alarmStartTime > ALARM_DURATION) {
		isAlarming = false; 
		digitalWrite(PIN_RELAY_HORN, RELAY_OFF); 
		rfid.PCD_Init();
		if (hasGSM) {
			gsmSerial.println("ATH");
		}
		return;
	}
	if (millis() - lastHornToggle > 300) {
		lastHornToggle = millis(); 
		hornState = !hornState;
		digitalWrite(PIN_RELAY_HORN, hornState ? RELAY_ON : RELAY_OFF);
	}
}

void makeCall(String number) { 
	if (!hasGSM || number == "") return; 
	gsmSerial.println("ATD" + number + ";");
}


void sendSMS(String text) {
	if (!hasGSM || phoneNumber == "") return;
	gsmSerial.println("AT+CMGS=\"" + phoneNumber + "\"");
	delay(1000);
	gsmSerial.print(text);
	delay(1000);
	gsmSerial.write(26);
}
// needed for atoi()
#include <stdlib.h>

// custom time library
#include <Time.h>

// ethernet and udp
#include <SPI.h>         
#include <Ethernet.h>
#include <EthernetUdp.h>

// used to store alarm settings
#include <EEPROM.h>


// the 4 gnd pins for multiplexing
int controlPin1 = A0;
int controlPin2 = A1;
int controlPin3 = A3;
int controlPin4 = A4;
int controlPins[] = {controlPin1,controlPin2,controlPin3,controlPin4};

// pins for shift register
int dataPin = 7;
int clockPin = 8;
int latchPin = 9;

// buzzer
int piezoPin = 5;

// button
int buttonPin = 2;
boolean buttonLastState = HIGH;
boolean buttonCurrentState = HIGH;

// 7segment bits
const byte segment[10] = {
  B00111111, //0
  B00000110, //1
  B01011011, //2
  B01001111, //3
  B01100110, //4
  B01101101, //5
  B01111101, //6
  B00000111, //7
  B01111111, //8
  B01101111  //9
};

unsigned long lastMillis = 0; // used for blinker
boolean dotState = false; // blinker state

boolean alarmState = false; // is alarm on or off?
int alarmTime[] = {10, 11}; // 10:00
int alarmLength = 2; // 2 minute alarms should be fine (+2 actually means 3 minutes.. so its 3 minutes of alarm!)
unsigned long alarmStopTime = 0; // when was the alarm last stopped (unix epoch)

// ethernet udp ntp
// 90-A2-DA-0D-04-F0
byte mac[] = { 0x90, 0xA2, 0xDA, 0x0D, 0x04, 0xF0 };
unsigned int localPort = 8888;
IPAddress timeServer(64, 90, 182, 55); // nist1-ny.ustiming.org	64.90.182.55	New York City, NY
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];
EthernetUDP Udp;

// a server listening on telnet port 23
EthernetServer server(23);
int clientInvalidCommands = 0; // counter for # of invalid commands sent by client. we will disconnect him if he sends more than 3 invalid commands!


void setup() { 
  pinMode(dataPin, OUTPUT);
  pinMode(clockPin, OUTPUT);  
  pinMode(latchPin, OUTPUT); 
  
  pinMode(piezoPin, OUTPUT); 
  
  pinMode(buttonPin, INPUT); 
  
  for(int i = 0; i <= 3; i++) {
    pinMode(controlPins[i], OUTPUT);
  }
  clear();
  
  // start Ethernet and UDP, use DHCP to get IP
  if (Ethernet.begin(mac) == 0) {
    while(true) {
      alarmTone();
    }
  }
  Udp.begin(localPort);
  
  // start server to listen for commands
  server.begin();
  
  setupTime();
  
  // load last set alarm state & time from EEPROM
  loadData();
}

void loop() {
  // recalibrate every 12 hours
  if((hour() == 00 || hour() == 12) && minute() == 00 && second() == 00) {
    setupTime();
  }
  
  // multiplex current time (24HH:MIN) to quad-7segment display
  showCurrentTime();
  
  // alarm!
  if(checkAlarm()) {
    alarmTone();
  }
  
  // turn on or off alarm, or stop current alarm!
  if(debounceButton()) {
    if(checkAlarm()) {
      stopAlarm();  // stops the alarm for the next 2 minutes
    } else {
      alarmState = !alarmState; // switch alarm on/off
    }
  }
  
  listenToClientCommands();
}

void listenToClientCommands() {
  EthernetClient client = server.available();

  if (client) {
    if(client.available() > 0) {
      
      // read command string until first '\n', discard everything afterwards
      String command;
      while(client.available() > 0) {
        char input = client.read();
        
        if(input == '\n') {
          client.flush();
          break;
        }
        
        command += input;
      }
      
      // check if it is a valid command
      if(isValidCommand(command)) {
        clientInvalidCommands = 0;
        if(command.substring(2,3).equalsIgnoreCase("Q")) client.stop();
        else doCommand(command);
        
      } else {
        if(++clientInvalidCommands >= 3) {
          client.stop();
          clientInvalidCommands = 0;
        }
      }
    }
  }
}

void doCommand(String command) {
  String commandType = command.substring(2,4);
  
  // if "C:A:hhmm", set new alarm time
  if(commandType.equals("A:")) {
    char alarmBuf[4];
    command.substring(4,6).toCharArray(alarmBuf, 4);
    alarmTime[0] = atoi(alarmBuf);
    command.substring(6,8).toCharArray(alarmBuf, 4);
    alarmTime[1] = atoi(alarmBuf);
    
    server.print("current alarm time set to: ");
    server.print(alarmTime[0]);
    server.print(":");
    server.println(alarmTime[1]);
  }
  
  // if "C:S:x", set alarm state to on or off
  if(commandType.equals("S:")) {
    if(command.substring(4,5).equals("1")) alarmState = true;
    else alarmState = false;
    
    server.print("alarm state set to: ");
    server.println(alarmState);
  }
  
  // if "C:H:", holds/stops the alarm if it is currently triggered (same as pushing the button)
  if(commandType.equals("H:")) {
    stopAlarm();
    
    server.println("stop alarm!");
  }
  
  // if "C:F:", force the alarm on! (note: this will turn alarm state on!)
  if(commandType.equals("F:")) {
    forceAlarm();
    
    server.println("force alarm!");
  }
  
  // if "C:G:", get current alarm time
  if(commandType.equals("G:")) {
    server.print("current alarm state & time: [");
    server.print(alarmState);
    server.print("] - ");
    server.print(alarmTime[0]);
    server.print(":");
    server.println(alarmTime[1]);
  }
  
  // if "C:L:", load alarm state & time from EEPROM
  if(commandType.equals("L:")) {
    loadData();
    server.print("loaded alarm state & time from EEPROM: [");
    server.print(alarmState);
    server.print("] - ");
    server.print(alarmTime[0]);
    server.print(":");
    server.println(alarmTime[1]);
  }
  
  // if "C:X:", load alarm state & time from EEPROM
  if(commandType.equals("X:")) {
    saveData();
    server.println("saved current alarm state & time to EEPROM!");
  }
}

boolean isValidCommand(String command) {
  if(command.startsWith("C:", 0)) {
    return true;
  } else {
    return false;
  }
}

void loadData() {
  alarmState = EEPROM.read(0);
  alarmTime[0] = EEPROM.read(1);
  alarmTime[1] = EEPROM.read(2);
}

void saveData() {
  EEPROM.write(0, alarmState);
  EEPROM.write(1, alarmTime[0]);
  EEPROM.write(2, alarmTime[1]);
}

boolean debounceButton() {
    boolean pushed = false;
    
    buttonCurrentState = digitalRead(buttonPin);
    delay(5);
    if(buttonCurrentState == digitalRead(buttonPin)) {
        if(buttonCurrentState != buttonLastState && buttonCurrentState == LOW) pushed = true;
        buttonLastState = buttonCurrentState;
    }
    
    return pushed;
}

void stopAlarm() {
  alarmStopTime = now();
}

void forceAlarm() {
  alarmState = true;
  alarmStopTime = 0;
  alarmTime[0] = hour();
  alarmTime[1] = minute();
}

boolean checkAlarm() {
  if(alarmState   // is alarm on?
  && hour() == alarmTime[0] && minute() >= alarmTime[1] && minute() <= alarmTime[1] + alarmLength    // is it the alarm time?
  && now() - (alarmLength * 60) > (alarmStopTime + 60)) {    // has last alarmStopTime been ago long enough
    return true;
  } else {
    return false;
  }
}

void alarmTone() {
  digitalWrite(piezoPin, HIGH);
  delayMicroseconds(150);
  digitalWrite(piezoPin, LOW);
  delayMicroseconds(250);
  digitalWrite(piezoPin, HIGH);
  delayMicroseconds(50);
  digitalWrite(piezoPin, LOW);
  delayMicroseconds(50);
  digitalWrite(piezoPin, HIGH);
  delayMicroseconds(100);
  digitalWrite(piezoPin, LOW);
  delayMicroseconds(250);
  digitalWrite(piezoPin, HIGH);
  delayMicroseconds(250);
  digitalWrite(piezoPin, LOW);
  delayMicroseconds(50);
}

void showCurrentTime() {
  int digits[4];
  int val = minute();
  // fill digits array
  for(int d = 0; d <= 2; d++) {
    digits[d] = val % 10;
    val /= 10;
  }
  val = hour();
  for(int d = 2; d <= 3; d++) {
    digits[d] = val % 10;
    val /= 10;
  }

  for(int d = 0; d <= 3; d++) {
    int value = segment[digits[d]];
    
    // flash the dot on and off every second
    if(millis() > lastMillis + 500) {
      lastMillis = millis();
      dotState = !dotState;
    }
    if(d == 0) bitWrite(value, 7, dotState); // write dotState bit to position 8 from right-to-left (Bx0000000, x marks the spot)
    
    if(d == 2) bitWrite(value, 7, alarmState); // write alarmState bit to position 8 from right-to-left (Bx0000000, x marks the spot)
    
    digitalWrite(latchPin, LOW);
    shiftOut(dataPin, clockPin, MSBFIRST, value);
    digitalWrite(latchPin, HIGH);

    digitalWrite(controlPins[d], LOW);
    
    delay(2.5);
    clear();
  }
}

void clear() {
  for(int i = 0; i <= 3; i++) {
    digitalWrite(controlPins[i], HIGH);
  }
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, B00000000);
  digitalWrite(latchPin, HIGH);
}

void setupTime() {
  sendNTPpacket(timeServer); // send an NTP packet to a time server
  
  // wait to see if a reply is available
  delay(2000);  
  if(Udp.parsePacket()) {
    Udp.read(packetBuffer, NTP_PACKET_SIZE);

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);  

    unsigned long secsSince1900 = highWord << 16 | lowWord;  

    const unsigned long seventyYears = 2208988800UL;   
    long summerTimezone = (60 * 60 * 2);  
    unsigned long epoch = secsSince1900 - seventyYears + summerTimezone;  

    // setup time library
    setTime(epoch);
  }
}

// send an NTP request to the time server at the given address 
unsigned long sendNTPpacket(IPAddress& address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE); 
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49; 
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp: 		   
  Udp.beginPacket(address, 123); // NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket(); 
}




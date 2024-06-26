#include <LibAPRS.h>        //Modified version of https://github.com/markqvist/LibAPRS
#include <SoftwareSerial.h>
#include <TinyGPS++.h>      //https://github.com/mikalhart/TinyGPSPlus
#include <LowPower.h>       //https://github.com/rocketscream/Low-Power
#include <Wire.h>
#include <Adafruit_BMP085.h>//https://github.com/adafruit/Adafruit-BMP085-Library
#include <Adafruit_BME280.h>
#include <avr/wdt.h>

#define RfPDPin     19
#define GpsVccPin   18
#define RfPwrHLPin  21
#define RfPttPin    20
#define BattPin     A2
#define PIN_DRA_RX  22
#define PIN_DRA_TX  23
#define SEALEVELPRESSURE_HPA (1013.25)

#define ADC_REFERENCE REF_3V3
#define OPEN_SQUELCH false

//Macros
#define GpsON         digitalWrite(GpsVccPin, LOW)//PNP
#define GpsOFF        digitalWrite(GpsVccPin, HIGH)
#define RfON          digitalWrite(RfPDPin, HIGH)
#define RfOFF         digitalWrite(RfPDPin, LOW)
#define RfPwrHigh     pinMode(RfPwrHLPin, INPUT)
#define RfPwrLow      pinMode(RfPwrHLPin, OUTPUT);digitalWrite(RfPwrHLPin, LOW)
#define RfPttON       digitalWrite(RfPttPin, HIGH)//NPN
#define RfPttOFF      digitalWrite(RfPttPin, LOW)
#define AprsPinInput  pinMode(12,INPUT);pinMode(13,INPUT);pinMode(14,INPUT);pinMode(15,INPUT)
#define AprsPinOutput pinMode(12,OUTPUT);pinMode(13,OUTPUT);pinMode(14,OUTPUT);pinMode(15,OUTPUT)

#define DEVMODE // Development mode. Uncomment to enable for debugging.
static bool Dev_Failsafe = false; // Development mode for testing Failsafe
static bool Dev_FoxHunt = false;  // Development mode for testing FoxHunt

//****************************************************************************
// Andrew D Blessing KW9D
// Josh Verbarg KD9ZSY
// Tom Pankonen KD9SAT 
//
char  CallSign[7]="KW9D"; //DO NOT FORGET TO CHANGE YOUR CALLSIGN
int   CallNumber=11; //SSID http://www.aprs.org/aprs11/SSIDs.txt
char  Symbol='O'; // '/O' for balloon, '/>' for car, for more info : http://www.aprs.org/symbols/symbols-new.txt
bool  alternateSymbolTable = false ; //false = '/' , true = '\'

char  comment[50] = "#ARIES";                       // Tag for Telemetry message
char  StatusMessage[50] = "Bloomington Area Career Center (BACC) IL";   // Status message

char  Frequency[9]="144.3900";   //default frequency. 144.3900 for US, 144.8000 for Europe
//char  FoxhuntFreq[9]="144.3900"; //frequency to start transmitting on after we descend for foxhunting
//char  FoxhuntFreq[9]="145.3000"; //frequency to start transmitting on after we descend for foxhunting

static uint16_t foxhuntAlt=1500;       //altitude in ft to turn on the foxhunting beacon on descent
uint16_t        flyAlt=3000;           //altitude in ft we must exceed to trigger foxhunt mode on descent
bool            flyAltReached = false; //did we exceed the flyAlt or not?

//*****************************************************************************

unsigned int   BeaconWait=49;  //seconds sleep for next beacon (TX) -- actual TX is BeaconWait + ~11 seconds (runtime)
unsigned int   BattWait=60;    //seconds sleep if super capacitors/batteries are below BattMin (important if power source is solar panel)
unsigned int   StatusWait=25;  //seconds sleep to send Status after Location is sent

float BattMin=4.5;        // min Volts to wake up.
float DraHighVolt=8.0;    // min Volts for radio module (DRA818V) to transmit (TX) 1 Watt, below this transmit 0.5 Watt. You don't need 1 watt on a balloon. Do not change this.
//float GpsMinVolt=4.0;     //min Volts for GPS to wake up. (important if power source is solar panel) 
boolean aliveStatus = true;     //for tx status message on first wake-up just once.
boolean initialLock = false;    //initial gps lock to limit failsafe mode
boolean beaconViaARISS = false; //try to beacon via ARISS (International Space Station) https://www.amsat.org/amateur-radio-on-the-iss/
unsigned int   ARISSWait=5;    //minutes between attempts at using ARISS (International Space Station)
unsigned long  ARISSAlt=75000; //altitude (feet) to opportunistically attempt ARISS based on ARISSWait interval (ISS pass takes ~90 mins) https://spotthestation.nasa.gov/tracking_map.cfm 

//do not change WIDE path settings below if you don't know what you are doing :) 
byte  Wide1=1; // 1 for WIDE1-1 path
byte  Wide2=1; // 1 for WIDE2-1 path

//*****************************************************************************
/**
Airborne stations above a few thousand feet should ideally use NO path at all, or at the maximum just WIDE2-1 alone.  
Due to their extended transmit range due to elevation, multiple digipeater hops are not required by airborne stations.  
Multi-hop paths just add needless congestion on the shared APRS channel in areas hundreds of miles away from the aircraft's own location.  
NEVER use WIDE1-1 in an airborne path, since this can potentially trigger hundreds of home stations simultaneously over a radius of 150-200 miles. 
 */
int pathSize=2; // 2 for WIDE1-N,WIDE2-N ; 1 for WIDE2-N
boolean autoPathSizeHighAlt = true; //force path to WIDE2-N only for high altitude (airborne) beaconing (over 1.000 meters (3.280 feet)) 

//*****************************************************************************
/**
In the event of GPS failing to maintain telemtery lock, send status APRS messages anyways.  This will ensure sensor data is still tx despite
location information is unavaliable.  When the tracker is in this condition, the Status interval is disregarded the TX Beacon interval is now
used to determine when to send status messages.  Additionally, when the tracker is in this condition, there is optional support for high performance
mdode to increase the chances of the GPS achieving a lock at the cost of additional power consumption.  This is not recommended for pico balloon.
*/
boolean gpsLock = false; //Keep track if we have a valid GPS lock or not

//uint16_t FxCount = 1; //increase +1 after every second spent transmitting foxhunt signal
float tempAltitude = 0; //store the current loop altitude for calculating foxhunting

//*****************************************************************************
unsigned status;

//boolean GpsFirstFix=false;
boolean ublox_high_alt_mode = false;
 
char telemetry_buff[100];  // telemetry buffer

// TX Counters
word TxCount = 0;
word TelemetryCount = 0;
word StatusCount = 0;
word NoGPS = 0;

// ARISS Mode
boolean arissModEnabled = false;

// Balloon pop free fall
unsigned long LowestPressure = 0;
boolean ExpressReturn = false;

//*****************************************************************************

TinyGPSPlus gps;
Adafruit_BMP085 bmp;
Adafruit_BME280 bme;
String serialCommand;

void setup() {
  wdt_enable(WDTO_8S);
  analogReference(INTERNAL2V56);
  pinMode(RfPDPin, OUTPUT);
  pinMode(GpsVccPin, OUTPUT);
  pinMode(RfPwrHLPin, OUTPUT);
  pinMode(RfPttPin, OUTPUT);
  pinMode(BattPin, INPUT);
  pinMode(PIN_DRA_TX,INPUT);

  RfOFF;
  GpsOFF;
  RfPwrLow;
  RfPttOFF;
  AprsPinInput;

  Serial.begin(115200);
  Serial1.begin(9600);
  #if defined(DEVMODE)
    Serial.println(F("Booting..."));
  #endif
      
  APRS_init(ADC_REFERENCE, OPEN_SQUELCH);
  APRS_setCallsign(CallSign,CallNumber);
  APRS_setDestination("APLIGA", 0);
  APRS_setMessageDestination("APLIGA", 0);
  APRS_setPath1("WIDE1", Wide1);
  APRS_setPath2("WIDE2", Wide2);
  APRS_useAlternateSymbolTable(alternateSymbolTable); 
  APRS_setSymbol(Symbol);
  //increase following value (for example to 500UL), default 350, if you experience packet loss/decode issues. 
  APRS_setPreamble(350UL);  
  APRS_setPathSize(pathSize);

  configDra818(Frequency);

  bmp.begin();

  // You can also pass in a Wire library object like &Wire2
  // status = bme.begin(0x76, &Wire2)
  status = bme.begin(BME280_ADDRESS_ALTERNATE);
  #if defined(DEVMODE)
    Serial.print(F("SensorID was: 0x")); Serial.println(bme.sensorID(),16);
    Serial.print(F("        ID of 0xFF probably means a bad address, a BMP 180 or BMP 085\n"));
    Serial.print(F("   ID of 0x56-0x58 represents a BMP 280,\n"));
    Serial.print(F("        ID of 0x60 represents a BME 280.\n"));
    Serial.print(F("        ID of 0x61 represents a BME 680.\n"));
  #endif
  
  if (!status) {
    Serial.println(F("Could not find a valid BME280 sensor, check wiring, address, sensor ID!"));
    //while (true) delay(10);
  } 

}

void loop() {
  wdt_reset();  
  //
  // Battery must be above min level; solar cell and super caps
  //
  if (readBatt() > BattMin) {

    //
    // Startup TX status (before GPS fix)
    //
    if(aliveStatus){

        #if defined(DEVMODE)
          Serial.println(F("Starting up..."));
        #endif
        sendStatus(StatusMessage);

        aliveStatus = false;
     }

    //
    // Update GPS telemtery
    //
    updateGpsData(1000);
    gpsDebug();

    //
    // Check GPS data
    //
    if ((gps.location.age() < 1000 || gps.location.isUpdated()) && gps.location.isValid()) {
      if (gps.satellites.isValid() && (gps.satellites.value() > 3)) {

        if (Dev_Failsafe) {
          Serial.println(F("==> DevMode for Failsafe"));
          initialLock = true;
          failsafe_mode(30);
          return;
        }

        gpsLock = true;
        if (!initialLock) {
          initialLock = true;
        }
        updatePosition();
        updateTelemetry();    
      
        //GpsOFF;
        //setGPS_PowerSaveMode();
        //GpsFirstFix=true;
        tempAltitude = gps.altitude.feet();
        if(!flyAltReached && tempAltitude > flyAlt && (bmp.readAltitude(SEALEVELPRESSURE_HPA) * 3.2808399) > flyAlt)
        { 
          //Make sure GPS and BME pressure agree we flew above the target altitude. If not we can trigger false flights during initial lock.
          #if defined(DEVMODE)
            Serial.println(F("==> Flight altitude reached!"));
          #endif
          flyAltReached = true;
        } else {
          if (Dev_FoxHunt) {
            Serial.println(F("==> DevMode for FoxHunt"));
            flyAltReached = true;
          }
        }
        //
        // Set APRS based on altitude
        //
        if (beaconViaARISS && gps.altitude.feet() > ARISSAlt){
          //Opportunistically attempt ARISS while very high altitude
          configureFreqbyAltitude();
        } else if(autoPathSizeHighAlt && gps.altitude.feet()>3000){
          //force to use high altitude settings (WIDE2-n)
          APRS_setPathSize(1);
        } else {
          //use default APRS settings
          APRS_setPathSize(pathSize);
        }
        
        // Send telemetry
        sendLocation();

        // Send status message based on status interval
        if(gps.time.minute() % 10 == 0){
          sleepSeconds(StatusWait);  // Wait before sending Status TX (after previous Telemetry message)
          sendStatus(StatusMessage); 
          sleepSeconds(30);          // Wait before sending Location TX (after Status message)
        } else {
          sleepSeconds(BeaconWait);  // Wait before sending TX next time
        }

        freeMem();
        Serial.flush();
        NoGPS = 0;

        if(flyAltReached && tempAltitude < foxhuntAlt && (bmp.readAltitude(SEALEVELPRESSURE_HPA) * 3.2808399) < foxhuntAlt) {
          #if defined(DEVMODE)  
            Serial.println(F("Sending FoxHunt"));
          #endif 
          sendFoxhunt(15);
          sendFoxhunt(15);
          sendFoxhunt(15);
        }

      } else {
        #if defined(DEVMODE)
          Serial.println(F("Not enough sattelites")); 
        #endif
        failsafe_mode(30);
      }
    } else {
      failsafe_mode(30);
    }
  } else {
    //
    // Sleep and wait for battery
    // 
    sleepSeconds(BattWait);
  }
}

void failsafe_mode(int sec){

  if (!gps.location.isUpdated() || !gps.location.isValid()) {
    gpsLock = false;
  }

  #if defined(DEVMODE)
    Serial.print(F("No GPS count: ")); Serial.println(NoGPS);
    Serial.print(F("GPS Lock: ")); Serial.println(gpsLock);
  #endif

  float pressure = bmp.readPressure() / 100.0; //Pa to hPa
  if ((pressure - LowestPressure == 50) && (pressure > LowestPressure)){
    ExpressReturn = true;
  }
  
  #if defined(DEVMODE)
    Serial.print(F("Current pressure: ")); Serial.println(pressure);
    Serial.print(F("Lowest pressure: ")); Serial.println(LowestPressure);
    Serial.print(F("Express route back to earth: ")); Serial.println(ExpressReturn);
  #endif

  NoGPS++;
  if ((NoGPS > 30 && initialLock) || ((ExpressReturn) && !gps.location.isUpdated() && !gps.location.isValid())) {
    char msg[100];
    char strpressure[7];
    dtostrf(bmp.readPressure() / 100.0, 4, 2, strpressure);
    sprintf(msg, "No Gps Lock: Time %02d-%02d-%02d %02d:%02d:%02d %s", gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(), gps.time.second(), strpressure); 
    sleepSeconds(sec);
    sendStatus(msg);
    //setGps_MaxPerformanceMode();  // Experimental   
  } 
 
}

void aprs_msg_callback(struct AX25Msg *msg) {
  //do not remove this function, necessary for LibAPRS
}

void sleepSeconds(int sec) {  
  //if(GpsFirstFix)GpsOFF;//sleep gps after first fix
  RfOFF;
  RfPttOFF;
  Serial.flush();
  wdt_disable();
  for (int i = 0; i < sec; i++) {
    //if(readBatt() < GpsMinVolt) GpsOFF;  //(for pico balloon only)
    LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_ON);   
  }
  wdt_enable(WDTO_8S);
}

byte configDra818(char *freq)
{
  SoftwareSerial Serial_dra(PIN_DRA_RX, PIN_DRA_TX);
  Serial_dra.begin(9600);
  RfON;
  char ack[3];
  int n;
  delay(2000);
  char cmd[50];
  sprintf(cmd, "AT+DMOSETGROUP=0,%s,%s,0000,4,0000", freq, freq);
  Serial_dra.println(cmd);
  ack[2] = 0;
  while (ack[2] != 0xa){
    if (Serial_dra.available() > 0) {
      ack[0] = ack[1];
      ack[1] = ack[2];
      ack[2] = Serial_dra.read();
    }
  }
  Serial_dra.end();
  RfOFF;
  pinMode(PIN_DRA_TX,INPUT);
  #if defined(DEVMODE)
    if (ack[0] == 0x30){
      Serial.println(F("Frequency set..."));
    } else {
      Serial.println(F("Frequency update error!"));
    }
  #endif
  return (ack[0] == 0x30) ? 1 : 0;
}

void configureFreqbyAltitude() {
  //
  // Opportunistically attempt ARISS ISS repeater, avoid loss of normal APRS telemetry
  //
  if (gps.time.minute() % ARISSWait == 0){
    // Setup ARISS APRS
    APRS_setPath1("ARISS", Wide1);
    APRS_setPath2("WIDE2", Wide2);
    APRS_setPathSize(2);
    configDra818("145.8250");
    arissModEnabled = true;
  } else {
    // Otherwise use auto altitude settting
    APRS_setPath1("WIDE1", Wide1);
    APRS_setPath2("WIDE2", Wide2);
    APRS_setPathSize(1);
    configDra818(Frequency);
    arissModEnabled = false;
  }
}


void updatePosition() {
  // Convert and set latitude NMEA string Degree Minute Hundreths of minutes ddmm.hh[S,N].
  char latStr[10];
  int temp = 0;

  double d_lat = gps.location.lat();
  double dm_lat = 0.0;

  if (d_lat < 0.0) {
    temp = -(int)d_lat;
    dm_lat = temp * 100.0 - (d_lat + temp) * 60.0;
  } else {
    temp = (int)d_lat;
    dm_lat = temp * 100 + (d_lat - temp) * 60.0;
  }

  dtostrf(dm_lat, 7, 2, latStr);

  if (dm_lat < 1000) {
    latStr[0] = '0';
  }

  if (d_lat >= 0.0) {
    latStr[7] = 'N';
  } else {
    latStr[7] = 'S';
  }

  APRS_setLat(latStr);

  // Convert and set longitude NMEA string Degree Minute Hundreths of minutes ddmm.hh[E,W].
  char lonStr[10];
  double d_lon = gps.location.lng();
  double dm_lon = 0.0;

  if (d_lon < 0.0) {
    temp = -(int)d_lon;
    dm_lon = temp * 100.0 - (d_lon + temp) * 60.0;
  } else {
    temp = (int)d_lon;
    dm_lon = temp * 100 + (d_lon - temp) * 60.0;
  }

  dtostrf(dm_lon, 8, 2, lonStr);

  if (dm_lon < 10000) {
    lonStr[0] = '0';
  }
  if (dm_lon < 1000) {
    lonStr[1] = '0';
  }

  if (d_lon >= 0.0) {
    lonStr[8] = 'E';
  } else {
    lonStr[8] = 'W';
  }

  APRS_setLon(lonStr);
}

// blank 
void blankline(char* msg, int msg_len){
  for (int i=1; i < msg_len; i++){
    msg[i] = ' ';
  }
}

void updateTelemetry() {
  //
  // Updating telemetry string - Max 67 chars
  //
  blankline(telemetry_buff, 67);
  sprintf(telemetry_buff, "%03d", gps.course.isValid() ? (int)gps.course.deg() : 0);
  telemetry_buff[3] += '/';
  sprintf(telemetry_buff + 4, "%03d", gps.speed.isValid() ? (int)gps.speed.knots() : 0);
  telemetry_buff[7] = '/';
  telemetry_buff[8] = 'A';
  telemetry_buff[9] = '=';
  sprintf(telemetry_buff + 10, "%06lu", (long)gps.altitude.feet());
  telemetry_buff[16] = ' ';
  sprintf(telemetry_buff + 17, "%03d", TxCount);
  telemetry_buff[20] = 'T';
  telemetry_buff[21] = 'x';
  telemetry_buff[22] = 'C';
  telemetry_buff[23] = ' '; float tempC = bmp.readTemperature();//-21.4;//
  dtostrf(tempC, 6, 2, telemetry_buff + 24);
  telemetry_buff[30] = 'C';
  telemetry_buff[31] = ' '; float pressure = bmp.readPressure() / 100.0; //Pa to hPa
  dtostrf(pressure, 7, 2, telemetry_buff + 32);
  telemetry_buff[39] = 'h';
//  telemetry_buff[40] = 'P';
//  telemetry_buff[41] = 'a';
  telemetry_buff[40] = ' ';
  dtostrf(readBatt(), 5, 2, telemetry_buff + 41);
  telemetry_buff[46] = 'V';
  telemetry_buff[47] = ' '; tempC = bme.readTemperature();//-21.4//
  dtostrf(tempC, 6, 2, telemetry_buff + 48);  
  telemetry_buff[54] = 'C';
  telemetry_buff[55] = ' '; pressure = bme.readPressure() / 100.0; //Pa to hPa
  dtostrf(pressure, 7, 2, telemetry_buff + 56);
  telemetry_buff[63] = 'h';
//  telemetry_buff[61] = ' '; float humidty = bme.readHumidity();// 21.1%
//  dtostrf(humidty, 4, 2, telemetry_buff + 62);
//  telemetry_buff[66] = '%';
//  telemetry_buff[67] = ' ';
  telemetry_buff[64] = ' ';
  sprintf(telemetry_buff + 65, "%s", comment);    

  #if defined(DEVMODE)
    Serial.print(F("Telemtery buff: "));
    Serial.println(telemetry_buff);
  #endif

  pressure = bme.readPressure() / 100.0; //Pa to hPa
  if (pressure < LowestPressure){
    LowestPressure = pressure;
  }
}

void sendLocation() {

  #if defined(DEVMODE)
    Serial.println(F("==> Sending location"));
  #endif
  if ((readBatt() > DraHighVolt) && (readBatt() < 10)){
    RfPwrHigh; //DRA Power 1 Watt
  } else {
    RfPwrLow; //DRA Power 0.5 Watt
  }

  int hh = gps.time.hour();
  int mm = gps.time.minute();
  int ss = gps.time.second();

  char timestamp_buff[7];

  sprintf(timestamp_buff, "%02d", gps.time.isValid() ? (int)gps.time.hour() : 0);
  sprintf(timestamp_buff + 2, "%02d", gps.time.isValid() ? (int)gps.time.minute() : 0);
  sprintf(timestamp_buff + 4, "%02d", gps.time.isValid() ? (int)gps.time.second() : 0);
  timestamp_buff[6] = 'h';

  #if defined(DEVMODE)
    Serial.print(F("Telemetry buffer length: "));
    Serial.println(String(strlen(telemetry_buff)));
    Serial.println(F("Turning RF ON"));
  #endif

  RfON;
  delay(2000);

  #if defined(DEVMODE)
    Serial.println(F("Turning on PTT"));
  #endif
  
  RfPttON;
  delay(1000);
  
  //APRS_sendLoc(telemetry_buff, strlen(telemetry_buff)); //beacon without timestamp
  APRS_sendLocWtTmStmp(telemetry_buff, strlen(telemetry_buff), timestamp_buff); //beacon with timestamp
  
  delay(10);
  while(digitalRead(1)){;}//LibAprs TX Led pin PB1
  delay(100);
  RfPttOFF;
  delay(50);
  RfOFF;

  #if defined(DEVMODE)
    Serial.print(F("Location sent (Freq: "));
    Serial.print(Frequency);
    Serial.print(F(") - "));
    Serial.println(TelemetryCount);
  #endif 

  TxCount+=1;
  TelemetryCount+=1;
}

void sendStatus(const char *msg) {

  #if defined(DEVMODE)
    Serial.println(F("==> Sending status"));
  #endif

  if ((readBatt() > DraHighVolt) && (readBatt() < 10)){ 
    RfPwrHigh; //DRA Power 1 Watt
  } else {
    RfPwrLow; //DRA Power 0.5 Watt
  }

  RfON;
  delay(2000);

  #if defined(DEVMODE)
    Serial.println(F("Turning PTT ON"));  
  #endif
  RfPttON;
  delay(1000);

  char batt[8];
  char status[105];
  dtostrf(readBatt(), 5, 2, batt); 
  // sprintf(msg, "No GPS Lock: Bat %5.2fv, Time %d-%d-%d %d:%d:%d, %4.0fM, %6.0fPa", readBatt(), gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(), gps.time.second(), gps.altitude.meters(), pressure);
  sprintf(status, "High Altitude Payload %sv, %003dTxC, %003dSxC, %003dTxC, %02dS, %05dH - ", batt, TxCount, StatusCount, TelemetryCount, gps.satellites.isValid() ? (int)gps.satellites.value() : 0, gps.hdop.isValid() ? (int)gps.hdop.value() : 0 );
  strcat(status, msg);

  #if defined(DEVMODE)
    Serial.println(F("Sending status message:"));
    Serial.println(status);
  #endif     

  delay(500);
  APRS_sendStatus(status, strlen(status));
  delay(10);

  while(digitalRead(1)){;}//LibAprs TX Led pin PB1
  delay(100);
  RfPttOFF;
  delay(50);
  RfOFF;

  #if defined(DEVMODE)
    Serial.print(F("Status sent (Freq: "));
    Serial.print(Frequency);
    Serial.print(F(") - "));
    Serial.println(StatusCount);
  #endif

  TxCount+=1;
  StatusCount+=1;

}

static void updateGpsData(int ms)
{
  GpsON;

  //if(!ublox_high_alt_mode){
      //enable ublox high altitude mode
      delay(100);
      setGPS_DynamicModel6();
      ublox_high_alt_mode = true;
   //}

  while (!Serial1) {
    delayMicroseconds(1); // wait for serial port to connect.
  }
    unsigned long start = millis();
    unsigned long bekle=0;
    do
    {
      while (Serial1.available()>0) {
        char c;
        c=Serial1.read();
        gps.encode(c);
        Serial.print(c);
        bekle= millis();
      }
      if (bekle!=0 && bekle+10<millis())break;
    } while (millis() - start < ms);

}

float readBatt() {
  float R1 = 560000.0; // 560K
  float R2 = 100000.0; // 100K
  float value = 0.0;
  do { 
    value =analogRead(BattPin);
    delay(5);
    value =analogRead(BattPin);
    value=value-8;
    value = (value * 2.56) / 1024.0;
    value = value / (R2/(R1+R2));
  } while (value > 16.0);
  return value ;
}

void freeMem() {
#if defined(DEVMODE)
  Serial.print(F("Free RAM: ")); Serial.print(freeMemory()); Serial.println(F(" byte"));
#endif
}

void sendFoxhunt(int secDuration) {

  #if defined(DEVMODE)
    Serial.println(F("==> Sending Foxhunt"));
  #endif
  RfPwrLow; //DRA Power 0.5 Watt

  int hh = gps.time.hour();
  int mm = gps.time.minute();
  int ss = gps.time.second();

  char timestamp_buff[7];

  sprintf(timestamp_buff, "%02d", gps.time.isValid() ? (int)gps.time.hour() : 0);
  sprintf(timestamp_buff + 2, "%02d", gps.time.isValid() ? (int)gps.time.minute() : 0);
  sprintf(timestamp_buff + 4, "%02d", gps.time.isValid() ? (int)gps.time.second() : 0);
  timestamp_buff[6] = 'h';

  #if defined(DEVMODE)
    Serial.print(F("Telemetry buffer length: "));
    Serial.println(String(strlen(telemetry_buff)));
    Serial.println(F("Turning RF ON"));
  #endif

  RfON;
  delay(2000);

  #if defined(DEVMODE)
    Serial.println(F("Turning on PTT"));
  #endif
  
  RfPttON;
  delay(1000);
  
  //APRS_sendLoc(telemetry_buff, strlen(telemetry_buff)); //beacon without timestamp
  updatePosition();
  updateTelemetry();
  APRS_sendLocWtTmStmp(telemetry_buff, strlen(telemetry_buff), timestamp_buff); //beacon with timestamp
  delay(500);

  //APRS_sendLoc(telemetry_buff, strlen(telemetry_buff)); //beacon without timestamp
  updatePosition();
  updateTelemetry();
  APRS_sendLocWtTmStmp(telemetry_buff, strlen(telemetry_buff), timestamp_buff); //beacon with timestamp 
  delay(500);

  //APRS_sendLoc(telemetry_buff, strlen(telemetry_buff)); //beacon without timestamp
  updatePosition();
  updateTelemetry();
  APRS_sendLocWtTmStmp(telemetry_buff, strlen(telemetry_buff), timestamp_buff); //beacon with timestamp
  delay(500);

  delay(10);
  while(digitalRead(1)){;}//LibAprs TX Led pin PB1
  delay(100);
  RfPttOFF;
  delay(50);
  RfOFF;

  #if defined(DEVMODE)
    Serial.print(F("Foxhunt sent (Freq: "));
    Serial.print(Frequency);
    Serial.println(F(")"));
  #endif

  sleepSeconds(secDuration);

}

void gpsDebug() {
#if defined(DEVMODE)
  Serial.println();
  Serial.println(F("Sats HDOP Latitude   Longitude   Fix  Date       Time     Date Alt    Course Speed Card Chars Sentences Checksum"));
  Serial.println(F("          (deg)      (deg)       Age                      Age  (m)    --- from GPS ----  RX    RX        Fail"));
  Serial.println(F("-----------------------------------------------------------------------------------------------------------------"));

  printInt(gps.satellites.value(), gps.satellites.isValid(), 5);
  printInt(gps.hdop.value(), gps.hdop.isValid(), 5);
  printFloat(gps.location.lat(), gps.location.isValid(), 11, 6);
  printFloat(gps.location.lng(), gps.location.isValid(), 12, 6);
  printInt(gps.location.age(), gps.location.isValid(), 5);
  printDateTime(gps.date, gps.time);
  printFloat(gps.altitude.meters(), gps.altitude.isValid(), 7, 2);
  printFloat(gps.course.deg(), gps.course.isValid(), 7, 2);
  printFloat(gps.speed.kmph(), gps.speed.isValid(), 6, 2);
  printStr(gps.course.isValid() ? TinyGPSPlus::cardinal(gps.course.value()) : "*** ", 6);

  printInt(gps.charsProcessed(), true, 6);
  printInt(gps.sentencesWithFix(), true, 10);
  printInt(gps.failedChecksum(), true, 9);
  Serial.println();

#endif
}

static void printFloat(float val, bool valid, int len, int prec)
{
#if defined(DEVMODE)
  if (!valid)
  {
    while (len-- > 1)
      Serial.print('*');
    Serial.print(' ');
  }
  else
  {
    Serial.print(val, prec);
    int vi = abs((int)val);
    int flen = prec + (val < 0.0 ? 2 : 1); // . and -
    flen += vi >= 1000 ? 4 : vi >= 100 ? 3 : vi >= 10 ? 2 : 1;
    for (int i = flen; i < len; ++i)
      Serial.print(' ');
  }
#endif
}

static void printInt(unsigned long val, bool valid, int len)
{
#if defined(DEVMODE)
  char sz[32] = "*****************";
  if (valid)
    sprintf(sz, "%ld", val);
  sz[len] = 0;
  for (int i = strlen(sz); i < len; ++i)
    sz[i] = ' ';
  if (len > 0)
    sz[len - 1] = ' ';
  Serial.print(sz);
#endif
}

static void printDateTime(TinyGPSDate &d, TinyGPSTime &t)
{
#if defined(DEVMODE)
  if (!d.isValid())
  {
    Serial.print(F("********** "));
  }
  else
  {
    char sz[32];
    sprintf(sz, "%02d/%02d/%02d ", d.month(), d.day(), d.year());
    Serial.print(sz);
  }

  if (!t.isValid())
  {
    Serial.print(F("******** "));
  }
  else
  {
    char sz[32];
    sprintf(sz, "%02d:%02d:%02d ", t.hour(), t.minute(), t.second());
    Serial.print(sz);
  }

  printInt(d.age(), d.isValid(), 5);
#endif
}

static void printStr(const char *str, int len)
{
#if defined(DEVMODE)
  int slen = strlen(str);
  for (int i = 0; i < len; ++i)
    Serial.print(i < slen ? str[i] : ' ');
#endif
}

//following GPS code from : https://github.com/HABduino/HABduino/blob/master/Software/habduino_v4/habduino_v4.ino
void setGPS_DynamicModel6()
{
  int gps_set_sucess=0;
  uint8_t setdm6[] = {
    0xB5, 0x62, 0x06, 0x24, 0x24, 0x00, 0xFF, 0xFF, 0x06,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
    0x05, 0x00, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 0x2C,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0xDC };
  while(!gps_set_sucess)
  {
    sendUBX(setdm6, sizeof(setdm6)/sizeof(uint8_t));
    gps_set_sucess=getUBX_ACK(setdm6);
  }
}

void setGPS_DynamicModel3()
{
  int gps_set_sucess=0;
  uint8_t setdm3[] = {
    0xB5, 0x62, 0x06, 0x24, 0x24, 0x00, 0xFF, 0xFF, 0x03,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
    0x05, 0x00, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 0x2C,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x76 };
  while(!gps_set_sucess)
  {
    sendUBX(setdm3, sizeof(setdm3)/sizeof(uint8_t));
    gps_set_sucess=getUBX_ACK(setdm3);
  }
}

void setGps_MaxPerformanceMode() {
  //Set GPS for Max Performance Mode
  uint8_t setMax[] = { 
    0xB5, 0x62, 0x06, 0x11, 0x02, 0x00, 0x08, 0x00, 0x21, 0x91 }; // Setup for Max Power Mode
  sendUBX(setMax, sizeof(setMax)/sizeof(uint8_t));
}

void sendUBX(uint8_t *MSG, uint8_t len) {
  Serial1.flush();
  Serial1.write(0xFF);
  _delay_ms(500);
  for(int i=0; i<len; i++) {
    Serial1.write(MSG[i]);
  }
}

boolean getUBX_ACK(uint8_t *MSG) {
  uint8_t b;
  uint8_t ackByteID = 0;
  uint8_t ackPacket[10];
  unsigned long startTime = millis();
 
  // Construct the expected ACK packet
  ackPacket[0] = 0xB5; // header
  ackPacket[1] = 0x62; // header
  ackPacket[2] = 0x05; // class
  ackPacket[3] = 0x01; // id
  ackPacket[4] = 0x02; // length
  ackPacket[5] = 0x00;
  ackPacket[6] = MSG[2]; // ACK class
  ackPacket[7] = MSG[3]; // ACK id
  ackPacket[8] = 0; // CK_A
  ackPacket[9] = 0; // CK_B
 
  // Calculate the checksums
  for (uint8_t ubxi=2; ubxi<8; ubxi++) {
    ackPacket[8] = ackPacket[8] + ackPacket[ubxi];
    ackPacket[9] = ackPacket[9] + ackPacket[8];
  }
  while (1) {
    // Test for success
    if (ackByteID > 9) {
      // All packets in order!
      return true;
    } 
    // Timeout if no valid response in 3 seconds
    if (millis() - startTime > 3000) {
      return false;
    }
    // Make sure data is available to read
    if (Serial1.available()) {
      b = Serial1.read();
      // Check that bytes arrive in sequence as per expected ACK packet
      if (b == ackPacket[ackByteID]) {
        ackByteID++;
      } else {
        ackByteID = 0; // Reset and look again, invalid order
      }
    }
  }
}

void setGPS_PowerSaveMode() {
  // Power Save Mode 
  uint8_t setPSM[] = { 
    0xB5, 0x62, 0x06, 0x11, 0x02, 0x00, 0x08, 0x01, 0x22, 0x92 }; // Setup for Power Save Mode (Default Cyclic 1s)
    sendUBX(setPSM, sizeof(setPSM)/sizeof(uint8_t));
}

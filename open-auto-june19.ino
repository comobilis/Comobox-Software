// This #include statement was automatically added by the Particle IDE.
#include "OpenAutoRoutines.h"

//Open Auto code GPL V3
// This #include statement was automatically added by the Particle IDE.
#include <ParticleSoftSerial.h>
#include <Particle-GPS.h>
#include "Particle.h"
#define RECEIVER SoftSer
#define PROTOCOL SERIAL_8N1

const int MAX_RESERVATIONS = 145;  //blocks of 10 minute lookahead for reservations to cover 24 hours.
const int ReservationResetClock=0; //0 = midnight
byte ReservationCtr =0; //initalise the reservation counter on boot
int CarType = 2; //default car type

//set baud rate for software serial
const uint32_t baud = 9600;

//Pin asignments
int OpenRelay = D4;
int CloseRelay = D5;
int StatusLED = D7;

//temp variable for the RFID card last read value
String CardReadStr;

//init GPS device on Serial1
Gps _gps = Gps(&Serial1);

//Create a 1ms timer to feed in the GPS data
Timer _timer = Timer(1, onSerialData);

#define PSS_RX D3 // RX must be interrupt enabled (on Photon/Electron D0/A5 are not)
//#define PSS_TX null //this pin isn't something we use, I couldn't find a null pin so I just used the number 1.
ParticleSoftSerial SoftSer(PSS_RX, 1); //note this is a work-around so that the code works on a Particle Photon (development) and an Electron (deployment) 

//Reservation structure for incoming reservations
struct RemoteReservation {
    char UserID[9]; //the RFID key to unlock the doors with a null terminator, stored as text
	int Start; //block number for start of reservation
	int Duration; //the number of 15 minute slots. Max reservation is 1425 minutes (can be handled upstream by odoo to block out more time in multiple grouped reservations)
	
};

//Reservation structure for local data storage in EEPROM
struct LocalReservation {
    char UserID[4]; //stored as hex
	};
	
int ReservationSize = 4; //number of bytes per reservation card code

//variable for RFID card reading process
char CARDcurrent[4];

//boolean to track the door status; actually we'll only use it as a toggle. 
bool DoorsOpenBool = false;

String ValidGPSPosition;
int BatteryVoltageRead;
double BatteryVoltage;

//define a master RFID card which is loaded either from the EEPROM (on boot) or via the config command.
String MasterRFIDCard = ""; //example = "983553fe";
char RFIDpass[8];

//define a variable for the currently active reservation card number and a boolean to say if it's live or not
bool ActiveReservation = false; //Active if the reservation is within it's time period.
String ReservationRFIDCard = "";


//parse reservations
class ReservationCmd {
  String argument;
public:
  void extractValues(String);
  String UserIDStr (void) {
    return argument.substring(argument.indexOf("UserID=")+7, argument.indexOf("Start="));
  }
  byte StartInt (void) {
    return (argument.substring(argument.indexOf("Start=")+6, argument.indexOf("Dur="))).toInt();
  }
  byte DurationInt (void) {
    return (argument.substring(argument.indexOf("Dur=")+4, argument.indexOf("f"))).toInt();
  }
};
void ReservationCmd::extractValues (String stringPassed){
  argument = stringPassed;
}

//parse configuration commands
class ConfigCmd {
  String argument;
public:
  void extractValues(String);
  String MasterRFIDStr (void) {
    return argument.substring(argument.indexOf("MasterRFID=")+11, argument.indexOf("CarType="));
  }
  int CarTypeInt (void) {
    return (argument.substring(argument.indexOf("CarType=")+8, argument.indexOf("f"))).toInt();
  }
//eventually add some more things here?
};
void ConfigCmd::extractValues (String stringPassed){
  argument = stringPassed;
}



int AddReservation(int ReservationToAdd, char Start, char Duration, int Odometer)
{ //note we added one to the ReservationCtr variable before we started this sub
    //work out the next free spot and put it in that reservation
    EEPROM.put((ReservationCtr), Start);
    EEPROM.put((ReservationCtr+150), Duration);
    EEPROM.put((300+(ReservationCtr*ReservationSize)), ReservationToAdd);
    EEPROM.put((900+(ReservationCtr*ReservationSize)),0); //write 0s to the Odo register
    
    //now update the reservation hash table, stored in the first 96 byte blocks from 1000.
    for (int j = 0; j < Duration; j++)
    {
    EEPROM.put(1500+((Start+j)), ReservationCtr);
    }

    //re-write the counter back to eeprom for synchronisation, which we store at the end of the memory space.
    EEPROM.put(2043,ReservationCtr);

    return ReservationCtr;//return the reservation number
}

void clearEEPROM();

void setup()
{  Serial.begin(9600);
   debug("Running setup",1);
   Serial.println("Setup running...");
    //set pin modes for hardwired devices
   pinMode(OpenRelay, OUTPUT);
   pinMode(CloseRelay, OUTPUT);
   pinMode(StatusLED, OUTPUT);
   pinMode(A1,AN_INPUT);
   //set these outputs correctly
   digitalWrite(OpenRelay, LOW);
   digitalWrite(CloseRelay, LOW);
   digitalWrite(StatusLED, LOW);
    
   
   //define functions
   Particle.function("Open", OpenRelayCmd);
   Particle.function("Close", CloseRelayCmd);
   Particle.function("StatusLED", StatusLEDFlash);
   Particle.function("MkResvn", ReserveString);
   Particle.function("PubResvn", PubResvn);
   Particle.function("ConfigCar", ConfigString);
   Particle.function("HomeBox", HomeCoords);
   Particle.function("WipeResvn", WipeReservations);
 
   //define variables
   Particle.variable("CarPos", ValidGPSPosition);
   Particle.variable("BatLevel", BatteryVoltage);  
   
  //set up serial debug link
  //Serial.begin();
  Serial.printlnf("ready for data");
  //setup RFID reader soft serial
  RECEIVER.begin(baud, PROTOCOL); 
  
  //Init GPS  
  _gps.begin(9600);
  _gps.sendCommand(PMTK_SET_NMEA_OUTPUT_ALLDATA);
  _timer.start();

  //retrive the master RFID card  
  EEPROM.get(2020, RFIDpass);
  MasterRFIDCard = "";
  MasterRFIDCard = MasterRFIDCard + RFIDpass[0];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[1];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[2];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[3];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[4];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[5];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[6];
  MasterRFIDCard = MasterRFIDCard + RFIDpass[7];
  Serial.print("MasterRFID set:");
  Serial.print(MasterRFIDCard);
  debug(MasterRFIDCard,1);
  //set the car type from eeprom
  EEPROM.get(2038, CarType);
  //debug(CarType,1);  
  
  //recall the reservation pointer on power up/reset
  EEPROM.get(2010, ReservationCtr); //how many reservations are there
  
  
 }


void onSerialData()
{
  _gps.onSerialData();
}


void loop()
{

    
//gps stuff    
  Rmc rmc = Rmc(_gps);
  ValidGPSPosition = rmc.latitude + ";" + rmc.northSouthIndicator + ";" + rmc.longitude + ";" + rmc.eastWestIndicator + ";" + rmc.utcTime;
  BatteryVoltageRead = analogRead(A1);
  BatteryVoltage = (((BatteryVoltageRead * 3.3)/4096)/.26);

int counter = 0;
unsigned long cardread = 0;

//read the input from the card reader and open/close the doors if valid
while (RECEIVER.available())
  {    //Particle.process();
       //delay(500);
      for (int cardident = 0; cardident < 5; cardident++) {
      counter = (RECEIVER.read());
      if (cardident > 0) Serial.print(int(counter), HEX);
      CARDcurrent[cardident-1] = counter;
      //Serial.print(" ");
      }
      //Serial.println("");
      cardread=int(CARDcurrent[0]);
      cardread=cardread << 8;
      cardread=cardread+ int(CARDcurrent[1]);
      cardread=cardread << 8;
      cardread=cardread+ int(CARDcurrent[2]);
      cardread=cardread << 8;
      cardread=cardread+ int(CARDcurrent[3]);
      CardReadStr = String(cardread, HEX);
      Serial.println("card string");
      Serial.println(cardread);
      bool success;

      if (String(cardread, HEX)==MasterRFIDCard) {
        debug("MasterRFIDused",1);
        //Serial.println("ReservationRFIDused");
        StatusLEDFn();
          if (DoorsOpenBool) {
              DoorCloseFn();
              Serial.println("closing");
          }
          else
          {
              DoorOpenFn();
              Serial.println("Opening");
          }
      }

      if ((String(cardread, HEX)==ReservationRFIDCard)) {
        debug("ReservationRFIDused",1);
        Serial.println("ReservationRFIDused");
        StatusLEDFn();
          if (DoorsOpenBool) {
              DoorCloseFn();
              Serial.println("closing");
          }
          else
          {
              DoorOpenFn();
              Serial.println("Opening");
          }
      }

      success = Particle.publish("RFIDident", String(cardread, HEX), 0, PUBLIC);
      if (!success) {
      Serial.println("failedtopublish");  // get here if event publish did not work
      }

  }
    RECEIVER.flush();

    if (Particle.connected()) {
        Serial.println("Connected!");
      }
      
    //put in a delay to prevent double reads  
    delay(1000);
}


//Allow the doors to be opened from Particle Cloud
int OpenRelayCmd(String command) {
    if (command=="on") {
    DoorOpenFn();
        return 1;
    }
    else {
        return -1;
    }
}


//Allow the doors to be closed from Particle Cloud
int CloseRelayCmd(String command) {
    if (command=="on") {
       DoorCloseFn();
       return 1;
    }
    else {
        return -1;
    }
}

//Allow the status LED to be flashed from Particle Cloud
int StatusLEDFlash(String command) {
    if (command=="on") {
    StatusLEDFn();
        return 1;
    }
    else {
        return -1;
    }
    
}

//Configure car from Particle Cloud
int ConfigString(String ConfigmssgArgs){

  Serial.println("Config command recieved...");
  Serial.println(ConfigmssgArgs);
  ConfigCmd command;
  command.extractValues(ConfigmssgArgs);
  Serial.println("Valid data received...");
  Serial.print("MasterRFID: ");
  Serial.println(command.MasterRFIDStr());
  debug(command.MasterRFIDStr(),1);
  Serial.println("CarType = ");
  Serial.println(command.CarTypeInt());
  MasterRFIDCard=command.MasterRFIDStr();
  CarType = command.CarTypeInt();
  char MasterRFIDstore[8];
  MasterRFIDstore[0] = MasterRFIDCard[0];
  MasterRFIDstore[1] = MasterRFIDCard[1];
  MasterRFIDstore[2] = MasterRFIDCard[2];
  MasterRFIDstore[3] = MasterRFIDCard[3];
  MasterRFIDstore[4] = MasterRFIDCard[4];
  MasterRFIDstore[5] = MasterRFIDCard[5];
  MasterRFIDstore[6] = MasterRFIDCard[6];
  MasterRFIDstore[7] = MasterRFIDCard[7];
  EEPROM.put(2020, MasterRFIDstore);
  EEPROM.put(2038, CarType);
return 1;
}

int PubResvn(String mssgArgs){
byte ResToPub;
ResToPub = mssgArgs.toInt();
char RFIDVal[8];
byte ExportVal;
int DistanceCovered;
EEPROM.get(300+(ResToPub*4),RFIDVal);
Particle.publish("Reservation RFID", RFIDVal, PUBLIC);


//    EEPROM.put((ReservationCtr), Start);
//    EEPROM.put((ReservationCtr+150), Duration);
//    EEPROM.put((300+(ReservationCtr*ReservationSize)), ReservationToAdd);
//    EEPROM.put((900+ReservationCtr*ReservationSize),0); //write 0s to the Odo register

EEPROM.get(ResToPub,ExportVal);
sprintf(RFIDVal, "%d", ExportVal);
Particle.publish("Reservation Start", RFIDVal, PUBLIC);

EEPROM.get(ResToPub+150,ExportVal);
sprintf(RFIDVal, "%d", ExportVal);
Particle.publish("Reservation Duration", RFIDVal, PUBLIC);

EEPROM.get(900+(ResToPub*4),DistanceCovered);
sprintf(RFIDVal, "%d", DistanceCovered);
Particle.publish("Reservation ODO", RFIDVal, PUBLIC);
return ResToPub;

}

//Store a reservation
int ReserveString(String mssgArgs){
    StatusLEDFn();
int StartSlot;
int DurationSlot;
String ResCardId;  //text string for rfid card
int RFIDRes; //hex numeric rfid card string
String StartString;
String DurationString;

  Serial.println("command received...");
  Serial.println(mssgArgs);
  ReservationCmd command;
  command.extractValues(mssgArgs);
  Serial.println("Valid reservation received...");
  Serial.print("UserID: ");
  Serial.println(command.UserIDStr());
  debug(command.UserIDStr(),1);
  Serial.println("Start = ");
  Serial.println(command.StartInt());
  Serial.println("Duration = ");
  Serial.println(command.DurationInt());

    //is reservation valid?
    ResCardId= command.UserIDStr();
    StartSlot = command.StartInt();
    //sprintf(StartString, StartSlot);
    DurationSlot = command.DurationInt();
    //sprintf(DurationString, DurationSlot);
    if ((StartSlot >= 0) * (StartSlot < 145))
    {
        if ((DurationSlot >= 0 ) * (DurationSlot <145))
        {
        //note there's no clash detection for reservations on the device! We assume it's done upstream
        debug("Reservation ok",1);
        //debug("Start Index", StartString);
        //debug("End String", DurationString);
        ReservationCtr++;
        //command.UserIDStr();
        
        RFIDRes=ConvertToHex(ResCardId[0])>>28;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[1])>>24;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[2])>>20;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[3])>>16;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[4])>>12;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[5])>>8;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[6])>>4;
        RFIDRes=RFIDRes+ConvertToHex(ResCardId[7]);
        

        AddReservation(RFIDRes, char(StartSlot), char(DurationSlot), 0);
        debug("Reservation stored",1);
        
        //ReservationCtr++; //increment the number of reservations.
        
        
                
        }
    }
return ReservationCtr;
    StatusLEDFn();
}

int HomeCoords(String command){
    if (command=="on") {
        digitalWrite(StatusLED,HIGH);
        delay(1000);
        digitalWrite(StatusLED,LOW);
        return 1;
    }
    else {
        return -1;
    }
}

int WipeReservations(String command) {
    //wipes all reservations, retains MasterRFID and CarType variables
    char TempMasterRFID[8];
    int TempCarType;
    //status complete
    if (command=="on") {
    //flash the LED    
    StatusLEDFn();
    //get the fixed variables
    EEPROM.get(2020, TempMasterRFID);
    EEPROM.get(2038, TempCarType);
    debug("EEPROM about to be wiped",1);
    debug(TempMasterRFID,1);
    //Wipe the EEPROM
    clearEEPROM();
    //Reset the onboard reservation counter to 0 (assuming EEPROM is wiped)
    EEPROM.put(2020, TempMasterRFID);
    EEPROM.put(2038, TempCarType);
    debug("EEPROM Wipe complete",1);
    debug(TempMasterRFID,1);
        return 1;
    }
    else {
        return -1;
    }
}
 
 
//print messages to the console
void debug(String message, int value)
{
    char msg [50];
    sprintf(msg, message.c_str(), value);
    Particle.publish("DEBUG", msg);
}

void DoorOpenFn() //open the doors, set the door status to open
{
    switch (CarType)
    {
    case 1:
    digitalWrite(OpenRelay,HIGH);
    delay(400);
    digitalWrite(OpenRelay,LOW);
    break;
    case 2:
    digitalWrite(OpenRelay,HIGH);
    delay(400);
    digitalWrite(OpenRelay,LOW);
    case 3:
    digitalWrite(OpenRelay,HIGH);
    delay(400);
    digitalWrite(OpenRelay,LOW);
    delay(200);
    digitalWrite(OpenRelay,HIGH);
    delay(400);
    digitalWrite(OpenRelay,LOW);
    break;
    default:
    digitalWrite(OpenRelay,HIGH);
    delay(400);
    digitalWrite(OpenRelay,LOW);
    }
    DoorsOpenBool = true;
    debug("Opened Doors",1);
    //return true;
}

void DoorCloseFn() //close the doors, set the door open state to false
{   
    switch (CarType)
    {
    case 1:
    digitalWrite(OpenRelay,HIGH);
    delay(400);
    digitalWrite(OpenRelay,LOW);
    break;
    case 2:
    digitalWrite(CloseRelay,HIGH);
    delay(400);
    digitalWrite(CloseRelay,LOW);
    case 3:
    digitalWrite(CloseRelay,HIGH);
    delay(400);
    digitalWrite(CloseRelay,LOW);
    delay(200);
    digitalWrite(CloseRelay,HIGH);
    delay(400);
    digitalWrite(CloseRelay,LOW);
    break;
    default:
    digitalWrite(CloseRelay,HIGH);
    delay(400);
    digitalWrite(CloseRelay,LOW);
    }
    DoorsOpenBool = false;
    debug("Closed Doors",1);
 
}

void StatusLEDFn() //flash the status led once.
{
    digitalWrite(StatusLED,HIGH);
    delay(1000);
    digitalWrite(StatusLED,LOW);
}




char ConvertToHex(char inputcharacter)
{
    char outputchar;
    switch (int(inputcharacter))
    {
    case 48: //0
    outputchar = char(0);
    break;
    case 49: //1
    outputchar = char(1);
    break;
    case 50: //2
    outputchar = char(2);
    break;
    case 51: //3
    outputchar = char(3);
    break;
    case 52: //4
    outputchar = char(4);
    break;
    case 53: //5
    outputchar = char(5);
    break;
    case 54: //6
    outputchar = char(6);
    break;
    case 55: //7
    outputchar = char(7);
    break;
    case 56:  //8
    outputchar = char(8);
    break;
    case 57: //9
    outputchar = char(9);
    break;
    case 65: //A
    outputchar = char(10);
    break;
    case 66: //B
    outputchar = char(11);
    break;
    case 67: //C
    outputchar = char(12);
    break;
    case 68: //D
    outputchar = char(13);
    break;
    case 69: //E
    outputchar = char(14);
    break;
    case 70: //F
    outputchar = char(15);
    break;
    case 71: //a
    outputchar = char(10);
    break;
    case 72: //b
    outputchar = char(11);
    break;
    case 73: //c
    outputchar = char(12);
    break;
    case 74: //d
    outputchar = char(13);
    break;
    case 75: //e
    outputchar = char(14);
    break;
    case 76: //f
    outputchar = char(15);
    break;
    //default
    
    }
} 

void clearEEPROM() {
	for(int addr = 0; addr < 2047; addr++) {
		EEPROM.write(addr, 0);
	}
}



    

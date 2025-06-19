#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_wifi.h"
#include "esp_system.h"

// Debug flag - set to true for verbose debugging
#define DEBUG_MODE true

// Define I2C pins (use default ESP32 pins unless you've connected to different ones)
#define SDA_PIN 21
#define SCL_PIN 22

// Define reader input pins
// card reader DATA0
#define DATA0 19
// card reader DATA1
#define DATA1 18

//define reader output pins
// LED Output for a GND tie back
#define LED 32
// Speaker Output for a GND tie back
#define SPK 33

//define relay modules
#define RELAY1 25
#define RELAY2 26

// card reader config and variables
// max number of bits
#define MAX_BITS 100
// time to wait for another weigand pulse
#define WEIGAND_WAIT_TIME 500 // Reduced from 3000ms to 500ms for faster response

// Forward declarations for structs
struct CardData;
struct Credential;

// Try multiple common LCD addresses - the code will find the correct one
LiquidCrystal_I2C lcd_0x27(0x27, 20, 4); // Most common address
LiquidCrystal_I2C lcd_0x3F(0x3F, 20, 4); // Second most common
LiquidCrystal_I2C lcd_0x20(0x20, 20, 4); // Your original address

// Pointer to the LCD that works
LiquidCrystal_I2C* lcd = NULL;

WebServer server(80);
Preferences preferences;

// general device settings
bool isCapturing = true;
String MODE = "CTF"; // Default mode

// stores all of the data bits
volatile unsigned char databits[MAX_BITS];
volatile unsigned int bitCount = 0;

// stores the last written card's data bits
unsigned char lastWrittenDatabits[MAX_BITS];
unsigned int lastWrittenBitCount = 0;

// goes low when data is currently being captured
volatile unsigned char flagDone;

// countdown until we assume there are no more bits
volatile unsigned int weigandCounter;

// Display screen timer
unsigned long displayTimeout = 30000;  // 30 seconds
unsigned long lastCardTime = 0;
bool displayingCard = false;

// WiFi monitoring variables
unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 30000; // Check every 30 seconds

// Wifi Settings
bool APMode = true;

// AP Settings
String ap_ssid = "doorsim";
String ap_passphrase = "";
int ap_channel = 1;
int ssid_hidden = 0;

// Speaker and LED Settings
int spkOnInvalid = 1;
int spkOnValid = 1;
int ledValid = 1;

// Custom Display Message
String customWelcomeMessage = "";
String welcomeMessageSelect = "default";
String customFlagMessage = "";
String flagMessageSelect = "default";

// decoded facility code and card code
unsigned long facilityCode = 0;
unsigned long cardNumber = 0;

// hex data string
String hexCardData;

// raw data string
String rawCardData;
String status;
String details;

// Add a global flag to help manage interrupt status
volatile bool interruptsAttached = false;

// Debug variable to track card reading
volatile bool cardBeingRead = false;

// breaking up card value into 2 chunks to create 10 char HEX value
volatile unsigned long bitHolder1 = 0;
volatile unsigned long bitHolder2 = 0;
unsigned long cardChunk1 = 0;
unsigned long cardChunk2 = 0;

// Wiegand processing state variables - added for debugging
volatile unsigned long lastInterruptTime = 0;
volatile int interruptCount = 0;

// Debug timeout variables
unsigned long setupStartTime = 0;
const unsigned long SETUP_TIMEOUT = 60000; // 60 seconds timeout for setup

// store card data for later review
struct CardData {
  unsigned int bitCount;
  unsigned long facilityCode;
  unsigned long cardNumber;
  String hexCardData;
  String rawCardData;
  String status;   // Add status field
  String details;  // Add details field
};

// store card data for later review
struct Credential {
  unsigned long facilityCode;
  unsigned long cardNumber;
  char name[50]; // Use a fixed-size array for the name
};

const int MAX_CREDENTIALS = 100;
Credential credentials[MAX_CREDENTIALS];
int validCount = 0;

// maximum number of stored cards
const int MAX_CARDS = 100;
CardData cardDataArray[MAX_CARDS];
int cardDataIndex = 0;

// Function declarations
void ledOnValid();
void speakerOnValid();
void speakerOnFailure();
void lcdInvalidCredentials();
void printWelcomeMessage();
void scanI2C();
void saveSettingsToPreferences();
void saveCredentialsToPreferences();
void processCardData();
void printCardData();
void clearDatabits();
void cleanupCardData();
bool shouldSkipProcessing();
void processCardNow();
void printAllCardData();
const Credential *checkCredential(uint16_t fc, uint16_t cn);
String centreText(const String &text, int width);
String prefixPad(const String &in, const char c, const size_t len);

// HTML for the web interface
const char *index_html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Card Data</title>
    <style>
        body {
            font-family: Arial, sans-serif;
        }
        header {
            background-color: #333;
            color: white;
            padding: 10px;
            text-align: center;
        }
        header a {
            color: white;
            margin: 0 15px;
            text-decoration: none;
            cursor: pointer;
        }
        cardTable tbody:nth-child(2) {
           text-decoration: none;
           cursor: pointer;
           target: _blank;
        }
        a:hover {
            text-decoration: underline;
        }
        .content {
            padding: 20px;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            margin-bottom: 20px;
        }
        th, td {
            padding: 8px;
            text-align: left;
            border-bottom: 1px solid #ddd;
        }
        th {
            background-color: #f2f2f2;
        }
        .hidden {
            display: none;
        }
        .collapsible {
            cursor: pointer;
        }
        .contentCollapsible {
            display: none;
            overflow: hidden;
        }
        .authorized {
            background-color: #90EE90;
        }
        .unauthorized {
            background-color: #FF4433;
        }
        .inputRow td {
            padding: 4px;
        }
        .inputRow input {
            width: 100%;
            box-sizing: border-box;
        }
        #customFlagMessage {
            margin-top: 10px;
        }
    </style>
</head>
<body>
    <header>
        <a onclick="showSection('lastRead')">Last Read</a>
        <a onclick="showSection('ctfMode')">CTF Mode</a>
        <a onclick="showSection('settings')">Settings</a>
    </header>
    <div class="content">
        <div id="lastRead">
            <h1>Card Data</h1>
            <table id="cardTable">
                <thead>
                    <tr>
                        <th>#</th>
                        <th>Bit Length</th>
                        <th>Facility Code</th>
                        <th>Card Number</th>
                        <th>Hex Data</th>
                        <th>Raw Data</th>
                    </tr>
                </thead>
                <tbody>
                </tbody>
            </table>
        </div>
        <div id="ctfMode" class="hidden">
            <h1>CTF Mode</h1>
            <h2>Last Read Cards</h2>
            <table id="lastReadCardsTable">
                <thead>
                    <tr>
                        <th>#</th>
                        <th>Status</th>
                        <th>Details</th>
                    </tr>
                </thead>
                <tbody>
                </tbody>
            </table>
            <h2 class="collapsible" onclick="toggleCollapsible()">Card Data (click to expand/collapse)</h2>
            <div class="contentCollapsible">
                <table id="userTable">
                    <thead>
                        <tr>
                            <th>#</th>
                            <th>Facility Code</th>
                            <th>Card Number</th>
                            <th>Name</th>
                            <th>Action</th>
                        </tr>
                    </thead>
                    <tbody>
                    </tbody>
                </table>
                <textarea id="importExportArea" rows="4" cols="50"></textarea>
                <br>
                <button onclick="importData()">Import</button>
                <button onclick="exportData()">Export</button>
            </div>
        </div>
        <div id="settings" class="hidden">
            <h1>Settings</h1>
            <h2>General</h2>
            <label for="modeSelect">Mode:</label>
            <select id="modeSelect">
                <option value="DEMO">Demo</option>
                <option value="CTF">CTF</option>
            </select>
            <br><br>
            <label for="timeoutSelect">Display Timeout:</label>
            <select id="timeoutSelect">
                <option value="20000">20 seconds</option>
                <option value="30000">30 seconds</option>
                <option value="60000">1 minute</option>
                <option value="120000">2 minutes</option>
                <option value="0">Never</option>
            </select>
            <br><br>
            <h3>Wifi</h3>
            <div id="apSettings">
                <label for="ap_ssid">SSID:</label>
                <input type="text" id="ap_ssid">
                <br><br>
                <label for="ap_passphrase">Password:</label>
                <input type="text" id="ap_passphrase">
                <br><br>
                <label for="ssid_hidden">Hidden:</label>
                <input type="checkbox" id="ssid_hidden">
                <br><br>
                <label for="ap_channel">Channel:</label>
                <input type="number" id="ap_channel" min="1" max="12">
            </div>
            <br><br>
            <h2>CTF Mode</h2>
            <label for="welcomeMessageSelect">Welcome Message:</label>
            <select id="welcomeMessageSelect" onchange="toggleWelcomeMessage()">
                <option value="default">Default</option>
                <option value="custom">Custom</option>
            </select>
            <div id="customWelcomeMessage">
                <label for="customMessage">Custom Message:</label>
                <input type="text" id="customMessage" maxlength="20">
            </div>
            <br><br>
            <label for="flagMessageSelect">Flag Message (for valid cards):</label>
            <select id="flagMessageSelect" onchange="toggleFlagMessage()">
                <option value="default">Default ("VALID")</option>
                <option value="custom">Custom Flag</option>
            </select>
            <div id="customFlagMessage">
                <label for="customFlag">Custom Flag:</label>
                <input type="text" id="customFlag" maxlength="20" placeholder="flag{example}">
            </div>
            <br><br>
            <div>
                <label for="ledValid">On Valid Card - LED:</label>
                <select id="ledValid">
                    <option value="1">Flashing</option>
                    <option value="2">Solid</option>
                    <option value="0">Off</option>
                </select>
                <br><br>
                <label for="spkOnValid">On Valid Card - Speaker:</label>
                <select id="spkOnValid">
                    <option value="1">Pop Beeps</option>
                    <option value="2">Melody Beeps</option>
                    <option value="0">Off</option>
                </select>
            </div>
            <br><br>
            <div>
                <label for="spkOnInvalid">On Invalid Card - Speaker:</label>
                <select id="spkOnInvalid">
                    <option value=1>Sad Beeps</option>
                    <option value=0>Off</option>
                </select>
            </div>
            <br><br>
            <button onclick="saveSettings()">Save Settings</button>
            <br><br>
            <button onclick="forceRefresh()">Force Refresh Tables</button>
        </div>
    </div>
    <script>
        let cardData = [];
        const tableBody = document.getElementById('cardTable').getElementsByTagName('tbody')[0];
        const userTableBody = document.getElementById('userTable').getElementsByTagName('tbody')[0];
        const lastReadCardsTableBody = document.getElementById('lastReadCardsTable').getElementsByTagName('tbody')[0];
        const importExportArea = document.getElementById('importExportArea');

        function updateTable() {
            fetch('/getCards')
                .then(response => response.json())
                .then(data => {
                    tableBody.innerHTML = '';
                    cardData = data; // Store the data for reuse
                    data.forEach((card, index) => {
                        let row = tableBody.insertRow();
                        let cellIndex = row.insertCell(0);
                        let cellBitLength = row.insertCell(1);
                        let cellFacilityCode = row.insertCell(2);
                        let cellCardNumber = row.insertCell(3);
                        let cellHexData = row.insertCell(4);
                        let cellRawData = row.insertCell(5);

                        cellIndex.innerHTML = index + 1;
                        cellBitLength.innerHTML = card.bitCount;
                        cellFacilityCode.innerHTML = card.facilityCode;
                        cellCardNumber.innerHTML = card.cardNumber;
                        cellHexData.innerHTML = `<a href="#" onclick="copyToClipboard('${card.hexCardData}')">${card.hexCardData}</a>`;
                        cellRawData.innerHTML = card.rawCardData;
                    });
                    console.log("Updated card table with " + data.length + " cards");
                })
                .catch(error => console.error('Error fetching card data:', error));
        }

        function updateUserTable() {
            fetch('/getUsers')
                .then(response => response.json())
                .then(data => {
                    userTableBody.innerHTML = '';
                    data.forEach((user, index) => {
                        let row = userTableBody.insertRow();
                        let cellIndex = row.insertCell(0);
                        let cellFacilityCode = row.insertCell(1);
                        let cellCardNumber = row.insertCell(2);
                        let cellName = row.insertCell(3);
                        let cellAction = row.insertCell(4);

                        cellIndex.innerHTML = index + 1;
                        cellFacilityCode.innerHTML = user.facilityCode;
                        cellCardNumber.innerHTML = user.cardNumber;
                        cellName.innerHTML = user.name;
                        cellAction.innerHTML = '<button onclick="deleteCard(' + index + ')">Delete</button>';
                    });

                    // Add input row at the bottom of the table
                    let inputRow = userTableBody.insertRow();
                    inputRow.className = 'inputRow';
                    let cellIndex = inputRow.insertCell(0);
                    let cellFacilityCode = inputRow.insertCell(1);
                    let cellCardNumber = inputRow.insertCell(2);
                    let cellName = inputRow.insertCell(3);
                    let cellAction = inputRow.insertCell(4);
                    
                    cellFacilityCode.innerHTML = '<input type="number" id="newFacilityCode">';
                    cellCardNumber.innerHTML = '<input type="number" id="newCardNumber">';
                    cellName.innerHTML = '<input type="text" id="newName">';
                    cellAction.innerHTML = '<button onclick="addCard()">Save</button>';
                    console.log("Updated user table with " + data.length + " users");
                })
                .catch(error => console.error('Error fetching user data:', error));
        }

        function updateLastReadCardsTable() {
            fetch('/getCards')
                .then(response => response.json())
                .then(data => {
                    lastReadCardsTableBody.innerHTML = '';
                    const last10Cards = data.slice(-10);
                    last10Cards.forEach((card, index) => {
                        let row = lastReadCardsTableBody.insertRow();
                        if (card.status === "Authorized") {
                            row.classList.add("authorized");
                        } else if (card.status === "Unauthorized") {
                            row.classList.add("unauthorized");
                        }
                        let cellIndex = row.insertCell(0);
                        let cellStatus = row.insertCell(1);
                        let cellDetails = row.insertCell(2);

                        cellIndex.innerHTML = index + 1;
                        cellStatus.innerHTML = card.status;
                        cellDetails.innerHTML = card.details;
                    });

                    // Add empty rows if there are less than 10 entries
                    for (let i = last10Cards.length; i < 10; i++) {
                        let row = lastReadCardsTableBody.insertRow();
                        let cellIndex = row.insertCell(0);
                        let cellStatus = row.insertCell(1);
                        let cellDetails = row.insertCell(2);

                        cellIndex.innerHTML = i + 1;
                        cellStatus.innerHTML = "";
                        cellDetails.innerHTML = "";
                    }
                    console.log("Updated last read cards table with " + last10Cards.length + " last cards");
                })
                .catch(error => console.error('Error fetching last read card data:', error));
        }

        function addCard() {
            const facilityCode = document.getElementById('newFacilityCode').value;
            const cardNumber = document.getElementById('newCardNumber').value;
            const name = document.getElementById('newName').value;

            fetch(`/addCard?facilityCode=${facilityCode}&cardNumber=${cardNumber}&name=${name}`)
                .then(response => {
                    if (response.ok) {
                        updateUserTable();
                        alert('Card added successfully');
                    } else {
                        alert('Failed to add card');
                    }
                })
                .catch(error => console.error('Error adding card:', error));
        }

        function deleteCard(index) {
            fetch(`/deleteCard?index=${index}`)
                .then(response => {
                    if (response.ok) {
                        updateUserTable();
                        alert('Card deleted successfully');
                    } else {
                        alert('Failed to delete card');
                    }
                })
                .catch(error => console.error('Error deleting card:', error));
        }

        function showSection(section) {
            document.getElementById('lastRead').classList.add('hidden');
            document.getElementById('ctfMode').classList.add('hidden');
            document.getElementById('settings').classList.add('hidden');
            document.getElementById(section).classList.remove('hidden');
            
            // Update the tables when switching sections
            if (section === 'lastRead') {
                updateTable();
            } else if (section === 'ctfMode') {
                updateUserTable();
                updateLastReadCardsTable();
            }
        }

        function toggleCollapsible() {
            const content = document.querySelector(".contentCollapsible");
            content.style.display = content.style.display === "block" ? "none" : "block";
        }

        function toggleWelcomeMessage() {
            const welcomeMessageSelect = document.getElementById('welcomeMessageSelect').value;
            const customMessageInput = document.getElementById('customMessage');
            if (welcomeMessageSelect === 'custom') {
                customMessageInput.disabled = false;
            } else {
                customMessageInput.disabled = true;
            }
        }

        function toggleFlagMessage() {
            const flagMessageSelect = document.getElementById('flagMessageSelect').value;
            const customFlagInput = document.getElementById('customFlag');
            if (flagMessageSelect === 'custom') {
                customFlagInput.disabled = false;
            } else {
                customFlagInput.disabled = true;
            }
        }

        function updateSettingsUI(settings) {
            document.getElementById('modeSelect').value = settings.mode;
            document.getElementById('timeoutSelect').value = settings.displayTimeout;
            document.getElementById('ap_ssid').value = settings.apSsid;
            document.getElementById('ap_passphrase').value = settings.apPassphrase;
            document.getElementById('ssid_hidden').checked = settings.ssidHidden;
            document.getElementById('ap_channel').value = settings.apChannel;
            document.getElementById('welcomeMessageSelect').value = settings.welcomeMessageSelect;
            document.getElementById('customMessage').value = settings.customMessage;
            document.getElementById('flagMessageSelect').value = settings.flagMessageSelect;
            document.getElementById('customFlag').value = settings.customFlag;
            document.getElementById('ledValid').value = settings.ledValid;
            document.getElementById('spkOnValid').value = settings.spkOnValid;
            document.getElementById('spkOnInvalid').value = settings.spkOnInvalid;
            toggleWelcomeMessage();
            toggleFlagMessage();
        }

        function saveSettings() {
            const mode = document.getElementById('modeSelect').value;
            const timeout = document.getElementById('timeoutSelect').value;
            const apSsid = document.getElementById('ap_ssid').value;
            const apPassphrase = document.getElementById('ap_passphrase').value;
            const ssidHidden = document.getElementById('ssid_hidden').checked;
            const apChannel = document.getElementById('ap_channel').value;
            const welcomeMessageSelect = document.getElementById('welcomeMessageSelect').value;
            const customMessage = document.getElementById('customMessage').value;
            const flagMessageSelect = document.getElementById('flagMessageSelect').value;
            const customFlag = document.getElementById('customFlag').value;
            const ledValid = document.getElementById('ledValid').value;
            const spkOnValid = document.getElementById('spkOnValid').value;
            const spkOnInvalid = document.getElementById('spkOnInvalid').value;

            let settings = {
                mode: mode,
                displayTimeout: parseInt(timeout, 10),
                apSsid: apSsid,
                apPassphrase: apPassphrase,
                ssidHidden: ssidHidden,
                apChannel: parseInt(apChannel),
                welcomeMessageSelect: welcomeMessageSelect,
                customMessage: customMessage,
                flagMessageSelect: flagMessageSelect,
                customFlag: customFlag,
                ledValid: parseInt(ledValid),
                spkOnValid: parseInt(spkOnValid),
                spkOnInvalid: parseInt(spkOnInvalid)
            };

            fetch('/saveSettings', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(settings)
            })
            .then(response => {
                if (response.ok) {
                    alert('Settings saved successfully');
                } else {
                    alert('Failed to save settings');
                }
            })
            .catch(error => console.error('Error saving settings:', error));
        }

        function fetchSettings() {
            fetch('/getSettings')
                .then(response => response.json())
                .then(data => {
                    updateSettingsUI(data);
                })
                .catch(error => console.error('Error fetching settings:', error));
        }

        function forceRefresh() {
            updateTable();
            updateUserTable();
            updateLastReadCardsTable();
            alert('Tables refreshed');
        }

        window.onload = function() {
            fetchSettings();
            updateTable();
            updateUserTable();
            updateLastReadCardsTable();
            // Initialize the disabled state for custom inputs
            toggleWelcomeMessage();
            toggleFlagMessage();
        };
        
        function exportData() {
            fetch('/getUsers')
                .then(response => response.json())
                .then(data => {
                    const dataString = JSON.stringify(data);
                    importExportArea.value = dataString;
                    importExportArea.select();
                    document.execCommand('copy');
                    alert('Data exported to clipboard');
                })
                .catch(error => console.error('Error exporting data:', error));
        }

        function importData() {
            const dataString = importExportArea.value;
            try {
                const data = JSON.parse(dataString);
                if (Array.isArray(data)) {
                    data.forEach(card => {
                        const facilityCode = card.facilityCode;
                        const cardNumber = card.cardNumber;
                        const name = card.name;

                        fetch(`/addCard?facilityCode=${facilityCode}&cardNumber=${cardNumber}&name=${name}`)
                            .then(response => {
                                if (!response.ok) {
                                    throw new Error('Failed to add card');
                                }
                            })
                            .catch(error => console.error('Error adding card:', error));
                    });
                    updateUserTable();
                    alert('Data imported successfully');
                } else {
                    alert('Invalid data format');
                }
            } catch (error) {
                alert('Invalid JSON format');
            }
        }

        function copyToClipboard(text) {
            const tempInput = document.createElement('input');
            tempInput.style.position = 'absolute';
            tempInput.style.left = '-9999px';
            tempInput.value = text;
            document.body.appendChild(tempInput);
            tempInput.select();
            document.execCommand('copy');
            document.body.removeChild(tempInput);
        }

        // Increase polling frequency for more responsive UI
        setInterval(updateTable, 1000);  // Every 1 second
        setInterval(updateLastReadCardsTable, 1000);  // Every 1 second
    </script>
</body>
</html>
)rawliteral";

// Scan I2C bus for devices
void scanI2C() {
  Serial.println("Scanning I2C bus for devices...");
  byte error, address;
  int nDevices = 0;
  
  for(address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.print(address, HEX);
      Serial.println();
      nDevices++;
    }
  }
  
  if (nDevices == 0) {
    Serial.println("No I2C devices found! Check your wiring.");
  } else {
    Serial.print("Found ");
    Serial.print(nDevices);
    Serial.println(" I2C device(s)");
  }
}

// Configure ESP32 power and performance settings
void configureESP32() {
    if (DEBUG_MODE) Serial.println("Configuring ESP32 power settings...");
    
    // Disable automatic WiFi power saving
    esp_wifi_set_ps(WIFI_PS_NONE);
    
    // Disable Bluetooth to save power and reduce interference
    btStop();
    
    if (DEBUG_MODE) Serial.println("ESP32 power settings configured");
}

// Try to initialise the LCD with the correct address
bool initLCD() {
    if (DEBUG_MODE) Serial.println("Setting up I2C bus...");
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(100);
    
    // Scan for I2C devices
    scanI2C();
    
    // Try each LCD address one by one
    if (DEBUG_MODE) Serial.println("Trying LCD address 0x27...");
    lcd_0x27.begin();
    lcd_0x27.backlight();
    lcd_0x27.clear();
    lcd_0x27.setCursor(0, 0);
    lcd_0x27.print("Testing 0x27");
    delay(500);
    
    Wire.beginTransmission(0x27);
    if (Wire.endTransmission() == 0) {
        if (DEBUG_MODE) Serial.println("LCD found at address 0x27");
        lcd = &lcd_0x27;
        return true;
    }
    
    if (DEBUG_MODE) Serial.println("Trying LCD address 0x3F...");
    lcd_0x3F.begin();
    lcd_0x3F.backlight();
    lcd_0x3F.clear();
    lcd_0x3F.setCursor(0, 0);
    lcd_0x3F.print("Testing 0x3F");
    delay(500);
    
    Wire.beginTransmission(0x3F);
    if (Wire.endTransmission() == 0) {
        if (DEBUG_MODE) Serial.println("LCD found at address 0x3F");
        lcd = &lcd_0x3F;
        return true;
    }
    
    if (DEBUG_MODE) Serial.println("Trying LCD address 0x20...");
    lcd_0x20.begin();
    lcd_0x20.backlight();
    lcd_0x20.clear();
    lcd_0x20.setCursor(0, 0);
    lcd_0x20.print("Testing 0x20");
    delay(500);
    
    Wire.beginTransmission(0x20);
    if (Wire.endTransmission() == 0) {
        if (DEBUG_MODE) Serial.println("LCD found at address 0x20");
        lcd = &lcd_0x20;
        return true;
    }
    
    if (DEBUG_MODE) Serial.println("ERROR: LCD not found at common addresses!");
    return false;
}

// IRAM_ATTR ensures the ISR runs from RAM, making it faster and more reliable
void IRAM_ATTR ISR_INT0() {
  // Track interrupt activity
  lastInterruptTime = millis();
  interruptCount++;
  cardBeingRead = true;
  
  // Original code
  bitCount++;
  flagDone = 0;

  if (bitCount < 23) {
    bitHolder1 = bitHolder1 << 1;
  } else {
    bitHolder2 = bitHolder2 << 1;
  }
  weigandCounter = WEIGAND_WAIT_TIME;
}

// IRAM_ATTR for second interrupt handler as well
void IRAM_ATTR ISR_INT1() {
  // Track interrupt activity
  lastInterruptTime = millis();
  interruptCount++;
  cardBeingRead = true;
  
  // Original code with bit tracking
  if (bitCount < MAX_BITS) {
    databits[bitCount] = 1;
    bitCount++;
  }
  flagDone = 0;

  if (bitCount < 23) {
    bitHolder1 = bitHolder1 << 1;
    bitHolder1 |= 1;
  } else {
    bitHolder2 = bitHolder2 << 1;
    bitHolder2 |= 1;
  }

  weigandCounter = WEIGAND_WAIT_TIME;
}

// Function to safely attach interrupts
void attachWeigandInterrupts() {
  if (DEBUG_MODE) Serial.println("Attaching Wiegand interrupts...");
  if (!interruptsAttached) {
    attachInterrupt(digitalPinToInterrupt(DATA0), ISR_INT0, FALLING);
    attachInterrupt(digitalPinToInterrupt(DATA1), ISR_INT1, FALLING);
    interruptsAttached = true;
    if (DEBUG_MODE) Serial.println("Weigand interrupts attached");
  }
}

// Function to safely detach interrupts if needed
void detachWeigandInterrupts() {
  if (interruptsAttached) {
    detachInterrupt(digitalPinToInterrupt(DATA0));
    detachInterrupt(digitalPinToInterrupt(DATA1));
    interruptsAttached = false;
    if (DEBUG_MODE) Serial.println("Weigand interrupts detached");
  }
}

void saveSettingsToPreferences() {
  preferences.begin("settings", false);
  preferences.putString("MODE", MODE);
  preferences.putULong("displayTimeout", displayTimeout);
  preferences.putBool("APMode", APMode);
  preferences.putString("ap_ssid", ap_ssid);
  preferences.putString("ap_passphrase", ap_passphrase);
  preferences.putInt("ap_channel", ap_channel);
  preferences.putInt("ssid_hidden", ssid_hidden);
  preferences.putInt("spkOnInvalid", spkOnInvalid);
  preferences.putInt("spkOnValid", spkOnValid);
  preferences.putInt("ledValid", ledValid);
  preferences.putString("customWelcomeMessage", customWelcomeMessage);
  preferences.putString("welcomeMessageSelect", welcomeMessageSelect);
  preferences.putString("customFlagMessage", customFlagMessage);
  preferences.putString("flagMessageSelect", flagMessageSelect);
  preferences.end();
}

// Safe preferences handling with timeout
bool loadSettingsFromPreferences() {
  if (DEBUG_MODE) Serial.println("Loading settings from preferences...");
  unsigned long startTime = millis();
  
  preferences.begin("settings", true); // Read-only mode
  
  if (millis() - startTime > 3000) {
    if (DEBUG_MODE) Serial.println("WARNING: Preferences begin() taking too long!");
    return false;
  }
  
  MODE = preferences.getString("MODE", "CTF");
  displayTimeout = preferences.getULong("displayTimeout", 30000);
  APMode = preferences.getBool("APMode", true);
  ap_ssid = preferences.getString("ap_ssid", "doorsim");
  ap_passphrase = preferences.getString("ap_passphrase", "");
  ap_channel = preferences.getInt("ap_channel", 1);
  ssid_hidden = preferences.getInt("ssid_hidden", 0);
  spkOnInvalid = preferences.getInt("spkOnInvalid", 1);
  spkOnValid = preferences.getInt("spkOnValid", 1);
  ledValid = preferences.getInt("ledValid", 1);
  customWelcomeMessage = preferences.getString("customWelcomeMessage", "");
  welcomeMessageSelect = preferences.getString("welcomeMessageSelect", "default");
  customFlagMessage = preferences.getString("customFlagMessage", "");
  flagMessageSelect = preferences.getString("flagMessageSelect", "default");
  
  preferences.end();
  
  if (DEBUG_MODE) {
    Serial.println("Settings loaded:");
    Serial.print("MODE: "); Serial.println(MODE);
    Serial.print("AP SSID: "); Serial.println(ap_ssid);
  }
  
  return true;
}

void saveCredentialsToPreferences() {
  preferences.begin("credentials", false);
  preferences.putInt("validCount", validCount);
  for (int i = 0; i < validCount; i++) {
    String fcKey = "fc" + String(i);
    String cnKey = "cn" + String(i);
    String nameKey = "name" + String(i);
    preferences.putUInt(fcKey.c_str(), credentials[i].facilityCode);
    preferences.putUInt(cnKey.c_str(), credentials[i].cardNumber);
    preferences.putString(nameKey.c_str(), credentials[i].name);
  }
  preferences.end();
  Serial.println("Credentials saved to Preferences:");
  for (int i = 0; i < validCount; i++) {
    Serial.print("Credential ");
    Serial.print(i);
    Serial.print(": FC=");
    Serial.print(credentials[i].facilityCode);
    Serial.print(", CN=");
    Serial.print(credentials[i].cardNumber);
    Serial.print(", Name=");
    Serial.println(credentials[i].name);
  }
  Serial.print("Valid Count: ");
  Serial.println(validCount);
}

void loadCredentialsFromPreferences() {
  preferences.begin("credentials", true);
  validCount = preferences.getInt("validCount", 0);
  for (int i = 0; i < validCount; i++) {
    String fcKey = "fc" + String(i);
    String cnKey = "cn" + String(i);
    String nameKey = "name" + String(i);
    credentials[i].facilityCode = preferences.getUInt(fcKey.c_str(), 0);
    credentials[i].cardNumber = preferences.getUInt(cnKey.c_str(), 0);
    String name = preferences.getString(nameKey.c_str(), "");
    strncpy(credentials[i].name, name.c_str(), sizeof(credentials[i].name) - 1);
    credentials[i].name[sizeof(credentials[i].name) - 1] = '\0';
  }
  preferences.end();
  Serial.println("Credentials loaded from Preferences:");
  for (int i = 0; i < validCount; i++) {
    Serial.print("Credential ");
    Serial.print(i);
    Serial.print(": FC=");
    Serial.print(credentials[i].facilityCode);
    Serial.print(", CN=");
    Serial.print(credentials[i].cardNumber);
    Serial.print(", Name=");
    Serial.println(credentials[i].name);
  }
  Serial.print("Valid Count: ");
  Serial.println(validCount);
}

void setupWifi() {
    if (DEBUG_MODE) Serial.println("Setting up WiFi in AP mode...");
    
    // Set explicitly to AP mode
    WiFi.mode(WIFI_AP);
    
    // Disconnect from any existing connections first
    WiFi.disconnect();
    
    // Add delay for stability
    delay(100);
    
    // Disable any automatic reconnect attempts
    WiFi.setAutoReconnect(false);
    
    // Start the AP with proper error handling
    Serial.print("Starting WiFi AP with SSID: ");
    Serial.println(ap_ssid);
    Serial.print("Password: ");
    Serial.println(ap_passphrase.length() > 0 ? "[SET]" : "[OPEN]");
    Serial.print("Channel: ");
    Serial.println(ap_channel);
    Serial.print("Hidden: ");
    Serial.println(ssid_hidden ? "YES" : "NO");
    
    // Use configured AP settings
    bool success = false;
    if (ap_passphrase.length() > 0) {
        // Start AP with password
        success = WiFi.softAP(ap_ssid.c_str(), ap_passphrase.c_str(), ap_channel, ssid_hidden);
    } else {
        // Start AP without password (open network)
        success = WiFi.softAP(ap_ssid.c_str(), NULL, ap_channel, ssid_hidden);
    }
    
    if (success) {
        Serial.println("AP started successfully with configured settings");
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
        Serial.print("SSID: ");
        Serial.println(WiFi.softAPSSID());
        Serial.print("Security: ");
        Serial.println(ap_passphrase.length() > 0 ? "WPA2" : "Open");
    } else {
        Serial.println("CRITICAL ERROR: Could not start AP with configured settings!");
        Serial.println("Falling back to default open network...");
        if(WiFi.softAP("doorsim")) {
            Serial.println("Fallback AP started successfully");
        } else {
            Serial.println("CRITICAL ERROR: Even fallback AP failed!");
        }
    }
    
    // Prevent WiFi from going to sleep
    WiFi.setSleep(false);
    
    // Give WiFi time to stabilize
    delay(500);
}

// Check if credential is valid
const Credential *checkCredential(uint16_t fc, uint16_t cn) {
  for (unsigned int i = 0; i < validCount; i++) {
    if (credentials[i].facilityCode == fc && credentials[i].cardNumber == cn) {
      // Found a matching credential, return a pointer to it
      return &credentials[i];
    }
  }
  // No matching credential found, return nullptr
  return nullptr;
}

String centreText(const String &text, int width) {
  int len = text.length();
  if (len >= width) {
    return text;
  }
  int padding = (width - len) / 2;
  String spaces = "";
  for (int i = 0; i < padding; i++) {
    spaces += " ";
  }
  return spaces + text;
}

void printWelcomeMessage() {
  if (!lcd) {
    if (DEBUG_MODE) Serial.println("ERROR: Cannot print welcome message, LCD not initialised");
    return;
  }
  
  if (DEBUG_MODE) Serial.println("Displaying welcome message");
  
  if (MODE == "CTF") {
    if (customWelcomeMessage.length() > 0) {
      lcd->clear();
      lcd->setCursor(0, 0);
      lcd->print(centreText(String(customWelcomeMessage), 20));
      lcd->setCursor(0, 2);
      lcd->print(centreText("Present Card", 20));
    }
    else {
      lcd->clear();
      lcd->setCursor(0, 0);
      lcd->print(centreText("CTF Mode", 20));
      lcd->setCursor(0, 2);
      lcd->print(centreText("Present Card", 20));
    }
  } else {
    lcd->clear();
    lcd->setCursor(0, 0);
    lcd->print(centreText("Door Sim - Ready", 20));
    lcd->setCursor(0, 2);
    lcd->print(centreText("Present Card", 20));  
  }
}

// Handle valid credentials
void speakerOnValid() {
  if (DEBUG_MODE) Serial.println("Playing valid card sound");
  
  switch (spkOnValid) {
    case 0:
      break;
    
    case 1:
      // Nice Beeps LED
      digitalWrite(SPK, LOW);
      delay(100);
      digitalWrite(SPK, HIGH);
      delay(50);
      digitalWrite(SPK, LOW);
      delay(100);
      digitalWrite(SPK, HIGH);
      break;

    case 2:
      // Long Beeps
      digitalWrite(SPK, LOW);
      delay(2000);
      digitalWrite(SPK, HIGH);
      break;
  }
}

void ledOnValid() {
  if (DEBUG_MODE) Serial.println("Activating valid card LED");
  
  switch (ledValid) {
    case 0:
      break;
    
    case 1:
      // Flashing LED
      digitalWrite(LED, LOW);
      delay(250);
      digitalWrite(LED, HIGH);
      delay(100);
      digitalWrite(LED, LOW);
      delay(250);
      digitalWrite(LED, HIGH);
      break;
    
    case 2:
      digitalWrite(LED, LOW);
      delay(2000);
      digitalWrite(LED, HIGH);
      break;
  }
}

// Handle invalid credentials
void lcdInvalidCredentials() {
  if (!lcd) {
    if (DEBUG_MODE) Serial.println("ERROR: Cannot display invalid credentials, LCD not initialized");
    return;
  }
  
  if (DEBUG_MODE) Serial.println("Displaying invalid credentials message");
  
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print("Card Read: ");
  lcd->setCursor(11, 0);
  lcd->print("INVALID");
  lcd->setCursor(0, 2);
  lcd->print(" THIS INCIDENT WILL");
  lcd->setCursor(0, 3);
  lcd->print("    BE REPORTED    ");
}

void speakerOnFailure() {
  if (DEBUG_MODE) Serial.println("Playing invalid card sound");
  
  switch (spkOnInvalid) {
    case 0:
      break;
    
    case 1:
      // Sad Beeps
      digitalWrite(SPK, LOW);
      delay(100);
      digitalWrite(SPK, HIGH);
      delay(50);
      digitalWrite(SPK, LOW);
      delay(100);
      digitalWrite(SPK, HIGH);
      delay(50);
      digitalWrite(SPK, LOW);
      delay(100);
      digitalWrite(SPK, HIGH);
      delay(50);
      digitalWrite(SPK, LOW);
      delay(100);
      digitalWrite(SPK, HIGH);
      break;
  }
}

void printCardData() {
  Serial.println("printCardData() called with:");
  Serial.print("Bit count: "); Serial.println(bitCount);
  Serial.print("Facility code: "); Serial.println(facilityCode);
  Serial.print("Card number: "); Serial.println(cardNumber);
  Serial.print("Hex data: "); Serial.println(hexCardData);
  Serial.print("Raw data: "); Serial.println(rawCardData);

  if (MODE == "CTF") {
    const Credential* result = checkCredential(facilityCode, cardNumber);
    if (result != nullptr) {
      // Valid credential found
      Serial.println("Valid credential found:");
      Serial.println("FC: " + String(result->facilityCode) + ", CN: " + String(result->cardNumber) + ", Name: " + result->name);
      
      // Determine what message to show for valid card
      String validMessage = "VALID";
      if (flagMessageSelect == "custom" && customFlagMessage.length() > 0) {
        validMessage = customFlagMessage;
      }
      
      if (lcd) {
        lcd->clear();
        lcd->setCursor(0, 0);
        lcd->print("Card Read: ");
        
        // Print the valid message, truncating if necessary to fit
        String displayMessage = validMessage;
        if (displayMessage.length() > 8) { // "Card Read: " takes 11 chars, leaving 9 for message
          displayMessage = displayMessage.substring(0, 8);
        }
        lcd->setCursor(11, 0);
        lcd->print(displayMessage);
        
        lcd->setCursor(0, 1);
        lcd->print("FC: " + String(result->facilityCode));
        lcd->setCursor(9, 1);
        lcd->print("CN:" + String(result->cardNumber));
        lcd->setCursor(0, 3);
        lcd->print("Name: " + String(result->name));
      }
      
      ledOnValid();
      speakerOnValid();

      // Update card data status and details
      status = "Authorized";
      details = result->name;
    } else {
      // No valid credential found
      Serial.println("Error: No valid credential found.");
      lcdInvalidCredentials();
      speakerOnFailure();

      // Update card data status and details
      status = "Unauthorized";
      details = "FC: " + String(facilityCode) + ", CN: " + String(cardNumber);
    }
  } else {
    // ranges for "valid" bitCount are a bit larger for debugging
    if (bitCount > 20 && bitCount < 120) {
      // ignore data caused by noise
      Serial.print("[*] Bit length: ");
      Serial.println(bitCount);
      Serial.print("[*] Facility code: ");
      Serial.println(facilityCode);
      Serial.print("[*] Card number: ");
      Serial.println(cardNumber);
      Serial.print("[*] Hex: ");
      Serial.println(hexCardData);
      Serial.print("[*] Raw: ");
      Serial.println(rawCardData);

      // LCD Printing
      if (lcd) {
        lcd->clear();
        lcd->setCursor(0, 0);
        lcd->print("Card Read: ");
        lcd->setCursor(11, 0);
        lcd->print(bitCount);
        lcd->print("bits");
        lcd->setCursor(0, 1);
        lcd->print("FC: ");
        lcd->print(facilityCode);
        lcd->setCursor(9, 1);
        lcd->print(" CN: ");
        lcd->print(cardNumber);
        lcd->setCursor(0, 3);
        lcd->print("Hex: ");
        hexCardData.toUpperCase();
        lcd->print(hexCardData);
      }

      // Update card data status and details
      status = "Read";
      details = "Hex: " + hexCardData;
    }
  }

  // Store card data - fixed to always store data regardless of mode
  if (cardDataIndex < MAX_CARDS) {
    Serial.print("Storing card at index: ");
    Serial.println(cardDataIndex);
    
    cardDataArray[cardDataIndex].bitCount = bitCount;
    cardDataArray[cardDataIndex].facilityCode = facilityCode;
    cardDataArray[cardDataIndex].cardNumber = cardNumber;
    cardDataArray[cardDataIndex].hexCardData = hexCardData;
    cardDataArray[cardDataIndex].rawCardData = rawCardData;
    cardDataArray[cardDataIndex].status = status;
    cardDataArray[cardDataIndex].details = details;
    cardDataIndex++;
    
    Serial.print("Card stored. New cardDataIndex: ");
    Serial.println(cardDataIndex);
  } else {
    // If we reached the max cards, start overwriting from the beginning
    Serial.println("Card storage full, resetting to start overwriting from beginning");
    cardDataIndex = 0;
    
    cardDataArray[cardDataIndex].bitCount = bitCount;
    cardDataArray[cardDataIndex].facilityCode = facilityCode;
    cardDataArray[cardDataIndex].cardNumber = cardNumber;
    cardDataArray[cardDataIndex].hexCardData = hexCardData;
    cardDataArray[cardDataIndex].rawCardData = rawCardData;
    cardDataArray[cardDataIndex].status = status;
    cardDataArray[cardDataIndex].details = details;
    cardDataIndex++;
  }

  // Start the display timer
  lastCardTime = millis();
  displayingCard = true;
}

// Process hid cards
unsigned long decodeHIDFacilityCode(unsigned int start, unsigned int end) {
  unsigned long HIDFacilityCode = 0;
  for (unsigned int i = start; i < end && i < bitCount; i++) {
    HIDFacilityCode = (HIDFacilityCode << 1) | databits[i];
  }
  return HIDFacilityCode;
}

unsigned long decodeHIDCardNumber(unsigned int start, unsigned int end) {
  unsigned long HIDCardNumber = 0;
  for (unsigned int i = start; i < end && i < bitCount; i++) {
    HIDCardNumber = (HIDCardNumber << 1) | databits[i];
  }
  return HIDCardNumber;
}

// Card value processing functions
// Function to append the card value (bitHolder1 and bitHolder2) to the
// necessary array then translate that to the two chunks for the card value that
// will be output
void setCardChunkBits(unsigned int cardChunk1Offset, unsigned int bitHolderOffset, unsigned int cardChunk2Offset) {
  for (int i = 19; i >= 0; i--) {
    if (i == 13 || i == cardChunk1Offset) {
      bitWrite(cardChunk1, i, 1);
    } else if (i > cardChunk1Offset) {
      bitWrite(cardChunk1, i, 0);
    } else {
      bitWrite(cardChunk1, i, bitRead(bitHolder1, i + bitHolderOffset));
    }
    if (i < bitHolderOffset) {
      bitWrite(cardChunk2, i + cardChunk2Offset, bitRead(bitHolder1, i));
    }
    if (i < cardChunk2Offset) {
      bitWrite(cardChunk2, i, bitRead(bitHolder2, i));
    }
  }
}

String prefixPad(const String &in, const char c, const size_t len) {
  String out = in;
  while (out.length() < len) {
    out = c + out;
  }
  return out;
}

void processHIDCard() {
  // bits to be decoded differently depending on card format length
  // see http://www.pagemac.com/projects/rfid/hid_data_formats for more info
  // also specifically: www.brivo.com/app/static_data/js/calculate.js
  // Example of full card value
  // |>   preamble   <| |>   Actual card value   <|
  // 000000100000000001 11 111000100000100100111000
  // |> write to chunk1 <| |>  write to chunk2   <|

  unsigned int cardChunk1Offset, bitHolderOffset, cardChunk2Offset;

  switch (bitCount) {
    case 26:
      facilityCode = decodeHIDFacilityCode(1, 9);
      cardNumber = decodeHIDCardNumber(9, 25);
      cardChunk1Offset = 2;
      bitHolderOffset = 20;
      cardChunk2Offset = 4;
      break;

    case 27:
      facilityCode = decodeHIDFacilityCode(1, 13);
      cardNumber = decodeHIDCardNumber(13, 27);
      cardChunk1Offset = 3;
      bitHolderOffset = 19;
      cardChunk2Offset = 5;
      break;

    case 29:
      facilityCode = decodeHIDFacilityCode(1, 13);
      cardNumber = decodeHIDCardNumber(13, 29);
      cardChunk1Offset = 5;
      bitHolderOffset = 17;
      cardChunk2Offset = 7;
      break;

    case 30:
      facilityCode = decodeHIDFacilityCode(1, 13);
      cardNumber = decodeHIDCardNumber(13, 29);
      cardChunk1Offset = 6;
      bitHolderOffset = 16;
      cardChunk2Offset = 8;
      break;

    case 31:
      facilityCode = decodeHIDFacilityCode(1, 5);
      cardNumber = decodeHIDCardNumber(5, 28);
      cardChunk1Offset = 7;
      bitHolderOffset = 15;
      cardChunk2Offset = 9;
      break;

    // modified to wiegand 32 bit format instead of HID
    case 32:
      facilityCode = decodeHIDFacilityCode(5, 16);
      cardNumber = decodeHIDCardNumber(17, 32);
      cardChunk1Offset = 8;
      bitHolderOffset = 14;
      cardChunk2Offset = 10;
      break;

    case 33:
      facilityCode = decodeHIDFacilityCode(1, 8);
      cardNumber = decodeHIDCardNumber(8, 32);
      cardChunk1Offset = 9;
      bitHolderOffset = 13;
      cardChunk2Offset = 11;
      break;

    case 34:
      facilityCode = decodeHIDFacilityCode(1, 17);
      cardNumber = decodeHIDCardNumber(17, 33);
      cardChunk1Offset = 10;
      bitHolderOffset = 12;
      cardChunk2Offset = 12;
      break;

    case 35:
      facilityCode = decodeHIDFacilityCode(2, 14);
      cardNumber = decodeHIDCardNumber(14, 34);
      cardChunk1Offset = 11;
      bitHolderOffset = 11;
      cardChunk2Offset = 13;
      break;

    case 36:
      facilityCode = decodeHIDFacilityCode(21, 33);
      cardNumber = decodeHIDCardNumber(1, 17);
      cardChunk1Offset = 12;
      bitHolderOffset = 10;
      cardChunk2Offset = 14;
      break;

    default:
      Serial.println("[-] Unsupported bitCount for HID card");
      facilityCode = 0;
      cardNumber = 0;
      // Default values to prevent issues
      cardChunk1Offset = 8;
      bitHolderOffset = 14;
      cardChunk2Offset = 10;
      break;
  }

  setCardChunkBits(cardChunk1Offset, bitHolderOffset, cardChunk2Offset);
  hexCardData = String(cardChunk1, HEX) + prefixPad(String(cardChunk2, HEX), '0', 6);
}

void processCardData() {
  // First, build the raw data string
  Serial.println("Processing card data...");
  Serial.print("Bit count: "); Serial.println(bitCount);
  
  rawCardData = "";
  for (unsigned int i = 0; i < bitCount && i < MAX_BITS; i++) {
    rawCardData += String(databits[i]);
  }
  
  Serial.print("Raw data: "); Serial.println(rawCardData);
  
  // Process the card data based on the bit count
  if (bitCount >= 26 && bitCount <= 96) {
    Serial.println("Valid bit count range, processing HID card...");
    processHIDCard();
    Serial.print("Processed - FC: "); Serial.print(facilityCode);
    Serial.print(", CN: "); Serial.print(cardNumber);
    Serial.print(", Hex: "); Serial.println(hexCardData);
  } else {
    Serial.print("Invalid bit count: "); Serial.println(bitCount);
    // Set default values
    facilityCode = 0;
    cardNumber = 0;
    hexCardData = "Unknown";
  }
}

void clearDatabits() {
  for (unsigned char i = 0; i < MAX_BITS; i++) {
    databits[i] = 0;
  }
}

// reset variables and prepare for the next card read
void cleanupCardData() {
  Serial.println("Cleaning up card data...");
  rawCardData = "";
  hexCardData = "";
  bitCount = 0;
  facilityCode = 0;
  cardNumber = 0;
  bitHolder1 = 0;
  bitHolder2 = 0;
  cardChunk1 = 0;
  cardChunk2 = 0;
  status = "";
  details = "";
  cardBeingRead = false;
}

// Function to process a card immediately after it's fully read
void processCardNow() {
  if (bitCount > 0) {
    Serial.println("Processing card immediately...");
    
    if (!shouldSkipProcessing()) {
      processCardData();
      
      if ((bitCount >= 26 && bitCount <= 36) || bitCount == 96) {
        Serial.println("Valid bit count, printing card data");
        printCardData();
        printAllCardData();
        
        // Immediately force a refresh of the web interface by setting a flag
        // that will be checked in the main loop
        lastCardTime = millis();
        displayingCard = true;
      } else {
        Serial.print("Invalid bit count: ");
        Serial.println(bitCount);
      }
    } else {
      Serial.println("Invalid data detected, skipping processing");
    }
    
    // Clear card data for next read
    cleanupCardData();
    clearDatabits();
  }
}

// FIXED: This function was checking if all bytes in databits are 0xFF,
// which is incorrect. It should check if we have valid data.
bool shouldSkipProcessing() {
  // Skip processing if bit count is invalid
  if (bitCount == 0 || bitCount > MAX_BITS) {
    Serial.println("Skipping processing - invalid bit count");
    return true;
  }
  
  // Check if all bits are 1 (which would be unusual for valid card data)
  bool allOnes = true;
  for (unsigned int i = 0; i < bitCount; i++) {
    if (databits[i] != 1) {
      allOnes = false;
      break;
    }
  }
  
  if (allOnes) {
    Serial.println("Skipping processing - all bits are 1");
    return true;
  }
  
  // Check if all bits are 0 (which would be unusual for valid card data)
  bool allZeros = true;
  for (unsigned int i = 0; i < bitCount; i++) {
    if (databits[i] != 0) {
      allZeros = false;
      break;
    }
  }
  
  if (allZeros) {
    Serial.println("Skipping processing - all bits are 0");
    return true;
  }
  
  return false;
}

void updateDisplay() {
  if (displayingCard && (millis() - lastCardTime >= displayTimeout)) {
    printWelcomeMessage();
    displayingCard = false;
  }
}

void printAllCardData() {
  Serial.println("Previously read card data:");
  for (int i = 0; i < cardDataIndex; i++) {
    Serial.print(i + 1);
    Serial.print(": Bit length: ");
    Serial.print(cardDataArray[i].bitCount);
    Serial.print(", Facility code: ");
    Serial.print(cardDataArray[i].facilityCode);
    Serial.print(", Card number: ");
    Serial.print(cardDataArray[i].cardNumber);
    Serial.print(", Hex: ");
    Serial.print(cardDataArray[i].hexCardData);
    Serial.print(", Raw: ");
    Serial.println(cardDataArray[i].rawCardData);
  }
}

// Web server handler functions
void handleRoot() {
    server.send(200, "text/html", index_html);
}

void handleGetCards() {
    Serial.println("handleGetCards() called");
    Serial.print("cardDataIndex: ");
    Serial.println(cardDataIndex);
    
    JsonDocument doc; // Using non-deprecated JsonDocument
    JsonArray cards = doc.to<JsonArray>();
    
    for (int i = 0; i < cardDataIndex; i++) {
        JsonObject card = cards.add<JsonObject>(); // Using non-deprecated method
        card["bitCount"] = cardDataArray[i].bitCount;
        card["facilityCode"] = cardDataArray[i].facilityCode;
        card["cardNumber"] = cardDataArray[i].cardNumber;
        card["hexCardData"] = cardDataArray[i].hexCardData;
        card["rawCardData"] = cardDataArray[i].rawCardData;
        card["status"] = cardDataArray[i].status;
        card["details"] = cardDataArray[i].details;
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleGetUsers() {
    JsonDocument doc;
    JsonArray users = doc.to<JsonArray>();
    for (int i = 0; i < validCount; i++) {
        JsonObject user = users.add<JsonObject>();
        user["facilityCode"] = credentials[i].facilityCode;
        user["cardNumber"] = credentials[i].cardNumber;
        user["name"] = credentials[i].name;
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleGetSettings() {
    JsonDocument doc;
    doc["mode"] = MODE;
    doc["displayTimeout"] = displayTimeout;
    doc["apSsid"] = ap_ssid;
    doc["apPassphrase"] = ap_passphrase;
    doc["ssidHidden"] = ssid_hidden;
    doc["apChannel"] = ap_channel;
    doc["welcomeMessageSelect"] = welcomeMessageSelect;
    doc["customMessage"] = customWelcomeMessage;
    doc["flagMessageSelect"] = flagMessageSelect;
    doc["customFlag"] = customFlagMessage;
    doc["ledValid"] = ledValid;
    doc["spkOnValid"] = spkOnValid;
    doc["spkOnInvalid"] = spkOnInvalid;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleSaveSettings() {
    if (server.hasArg("plain")) {
        String json = server.arg("plain");
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, json);
        if (!error) {
            // Store old WiFi settings to see if they changed
            String oldSSID = ap_ssid;
            String oldPassphrase = ap_passphrase;
            int oldChannel = ap_channel;
            int oldHidden = ssid_hidden;
            
            // Parse the JSON and update settings
            MODE = doc["mode"] | "CTF";
            displayTimeout = doc["displayTimeout"] | 30000;
            ap_ssid = doc["apSsid"] | "doorsim";
            ap_passphrase = doc["apPassphrase"] | "";
            ap_channel = doc["apChannel"] | 1;
            ssid_hidden = doc["ssidHidden"] | 0;
            welcomeMessageSelect = doc["welcomeMessageSelect"] | "default";
            customWelcomeMessage = doc["customMessage"] | "";
            flagMessageSelect = doc["flagMessageSelect"] | "default";
            customFlagMessage = doc["customFlag"] | "";
            spkOnInvalid = doc["spkOnInvalid"] | 1;
            spkOnValid = doc["spkOnValid"] | 1;
            ledValid = doc["ledValid"] | 1;
            
            saveSettingsToPreferences();
            
            // Check if WiFi settings changed
            if (oldSSID != ap_ssid || oldPassphrase != ap_passphrase || 
                oldChannel != ap_channel || oldHidden != ssid_hidden) {
                Serial.println("WiFi settings changed, restarting WiFi...");
                setupWifi(); // Restart WiFi with new settings
            }
            
            server.send(200, "application/json", "{\"status\":\"success\"}");
        } else {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
        }
    } else {
        server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No data\"}");
    }
}

void handleAddCard() {
    if (validCount < MAX_CREDENTIALS) {
        if (server.hasArg("facilityCode") && server.hasArg("cardNumber") && server.hasArg("name")) {
            String facilityCodeStr = server.arg("facilityCode");
            String cardNumberStr = server.arg("cardNumber");
            String name = server.arg("name");
            
            credentials[validCount].facilityCode = facilityCodeStr.toInt();
            credentials[validCount].cardNumber = cardNumberStr.toInt();
            strncpy(credentials[validCount].name, name.c_str(), sizeof(credentials[validCount].name) - 1);
            credentials[validCount].name[sizeof(credentials[validCount].name) - 1] = '\0';
            validCount++;
            
            saveCredentialsToPreferences();
            server.send(200, "text/plain", "Card added successfully");
        } else {
            server.send(400, "text/plain", "Missing parameters");
        }
    } else {
        server.send(500, "text/plain", "Max number of credentials reached");
    }
}

void handleDeleteCard() {
    if (server.hasArg("index")) {
        int index = server.arg("index").toInt();
        if (index >= 0 && index < validCount) {
            for (int i = index; i < validCount - 1; i++) {
                credentials[i] = credentials[i + 1];
            }
            validCount--;
            saveCredentialsToPreferences();
            server.send(200, "text/plain", "Card deleted successfully");
        } else {
            server.send(400, "text/plain", "Invalid index");
        }
    } else {
        server.send(400, "text/plain", "Missing index parameter");
    }
}

void handleExportData() {
    JsonDocument doc;
    JsonArray users = doc["users"].to<JsonArray>();
    for (int i = 0; i < validCount; i++) {
        JsonObject user = users.add<JsonObject>();
        user["facilityCode"] = credentials[i].facilityCode;
        user["cardNumber"] = credentials[i].cardNumber;
        user["name"] = credentials[i].name;
    }
    JsonArray cards = doc["cards"].to<JsonArray>();
    for (int i = 0; i < cardDataIndex; i++) {
        JsonObject card = cards.add<JsonObject>();
        card["bitCount"] = cardDataArray[i].bitCount;
        card["facilityCode"] = cardDataArray[i].facilityCode;
        card["cardNumber"] = cardDataArray[i].cardNumber;
        card["hexCardData"] = cardDataArray[i].hexCardData;
        card["rawCardData"] = cardDataArray[i].rawCardData;
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void setupWebServer() {
    if (DEBUG_MODE) Serial.println("Setting up full web server...");
    
    // Main page
    server.on("/", HTTP_GET, handleRoot);
    
    // API endpoints
    server.on("/getCards", HTTP_GET, handleGetCards);
    server.on("/getUsers", HTTP_GET, handleGetUsers);
    server.on("/getSettings", HTTP_GET, handleGetSettings);
    server.on("/saveSettings", HTTP_POST, handleSaveSettings);
    server.on("/addCard", HTTP_GET, handleAddCard);
    server.on("/deleteCard", HTTP_GET, handleDeleteCard);
    server.on("/exportData", HTTP_GET, handleExportData);
    
    // Start the server
    server.begin();
    
    if (DEBUG_MODE) Serial.println("Full web server started");
}

// Debug interrupt status
void debugInterrupts() {
    static unsigned long lastDebugTime = 0;
    const unsigned long debugInterval = 5000; // 5 seconds
    
    if (millis() - lastDebugTime >= debugInterval) {
        lastDebugTime = millis();
        
        Serial.print("Interrupt status - Count: ");
        Serial.print(interruptCount);
        Serial.print(", Last interrupt: ");
        Serial.print(millis() - lastInterruptTime);
        Serial.println("ms ago");
        
        // Reset interrupt count for next period
        interruptCount = 0;
    }
}

void setup() {
    // Record start time for timeout detection
    setupStartTime = millis();
    
    // Initialize serial communication first for debugging
    Serial.begin(115200);
    
    // Wait a moment for serial to connect
    delay(1000);
    
    Serial.println("\n\n");
    Serial.println("==============================================");
    Serial.println("ESP32 Door Simulator - FIXED VERSION");
    Serial.println("==============================================");
    Serial.println("Starting initialisation process...");
    
    // Configure ESP32 power settings
    configureESP32();
    
    // Set pin modes
    Serial.println("Setting up I/O pins...");
    pinMode(DATA0, INPUT);
    pinMode(DATA1, INPUT);
    pinMode(LED, OUTPUT);
    pinMode(SPK, OUTPUT);
    pinMode(RELAY1, OUTPUT);
    pinMode(RELAY2, OUTPUT);
    
    // Set initial states
    digitalWrite(LED, HIGH);
    digitalWrite(SPK, HIGH);
    digitalWrite(RELAY1, HIGH);
    Serial.println("I/O pins configured");
    
    // Initialize LCD
    bool lcdInitialised = initLCD();
    if (lcdInitialised && lcd) {
        Serial.println("LCD initialised successfully");
        lcd->clear();
        lcd->setCursor(0, 0);
        lcd->print("Initialising...");
    }
    else {
        Serial.println("WARNING: Failed to initialise LCD");
        // Continue anyway for debugging
    }
    
    // Try to load settings, but don't stop if it fails
    Serial.println("Loading settings phase...");
    if (!loadSettingsFromPreferences()) {
        Serial.println("WARNING: Failed to load settings from preferences");
        Serial.println("Using default settings");
    }
    
    // Load credentials
    Serial.println("Loading credentials phase...");
    loadCredentialsFromPreferences();
    
    // Setup WiFi - very important for stability
    Serial.println("WiFi setup phase...");
    setupWifi();
    
    // Weigand initialisation
    Serial.println("Initialising Wiegand reader...");
    weigandCounter = WEIGAND_WAIT_TIME;
    
    // Setup web server only after WiFi is up
    Serial.println("Web server setup phase...");
    setupWebServer();
    
    // Now that everything else is set up, attach interrupts
    Serial.println("Attaching card reader interrupts...");
    attachWeigandInterrupts();
    
    // Show welcome message if LCD is working
    if (lcd) {
        Serial.println("Displaying welcome message...");
        printWelcomeMessage();
    }
    
    // Setup completion
    Serial.println("==============================================");
    Serial.println("Setup completed successfully!");
    Serial.println("System is now running in " + MODE + " mode");
    Serial.println("Connect to WiFi SSID: doorsim");
    Serial.println("Access the web interface at: " + WiFi.softAPIP().toString());
    Serial.println("==============================================");
}

void loop() {
    // Check if we're in setup but it's taking too long
    if (millis() - setupStartTime < SETUP_TIMEOUT && !lcd) {
        Serial.println("Still waiting for initialisation to complete...");
        delay(1000);
        return;
    }
    
    // Handle HTTP requests
    server.handleClient();
    
    // Debug interrupt status periodically
    debugInterrupts();
    
    // Check if WiFi is still up (every 30 seconds)
    if (millis() - lastWifiCheck >= wifiCheckInterval) {
        lastWifiCheck = millis();
        
        IPAddress apIP = WiFi.softAPIP();
        if (apIP == IPAddress(0, 0, 0, 0)) {
            Serial.println("WiFi AP is down, restarting...");
            setupWifi();
        } else {
            int stationCount = WiFi.softAPgetStationNum();
            if (DEBUG_MODE) {
                Serial.print("WiFi AP is up, IP: ");
                Serial.print(apIP.toString());
                Serial.print(", Connected stations: ");
                Serial.println(stationCount);
            }
        }
    }
    
    // Update display if needed
    updateDisplay();
    
    // Handle Weigand card reader data
    if (!flagDone) {
        if (--weigandCounter == 0) {
            flagDone = 1;
            
            if (cardBeingRead) {
                Serial.println("Card read complete, flagDone set");
                Serial.print("Bit count: ");
                Serial.println(bitCount);
                
                // Process the card immediately when read is complete
                processCardNow();
            }
        }
    }
    
    if (bitCount > 0 && flagDone) {
        // This is an additional backup to ensure card data is processed
        // but generally we should already have processed via processCardNow
        Serial.print("Backup card processing with bit count: ");
        Serial.println(bitCount);
        
        processCardNow();
    }
    
    // Keep the system responsive
    delay(10);
}
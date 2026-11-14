/*
 * I2C, PS/2, UART Keyboard for ATtiny414 or 814
 *
 * Pin connections:
 * PB1 -> PS/2 DATA, SDA, UART TX (requires an external pull-up resistor, e.g., 4.7 kΩ to 5V)
 * PB0 -> PS/2 CLOCK, SCL (requires an external pull-up resistor, e.g., 4.7 kΩ to 5V)
 *
 * A short blink after startup indicates keyboard mode:
 * BLUE - Connecting CLOCK to GND will set UART mode (9600 Bd, 8bits, No parity)
 * GREEN - Connecting MODE_PIN to GND will set PS2 keyboard mode (Set 2)
 * RED - Both floating will set I2C mode compatible with CardKB (SLAVE_ADDRESS = 0x5F)
 */
#include <stdbool.h>
#include <stdint.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>
#include <util/delay.h>

/*
 * Low level hardware helpers for ATtiny414/814
 *
 * Pin connections:
 * PB0 -> PS/2 CLOCK, SCL
 * PB1 -> PS/2 DATA, SDA, UART TX
 * PB2 -> WS2812B data
 * PB3 -> MODE pin
 * PA0 -> for UPDI
 * PA1, PA2, PA3 -> SHIFT, SYMBOL, FUNC keys
 * PA5, PA4, PA6, PA7 -> analog inputs of the key matrix
 */

// PORTB pins
#define PS2_CLOCK_PIN PIN0_bm
#define PS2_DATA_PIN  PIN1_bm
#define WS2812_PIN    PIN2_bm
#define MODE_PIN      PIN3_bm

// PORTA pins
#define SHIFT_PIN  PIN1_bm
#define SYMBOL_PIN PIN2_bm
#define FUNC_PIN   PIN3_bm

// Logic levels as returned by the pin read functions
#define LOW  0
#define HIGH 1

static inline uint8_t ReadPinA(uint8_t pin)
{
  return (VPORTA.IN & pin) ? HIGH : LOW;
}

static inline uint8_t ReadPinB(uint8_t pin)
{
  return (VPORTB.IN & pin) ? HIGH : LOW;
}

// Open drain emulation on PORTB: LOW drives the pin to GND, Float switches it
// to high impedance (logic high via an external pull-up)
static inline void OpenDrainLow(uint8_t pin)
{
  VPORTB.OUT &= ~pin;
  VPORTB.DIR |= pin;
}
static inline void OpenDrainFloat(uint8_t pin)
{
  VPORTB.DIR &= ~pin;
}

// *********************************
//      Functions for millis counter
// *********************************
static volatile uint32_t millisCounter;

// Timer interrupt service
ISR(TCB0_INT_vect)
{
  millisCounter++;
  TCB0.INTFLAGS = TCB_CAPT_bm;
}

uint32_t millis(void)
{
  uint32_t ret;

  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    ret = millisCounter;
  }
  return ret;
}

// Global variables for SHIFT, SYMBOL, FUNC keys
// Variable for storing the SHIFT key state
uint8_t lastShiftState = HIGH;
bool processShift = false;
bool isShift = false;
// Variable for storing the SYMBOL key state
uint8_t lastSymbolState = HIGH;
bool processSymbol = false;
bool isSymbol = false;
// Variable for storing the FUNC key state
uint8_t lastFuncState = HIGH;
bool processFunc = false;
bool isFunc = false;
// Flag for update LED
bool changeLED = false;

// *********************************
//      Functions for PS2
// *********************************
// PS/2 Scan Code Set 2 table (make codes) for the specified keys 0 to 52
static const unsigned char PS2_ScanCodes[] PROGMEM = {
  0x16, // 0:  '1'
  0x1E, // 1:  '2'
  0x26, // 2:  '3'
  0x25, // 3:  '4'
  0x2E, // 4:  '5'
  0x36, // 5:  '6'
  0x3D, // 6:  '7'
  0x3E, // 7:  '8'
  0x46, // 8:  '9'
  0x45, // 9:  '0'
  0x15, // 10: 'Q'
  0x1D, // 11: 'W'
  0x24, // 12: 'E'
  0x2D, // 13: 'R'
  0x2C, // 14: 'T'
  0x35, // 15: 'Y'
  0x3C, // 16: 'U'
  0x43, // 17: 'I'
  0x44, // 18: 'O'
  0x4D, // 19: 'P'
  0x1C, // 20: 'A'
  0x1B, // 21: 'S'
  0x23, // 22: 'D'
  0x2B, // 23: 'F'
  0x34, // 24: 'G'
  0x33, // 25: 'H'
  0x3B, // 26: 'J'
  0x42, // 27: 'K'
  0x4B, // 28: 'L'
  0x5A, // 29: 'Enter'
  0x1A, // 30: 'Z'
  0x22, // 31: 'X'
  0x21, // 32: 'C'
  0x2A, // 33: 'V'
  0x32, // 34: 'B'
  0x31, // 35: 'N'
  0x3A, // 36: 'M'
  0x41, // 37: 'colon' (,)
  0x49, // 38: 'dot' (.)
  0x29, // 39: 'Space'
  0x12, // 40: 'Left Shift'
  0x14, // 41: 'Left Ctrl'
  0x0D, // 42: 'Tabulator'
  0x66, // 43: 'Backspace'
  0x0E, // 44: 'Tilde'
  0x4A, // 45: '? + /'
  0x4E, // 46: '_ + -'
  0x55, // 47: '+ + ='
  0x5D, // 48: '| + backslash'
  0x54, // 49: '{ + ['
  0x5B, // 50: '} + ]'
  0x4C, // 51: ': + ;'
  0x52  // 52: '" +''
};

#define PS2_KEYS (sizeof(PS2_ScanCodes) / sizeof(PS2_ScanCodes[0]))

// Sets DATA according to the bit and generates one CLOCK pulse
static void SendPS2Bit(bool bit)
{
  if (bit)
    OpenDrainFloat(PS2_DATA_PIN);
  else
    OpenDrainLow(PS2_DATA_PIN);
  _delay_us(20);
  OpenDrainLow(PS2_CLOCK_PIN);
  _delay_us(40);
  OpenDrainFloat(PS2_CLOCK_PIN);
}

// Send 1 byte to PS2 interface
void SendPS2Byte(unsigned char byteToSend)
{
  unsigned char parity = 1; // PS/2 uses odd parity
  // 1. Start bit (always a logical 0)
  SendPS2Bit(false);
  _delay_us(20);
  // 2. 8 data bits (from the least significant LSB to the most significant MSB)
  for (uint8_t i = 0; i < 8; i++) {
    bool bit = byteToSend & (1 << i);
    if (bit) parity ^= 1; // Odd parity calculation
    SendPS2Bit(bit);
    _delay_us(20);
  }
  // 3. Parity bit
  SendPS2Bit(parity);
  _delay_us(20);
  // 4. Stop bit (always logical 1 = idle)
  SendPS2Bit(true);
  // Short pause between bytes for bus stabilization
  _delay_us(100);
}

// Send key code when pressed/released key
void SendKeyDown(unsigned char Key)
{
  if (Key >= PS2_KEYS) return; // Over-range protection (0 to 52)
  // Loading the scan code from flash memory (PROGMEM)
  unsigned char scanCode = pgm_read_byte(&PS2_ScanCodes[Key]);
  // Sending the keypress code
  SendPS2Byte(scanCode);
}
void SendKeyUp(unsigned char Key)
{
  if (Key >= PS2_KEYS) return; // Over-range protection (0 to 52)
  // Loading the scan code from flash memory (PROGMEM)
  unsigned char scanCode = pgm_read_byte(&PS2_ScanCodes[Key]);
  // A PS/2 key release (Break) is signaled by the prefix 0xF0 followed by the code itself
  SendPS2Byte(0xF0);
  _delay_ms(2);   // The 2-millisec delay here and below is for backward compatibility with IBM PC (it may be omitted for TinyBasRV)
  SendPS2Byte(scanCode); 
}

bool wasAlternateKey = false; // Flag for SYMBOL keys for PS2 remaping

// Shift-key functions for the SYMBOL key in PS/2 mode
void WithShift(unsigned char Key) {
  if(!isShift) { SendKeyDown(40); _delay_ms(2); }    // We have to handle not pressed button Shift
  SendKeyDown(Key); wasAlternateKey = true;
  if(!isShift) { _delay_ms(2); SendKeyUp(40); }
}
// Function for the SYMBOL key (without Shift) in PS/2 mode
void WithoutShift(unsigned char Key) {
  if(isShift) { SendKeyUp(40); _delay_ms(2); }    // We have to handle pressed button Shift
  SendKeyDown(Key); wasAlternateKey = true;
  if(isShift) { _delay_ms(2); SendKeyDown(40); }
}

// *********************************
//      Functions for I2C
// *********************************
// I2C address definition for this slave device (corresponds to CardKB)
const unsigned char  SLAVE_ADDRESS = 0x5F;

// I2C and UART table for the specified keys 0 to 39
const unsigned char  CardKBTable[160] PROGMEM = {
  '1','2','3','4','5','6','7','8','9','0',
  'q','w','e','r','t','y','u','i','o','p',
  'a','s','d','f','g','h','j','k','l','\n',
  'z','x','c','v','b','n','m',',','.',' ',
  // SHIFT
  '1','2','3','4','5','6','7','8','9','0',
  'Q','W','E','R','T','Y','U','I','O','P',
  'A','S','D','F','G','H','J','K','L','\n',
  'Z','X','C','V','B','N','M',',','.',' ',
  // SYMBOL
  '!','@','#','$','%','^','&','*','(',')',
  0x08,'{','}','[',']','/','\\','|',0x27,0x22,
  '~',0x09,'d','f',':',';','+','-','=','\n',
  'z','x','c','v','b','_','?','<','>',' ',
  // FUNC
  '1','2','3','4','5','6','7','8','9','0',
  0x8D,0x8E,0x8F,0x90,0x91,0x92,0x93,0x94,0x95,0x96,
  0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,0xA0,0xA1,0xA2,'\n',
  0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,' '
};

// Global variable (0 = no key, otherwise the key code from CardKBTable)
volatile uint8_t pressedKey = 0;

// Set on address match, the first byte of a read transfer is the key
static bool firstByte;

// I2C interrupt service
ISR(TWI0_TWIS_vect)
{
  uint8_t status = TWI0.SSTATUS;

  // Bus error or collision, abort the transfer and wait for next start
  if (status & (TWI_COLL_bm | TWI_BUSERR_bm)) {
    TWI0.SSTATUS = TWI_COLL_bm | TWI_BUSERR_bm;
    TWI0.SCTRLB = TWI_SCMD_COMPTRANS_gc;
    return;
  }

  if (status & TWI_APIF_bm) {
    if (status & TWI_AP_bm) {
      // Address match, ACK it
      firstByte = true;
      TWI0.SCTRLB = TWI_ACKACT_ACK_gc | TWI_SCMD_RESPONSE_gc;
    } else {
      // Stop condition
      TWI0.SCTRLB = TWI_SCMD_COMPTRANS_gc;
    }
    return;
  }

  if (status & TWI_DIF_bm) {
    if (status & TWI_DIR_bm) {
      // Master read, finish if master NACKed the previous byte
      if (!firstByte && (status & TWI_RXACK_bm)) {
        TWI0.SCTRLB = TWI_SCMD_COMPTRANS_gc;
        return;
      }
      // We will respond to the Master by sending a single byte,
      // any further bytes requested are zeros
      if (firstByte) {
        TWI0.SDATA = pressedKey;
        pressedKey = 0;
        firstByte = false;
      } else {
        TWI0.SDATA = 0;
      }
      TWI0.SCTRLB = TWI_SCMD_RESPONSE_gc;
    } else {
      // Master write, the received data are ignored
      uint8_t dummyByte = TWI0.SDATA; (void)dummyByte;
      TWI0.SCTRLB = TWI_ACKACT_ACK_gc | TWI_SCMD_RESPONSE_gc;
    }
  }
}

// *********************************
//      Functions for WS2812B LED
// *********************************
#define NOP4  "nop\n\t" "nop\n\t" "nop\n\t" "nop\n\t"
#define NOP12 NOP4 NOP4 NOP4

// Sends one byte MSB first
// Optimized for a 20 MHz clock (1 cycle = 50 ns)
static inline __attribute__((always_inline)) void sendByte(uint8_t byte)
{
  for (uint8_t i = 0; i < 8; i++) {
    if (byte & 0x80) {
      // Bit 1: High cca 700ns, Low cca 300ns
      VPORTB.OUT |= WS2812_PIN;  // Set WS2812B input HIGH
      asm volatile(NOP12);
      VPORTB.OUT &= ~WS2812_PIN;  // Set WS2812B input LOW
      asm volatile(NOP4);
    } else {
      // Bit 0: High cca 300ns, Low cca 850ns
      VPORTB.OUT |= WS2812_PIN;   // Set WS2812B input HIGH
      asm volatile(NOP4);
      VPORTB.OUT &= ~WS2812_PIN;  // Set WS2812B input LOW
      asm volatile(NOP12);
    }
    byte <<= 1; // Shift to the next bit
  }
}

// Sets the color of a single WS2812B LED on PB2 (pin has to be an output) 
void setWS2812_Single(uint8_t rbyte, uint8_t gbyte, uint8_t bbyte)
{
  // The WS2812B requires the data order: Green, Red, Blue.
  cli(); // Disabling interrupts for precise timing
  sendByte(gbyte);
  sendByte(rbyte);
  sendByte(bbyte);
  sei(); // Re-enabling interruption

  // Reset signal (logic low held for more than 280 µs) if data were to be sent in rapid succession
  //  _delay_us(280);
}

// Set LED color for func keys (SHIFT most, SYMBOL, FUNC last)
void SetLEDColors(void) {
  if(isShift) setWS2812_Single(15, 0, 0); // Red
  else if(isSymbol) setWS2812_Single(0, 10, 0); // Green
  else if(isFunc) setWS2812_Single(0, 0, 15); // Blue
  else setWS2812_Single(0, 0, 0); // None
}

// *********************************
//   Functions for keyboard matrix
// *********************************

// Analog inputs of the key matrix (PA5, PA4, PA6, PA7)
const uint8_t analogPins[] PROGMEM = {ADC_MUXPOS_AIN5_gc, ADC_MUXPOS_AIN4_gc, ADC_MUXPOS_AIN6_gc, ADC_MUXPOS_AIN7_gc};

// Debounce timing (including ms setting)
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 20;

// Array of upper limits (thresholds) for individual buttons 0 through 9
// If the ADC value is less than or equal to a given threshold, it corresponds to a specific button
// If the value is higher than the last threshold (970), it is evaluated as not pressed
const int buttonThresholds[10] PROGMEM = {
  55,  // Button 0 (theoretically 0 V / 0 ADC) -> range: 0 to 55
  161, // Button 1 (theoretically 0.5 V / 102 ADC) -> range: 56 to 161
  263, // Button 2 (theoretically 1.0 V / 205 ADC) -> range: 162 to 263
  358, // Button 3 (theoretically 1.5 V / 307 ADC) -> range: 264 to 358
  454, // Button 4 (theoretically 2.0 V / 409 ADC) -> range: 359 to 454
  563, // Button 5 (theoretically 2.5 V / 512 ADC) -> range: 455 to 563
  671, // Button 6 (theoretically 3.0 V / 614 ADC) -> range: 564 to 671
  766, // Button 7 (theoretically 3.5 V / 716 ADC) -> range: 672 to 766
  864, // Button 8 (theoretically 4.0 V / 818 ADC) -> range: 767 to 864
  970  // Button 9 (theoretically 4.5 V / 921 ADC) -> range: 865 to 970
};

// Function for converting an analog value to a local button index (0 to 9)
// If no button is pressed, it returns 0xFF.
uint8_t getButtonFromAnalog(int analogValue) {
  // We successively check the boundary limits from the lowest to the highest
  for (uint8_t i = 0; i < 10; i++) {
    if (analogValue <= pgm_read_word(&(buttonThresholds[i]))) {
      return i; // We found a matching range; returning the button index
    }
  }
  // If the value exceeds even the last threshold (970), nothing is pressed (e.g., 1023 at 5V)
  return 0xFF;
}

// Read analog value from pin - ain is mux number
uint16_t ADCRead(uint8_t ain)
{
  ADC0.MUXPOS = ain;
  _delay_us(100);  
  ADC0.COMMAND = 0x01;
  while (!(ADC0.INTFLAGS & 0x01))
  { ; }
  // Reading the result clears the RESRDY flag
  ADC0.INTFLAGS = 0x01;
  return ADC0.RES;
}

// *********************************
//   Function for UART
// *********************************

// Calculation of single-bit duration in microseconds for 9600 baud
// 1 second / 9600 = ~104.16 microseconds
#define BIT_DELAY_US (1000000.0 / 9600)

void SendUART(unsigned char Data)
{
  // 1. Start bit (Logic 0)
  VPORTB.OUT &= ~PS2_DATA_PIN;
  _delay_us(BIT_DELAY_US);
  // 2. Data bits (8 bits, from least significant – LSB – to most significant – MSB)
  for (unsigned char i = 0; i < 8; i++) {
    if (Data & (1 << i))
      VPORTB.OUT |= PS2_DATA_PIN;
    else
      VPORTB.OUT &= ~PS2_DATA_PIN;
    _delay_us(BIT_DELAY_US);
  }
  // 3. Stop bit (Logic 1)
  VPORTB.OUT |= PS2_DATA_PIN;
  _delay_us(BIT_DELAY_US);
}

// Global variables for keyboard mode
uint8_t  KeybMode = 0;    // Keyboard mode: 0=I2C, 1=PS2, 2=UART

uint8_t lastStableButton = 0xFF;
uint8_t activeButton = 0xFF; 


int main(void) {
  // *********************************
  //      Microcontroller setup
  // *********************************
  // The 20 MHz oscillator (selected by the OSCCFG fuse) runs divided by 6
  // after reset, disable the prescaler.
  _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0);
  
  // millis initialization
  // Periodic interrupt mode, CCMP + 1 clock cycles per period
  TCB0.CTRLB = TCB_CNTMODE_INT_gc;
  TCB0.CCMP = F_CPU / 1000 - 1;
  TCB0.CNT = 0;
  TCB0.INTCTRL = TCB_CAPT_bm;
  TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;

  // Configuring pin for Mode setting (input with pull-up)
  PORTB.PIN3CTRL = PORT_PULLUPEN_bm;
  // Configuring the "SHIFT","SYMBOL" and "FUNC" pins as inputs with internal pull-up resistors
  PORTA.PIN1CTRL = PORT_PULLUPEN_bm;
  PORTA.PIN2CTRL = PORT_PULLUPEN_bm;
  PORTA.PIN3CTRL = PORT_PULLUPEN_bm;
  // Configuring keyboard A/D pins as inputs with the digital input buffer disabled
  PORTA.PIN4CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN6CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN7CTRL = PORT_ISC_INPUT_DISABLE_gc;
  // 20 MHz / 16 = 1.25 MHz ADC clock (max 1.5 MHz for 10-bit results)
  ADC0.CTRLC = ADC_SAMPCAP_bm | ADC_REFSEL_VDDREF_gc | ADC_PRESC_DIV16_gc;
  ADC0.CTRLA = ADC_ENABLE_bm;

  // Test Keyboard Mode (which protocol will be used)
  KeybMode = 0;   // Default is I2C mode
  // Configuring the WS2812B pin as an output (used for UART mode test as well)
  VPORTB.DIR |= WS2812_PIN;
  VPORTB.OUT |= WS2812_PIN; _delay_ms(1);
  if(ReadPinB(MODE_PIN) == LOW) KeybMode = 1;    // PS2 mode is set
  // Return WS2812B pin to LOW
  VPORTB.OUT &= ~WS2812_PIN; _delay_ms(1);
  if(ReadPinB(PS2_CLOCK_PIN) == LOW) KeybMode = 2;    // UART mode is set

  // Set interface for keyboard mode
  if(KeybMode == 0) {   // I2C mode
    // Initialization of the I2C slave at a defined address
    TWI0.SADDR = SLAVE_ADDRESS << 1;
    TWI0.SCTRLA = TWI_DIEN_bm | TWI_APIEN_bm | TWI_PIEN_bm | TWI_ENABLE_bm;
    isShift = true; // I2C will be signalled by RED
  }
  else if(KeybMode == 1) {   // PS2 mode
    // Initializing pins to a high-impedance state (logic high ensured by external pull-up resistors)
    OpenDrainFloat(PS2_DATA_PIN);
    OpenDrainFloat(PS2_CLOCK_PIN);
    isSymbol = true; // PS2 will be signalled by GREEN
  }
  else {   // UART mode
    // Idle line is logic 1
    VPORTB.OUT |= PS2_DATA_PIN;
    VPORTB.DIR |= PS2_DATA_PIN;    
    isFunc = true; // UART will be signalled by BLUE
  }

  sei();

  // A brief pause after startup for bus stabilization and MODE signaling.
  SetLEDColors();
  _delay_ms(200);
  isShift = false; isSymbol = false; isFunc = false;
  SetLEDColors();

  // *********************************
  //      Main loop starts here
  // *********************************
  for (;;)
  {
    unsigned long currentMillis = millis();

    // Triggering the check every XX milliseconds (debouncing) – see above debounceDelay
    if (currentMillis - lastDebounceTime >= debounceDelay) {
      lastDebounceTime = currentMillis;

      // Test "Shift" key
      uint8_t currentShiftState = ReadPinA(SHIFT_PIN);
      if(currentShiftState != lastShiftState) { // Key is changing so set only flag for processing after stable
        lastShiftState = currentShiftState;
        processShift = true;
      } else if (processShift) {  // Key is stable already after change - process it
        processShift = false;
        changeLED = true;
        if(lastShiftState == LOW) {  // Key is pressed
          isShift = true;
          if(KeybMode == 1) SendKeyDown(40);
        } else {  // Key is released
          isShift = false;
          if(KeybMode == 1) SendKeyUp(40);
        }
      }

      // Test "Symbol" key
      uint8_t currentSymbolState = ReadPinA(SYMBOL_PIN);
      if(currentSymbolState != lastSymbolState) { // Key is changing so set only flag for processing after stable
        lastSymbolState = currentSymbolState;
        processSymbol = true;
      } else if (processSymbol) {  // Key is stable already after change - process it
        processSymbol = false;
        changeLED = true;
        if(lastSymbolState == LOW) {  // Key is pressed
          isSymbol = true;
        } else {  // Key is released
          isSymbol = false;
        }
      }

      // Test "Func" key
      uint8_t currentFuncState = ReadPinA(FUNC_PIN);
      if(currentFuncState != lastFuncState) { // Key is changing so set only flag for processing after stable
        lastFuncState = currentFuncState;
        processFunc = true;
      } else if (processFunc) {  // Key is stable already after change - process it
        processFunc = false;
        changeLED = true;
        if(lastFuncState == LOW) {  // Key is pressed
          isFunc = true;
          if(KeybMode == 1) SendKeyDown(41);
        } else {  // Key is released
          isFunc = false;
          if(KeybMode == 1) SendKeyUp(41);
        }
      }
      if(changeLED) { changeLED = false; SetLEDColors(); }  // Set LED after change

      // Character key matrix test
      uint8_t currentDetectedKey = 0xFF;
      // Sequential scanning of all 4 analog inputs
      for (uint8_t inputIndex = 0; inputIndex < 4; inputIndex++) {
        int adcValue = ADCRead(pgm_read_byte(&(analogPins[inputIndex])));
        uint8_t localButton = getButtonFromAnalog(adcValue);
        // If a pressed button was detected on this input
        if (localButton != 0xFF) {
          // Conversion to a global index (0 to 39) based on input
          currentDetectedKey = (inputIndex * 10) + localButton;
          break; // We break the loop – we consider only the first button found
        }
      }

      // Debounce logic
      // If the stable state has changed compared to the previous stable state
      if (currentDetectedKey != lastStableButton) {
        lastStableButton = currentDetectedKey;
        // Sequential press logic (interlocking)
        if (activeButton == 0xFF) {
          // If no button was active and the user pressed one
          if (lastStableButton != 0xFF) {
            // The action is performed here when the button is pressed
            activeButton = lastStableButton;
            unsigned int ShiftInTable = 0;
            if(isFunc) ShiftInTable = 120; 
            if(isSymbol) ShiftInTable = 80; 
            if(isShift) ShiftInTable = 40;
            // If MODE == 0, simply saving it to the variable is sufficient, and the interrupt will send it
            pressedKey = pgm_read_byte(&(CardKBTable[activeButton+ShiftInTable]));
            // If MODE == 2, we also send it to the UART
            if(KeybMode == 2) SendUART(pressedKey);
            if(KeybMode == 1) { // It's more complicated with PS2 mode...
              if(isSymbol) {
                // do something different depending on the SYMBOL key:
                switch (activeButton) {
                  case 10:    // Backspace
                    SendKeyDown(43); wasAlternateKey = true;
                    break;
                  case 11:    // {
                    WithShift(49);
                    break;
                  case 12:    // }
                    WithShift(50);
                    break;
                  case 13:    // [
                    WithoutShift(49);
                    break;
                  case 14:    // ]
                    WithoutShift(50);
                    break;
                  case 15:    // /
                    WithoutShift(45);
                    break;
                  case 16:    // backslash
                    WithoutShift(48);
                    break;
                  case 17:    // |
                    WithShift(48);
                    break;
                  case 18:    // '
                    WithoutShift(52);
                    break;
                  case 19:    // "
                    WithShift(52);
                    break;
                  case 20:    // Tilde
                    SendKeyDown(44); wasAlternateKey = true;
                    break;
                  case 21:    // Tabulator
                    SendKeyDown(42); wasAlternateKey = true;
                    break;
                  case 24:    // :
                    WithShift(51);
                    break;
                  case 25:    // ;
                    WithoutShift(51);
                    break;
                  case 26:    // =
                    WithShift(47);
                    break;
                  case 28:    // +
                    WithoutShift(47);
                    break;
                  case 27:    // -
                    WithoutShift(46);
                    break;
                  case 35:    // _
                    WithShift(46);
                    break;
                 case 36:    // ?
                    WithShift(45);
                    break;
                  default:    // No special key
                    SendKeyDown(activeButton);
                }
              } else
              SendKeyDown(activeButton);
            }
          }
        } else {
          // If a button was active and has now been fully released (0xFF)
          if (lastStableButton == 0xFF) {
            // The action performed upon releasing the button takes place here
            if(KeybMode == 1) { // I only have to do something in PS2 mode
              if(wasAlternateKey) {
                // do something different depending on the SYMBOL key:
                switch (activeButton) {
                  case 10:    // Backspace
                    SendKeyUp(43); wasAlternateKey = false;
                    break;
                  case 11:    // {
                  case 13:    // [
                     SendKeyUp(49); wasAlternateKey = false;
                    break;
                  case 12:    // }
                  case 14:    // ]
                     SendKeyUp(50); wasAlternateKey = false;
                    break;
                  case 16:    // backslash
                  case 17:    // |
                     SendKeyUp(48); wasAlternateKey = false;
                    break;
                  case 18:    // '
                  case 19:    // "
                     SendKeyUp(52); wasAlternateKey = false;
                    break;
                  case 20:    // Tilde - not used in TinyBasRV
                     SendKeyUp(44); wasAlternateKey = false;
                    break;
                  case 21:    // Tabulator
                    SendKeyUp(42); wasAlternateKey = false;
                    break;
                  case 24:    // :
                  case 25:    // ;
                     SendKeyUp(51); wasAlternateKey = false;
                    break;
                  case 26:    // +
                  case 28:    // =
                     SendKeyUp(47); wasAlternateKey = false;
                    break;
                  case 27:    // -
                  case 35:    // _
                     SendKeyUp(46); wasAlternateKey = false;
                    break;
                  case 15:    // /
                  case 36:    // ?
                     SendKeyUp(45); wasAlternateKey = false;
                    break;
                  default:    // No special key
                    SendKeyUp(activeButton);
                }
              } else
              SendKeyUp(activeButton);
            }
            activeButton = 0xFF; // We will release the lock for the next button
          }
          // If the user presses another button without releasing the original one,
          // the program ignores this change and waits for lastStableButton to become 0xFF.
        }
      }
    }
  }
}

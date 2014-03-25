#include "SerialModem.h"
#include "platforms/platform.h"

using namespace Modem;


SerialModemClass SerialModem;
// The CircularGapBuffer is created with a reference to the shared buffer so compile
// time has an accurate SRAM usage calculation vs allocating it at run-time
char g_sharedBuffer[SERIAL_MODEM_SHARED_BUFFER];
CircularGapBuffer *g_gapBuffer = new CircularGapBuffer(g_sharedBuffer, SERIAL_MODEM_SHARED_BUFFER);

void sm_hardware_power_toggle(uint8_t pin) {
  if (pin == -1)
    return;
  hw_setPinMode(pin, hw_OUTPUT);
  hw_digitalWrite(pin, hw_HIGH);
  plt_delay(200);
  hw_digitalWrite(pin, hw_LOW);
}

void sm_hardware_set_dtr(uint8_t pin, bool state) {
  if (pin == -1)
    return;
  hw_setPinMode(pin, hw_OUTPUT);
  hw_digitalWrite(pin, state ? hw_HIGH : hw_LOW);
}


SerialModemClass::SerialModemClass() : _hardware_power_pin(-1),
                                       _hardware_dtr_pin(-1),
                                       _sim_pin(NULL),
                                       _powered_on(false) {

}

bool SerialModemClass::begin(SMSerialInterfaceClass serial, uint32_t baud) {
  _hardware_serial = serial;
  _hardware_serial->begin(baud);
  // while(!plt_SERIAL_MODEM);

  if (!_powered_on) {
    // Double check to see if our modem isn't on
    _powered_on == sm_interface_attention();
  }
  if (!_powered_on)
    hwStart();

  return ready();
  // writeCommand("ATE0"); // disable command echo
  // writeCommand("AT&V"); // read current configuration
  // writeCommand("AT+CFUN?");
}

bool SerialModemClass::ready() {
  return assert_hardware_interface() &&
         _powered_on &&
         sm_interface_attention();
}

//
// Hardware Interface
//

void SerialModemClass::setHardwareInterface(sm_interface interface) {
  _hardware_interface = interface;
}

bool SerialModemClass::setSIMPin(char *pin) {
  return !!pin &&
         assert_hardware_interface() &&
         sm_interface_set_sim_pin(pin);
}

bool SerialModemClass::setAPN(char *apn) {
  return assert_hardware_interface() &&
         sm_interface_set_apn(apn);
}

uint8_t SerialModemClass::getNetworkStatus() {
  return assert_hardware_interface() ?
         sm_interface_network_status() : NETWORK_STATUS_UNKNOWN;
}

//
// Hardware Control
//

void SerialModemClass::hwStart() {
  // No need to attempt a power toggle when we "know" we have been powered on
  if (_powered_on)
    return;

  sm_hardware_power_toggle(_hardware_power_pin);
  _powered_on = true;
}

void SerialModemClass::hwShutdown() {
  if (!_powered_on)
    return;

  sm_hardware_power_toggle(_hardware_power_pin);
  _powered_on = false;
}

void SerialModemClass::setHardwarePowerPin(uint8_t pin) {
  _hardware_power_pin = pin;
}

//
// Modem Serial Handling
//

size_t SerialModemClass::writeBytes(const uint8_t *bytes, size_t size) {
  if (!assert_hardware_interface())
    return 0;
#ifdef DEBUG
  // could this be shortened/cleaned up?
  // we are just sniffing outgoing bytes to check for line-breaks for cleaner debug output..
  static bool gWriteIsNewline=true;
  if (gWriteIsNewline)
    DLog("$ ");
  int offset = 0;
  char tmp[83];
  do {
    int chunk = size - offset;
    memset(&tmp, 0, 83);
    strncpy(tmp, (char *)bytes+offset, chunk > 80 ? 80 : chunk);
    DLog(tmp);
    offset+=80;
  } while (offset < size);
  gWriteIsNewline = (bytes[size-1] == '\n');
#endif
  return _hardware_serial->write(bytes, size);
}

uint8_t SerialModemClass::sendBasicCommand(const char *cmd, uint32_t timeout, char esc) {
  char *response = sendCommand(cmd, timeout, esc);

  if (!response)
    return Modem::NO_RESPONSE;
  else if (strcasestr(response, "OK"))
    return Modem::SUCCESS;
  else if (strcasestr(response, "ERROR"))
    return Modem::ERROR;
  else
    return Modem::FAILURE;
}

char * SerialModemClass::sendCommand(const char *cmd, uint32_t timeout, char esc, char *responseCheck) {
  if (!assert_hardware_interface())
    return NULL;

  DLog("$ %s\n", cmd);

  // Clean the input buffer
  while(_hardware_serial->available() > 0) _hardware_serial->read();
  _hardware_serial->write(cmd);
  if (esc)
    _hardware_serial->write(esc);

  uint8_t pos=0;
  char *ptr_write = &g_sharedBuffer[0];
  char *ptr_last_line = ptr_write;
  char *ptr_end = ptr_write + SERIAL_MODEM_SHARED_BUFFER - 1;

  uint32_t started_at = plt_millis();
  bool started=false;
  bool recv_line=false;
  do {
    plt_delay(10);

    // Since our buffer is circular read only a "page" at a time at most
    uint16_t bytesRead = 0;
    while (_hardware_serial->available() || bytesRead > SERIAL_MODEM_SHARED_BUFFER) {
      char ch = _hardware_serial->read();

      if (ch == ESC_CR || (ch >= 32 && ch <= 126)) {
        g_gapBuffer->appendCircular(ch);
        bytesRead++;
      }
    }
    // always ensure string integrity
    *ptr_write = 0;

    char *responseMatch = NULL;
    // if (((responseMatch = g_gapBuffer->strstr(ptr_last_line, "OK\x0D")) && *(responseMatch+2) == ESC_CR) ||
    //     ((responseMatch = g_gapBuffer->strstr(ptr_last_line, "ERROR\x0D")) && *(responseMatch+5) == ESC_CR) ||
    //     (responseCheck && (responseMatch = strstr(ptr_last_line, responseCheck)) && *(responseMatch+strlen(responseCheck)) == ESC_CR)) {
    //   DLog(">> %s", g_sharedBuffer);
    //   return g_sharedBuffer;
    // }

    // if (!started) {
    //   char *match = strstr(g_sharedBuffer, cmd);
    //   if (recv_line && !match)
    //     started = true;
    //   else if (pos > strlen(cmd) + 2)
    //     started = true;
    //   // match && recv_line)
    //   // if (match && *(match + strlen(cmd)) == 13) {
    //   //   started = true;
    //   //   DLog(" * started with echo match!\n");
    //   //   DLog(">> %s\n", g_sharedBuffer);
    //   // }
    //   // else if (ptr_write > &g_sharedBuffer[0] + strlen(cmd) + 2) {
    //   //   started = true;
    //   //   DLog(" * started with length!\n");
    //   //   DLog(">> %s\n", g_sharedBuffer);
    //   // }
    // }
    // else if (ptr_prev == ptr_write) {
    //   DLog("> %s\n", g_sharedBuffer);
    //   return g_sharedBuffer;
    // }
  } while((plt_millis() - started_at) < timeout);
  DLog(" * sendCommand timeout\n");
  DLog(">> %s\n", g_sharedBuffer);
  return NULL;
}

uint8_t SerialModemClass::readLine(char *buffer, uint8_t size, unsigned int timeout) {
  if (!assert_hardware_interface())
    return 0;
  uint8_t pos=0;
  unsigned long previous = plt_millis();
  do {
    if (_hardware_serial->available() == 0)
      continue;
    char ch = _hardware_serial->read();
    // filter out non-ascii characters (possible rx interference?)
    if (ch >= 32 && ch <= 126)
      buffer[pos++] = ch;
    else if (ch == 10 || ch == 13) {
      // check for an extra line-break/carriage-return and munch it
      while ((ch = _hardware_serial->peek()) != 0) {
        if (ch == 10 || ch == 13)
          _hardware_serial->read();
        else
          break;
      }
      // an empty line is ignored
      if (pos == 0)
        continue;

      buffer[pos] = 0;
      DLog("> %s\n", buffer);
      return pos;
    }
  } while((millis() - previous) < timeout && pos < size);
  return 0;
}

bool SerialModemClass::assert_hardware_interface() {
  if (!_hardware_interface) {
    DLog("* ERROR: no specified hardware interface\n");
    return false;
  }
  if (!_hardware_serial) {
    DLog("* ERROR: no specified hardware serial\n");
    return false;
  }
  return true;
}

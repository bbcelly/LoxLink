//
//  MMM_LoxNATExtension.cpp
//
//  Created by Markus Fritze on 03.03.19.
//  Copyright (c) 2019 Markus Fritze. All rights reserved.
//

#include "LoxNATExtension.hpp"
#include "LED.hpp"
#include "LoxCANBaseDriver.hpp"
#include "global_functions.hpp"
extern "C" {
  #include "CryptoCanAlgo.h"
}
#include "stm32f1xx_hal.h" // HAL_IncTick
#include <assert.h>
#include <__cross_studio_io.h>
#include <string.h>

/***
 *  Internal function to send a message to the driver
 ***/
void LoxNATExtension::send_message(LoxMsgNATCommand_t command, LoxCanMessage &msg) {
  msg.commandNat = command;
  msg.directionNat = LoxCmdNATDirection_t_fromDevice;
  msg.busType = this->busType;
  driver.SendMessage(msg);
}

/***
 *  Send a Search_Reply or NAT_Index_Request command, even if the extension does not have a NAT
 ***/
void LoxNATExtension::send_special_message(LoxMsgNATCommand_t command) {
  assert(command == Search_Reply || command == NAT_Index_Request);
  LoxCanMessage msg;
  msg.commandNat = command;
  msg.extensionNat = (this->state == eDeviceState_offline) ? (crc8_default(&this->serial, 4) | 0x80) : this->extensionNAT;
  msg.value16 = this->device_type;
  msg.value32 = this->serial;
  send_message(command, msg);
  // Extension only accept broadcast messages, after the NAT Index Request has been sent.
  // This seems to be ok for way the Miniserver works.
  if (command == NAT_Index_Request) {
    driver.FilterSetupNAT(0, this->busType, 0xFF); // 0xFF = broadcast extension NAT
  }
}

/***
 *  Send regular package, but only if a NAT has been assigned to the extension
 ***/
void LoxNATExtension::lox_send_package_if_nat(LoxMsgNATCommand_t command, LoxCanMessage &msg) {
  // extension NAT not set?
  if (this->extensionNAT == 0x00)
    return;
  msg.deviceNAT = driver.isLoxoneLinkBusDriver() ? this->deviceNAT : this->extensionNAT;
  msg.commandNat = command;
  msg.extensionNat = this->extensionNAT;
  send_message(command, msg);
}

/***
 *  Send a digital value
 ***/
void LoxNATExtension::send_digital_value(uint8_t index, uint32_t value) {
  LoxCanMessage msg;
  msg.value8 = index;
  msg.value32 = value;
  lox_send_package_if_nat(Digital_Value, msg);
}

/***
 *  Send an analog value with a specific format
 ***/
void LoxNATExtension::send_analog_value(uint8_t index, uint32_t value, uint16_t flags, eAnalogFormat format) {
  LoxCanMessage msg;
  msg.value8 = index;
  msg.data[2] = flags >> 8;
  msg.data[1] = format | (flags & 0xF0);
  msg.value32 = value;
  lox_send_package_if_nat(Analog_Value, msg);
}

/***
 *  Send a frequency value
 ***/
void LoxNATExtension::send_frequency_value(uint8_t index, uint32_t value) {
  LoxCanMessage msg;
  msg.value8 = index;
  msg.value32 = value;
  lox_send_package_if_nat(Frequency, msg);
}

/***
 *  Send a fragmented package, which can be longer than 7 bytes.
 ***/
void LoxNATExtension::send_fragmented_message(LoxMsgNATCommand_t command, const void *data, int size) {
  LoxCanMessage msg;

  // Send the fragmented header
  msg.value8 = command;
  msg.value16 = size;
  msg.value32 = crc32_stm32_aligned(data, size);
  msg.deviceNAT = this->deviceNAT;
  msg.fragmented = LoxCmdNATPackage_t_fragmented;
  lox_send_package_if_nat(Fragment_Start, msg);

  // send the rest of the data in 7 bytes blocks
  int packageCount = (size + 6) / 7;
  int offset = 0;
  while (packageCount-- > 0) {
    int packageSize = 7;
    if (packageCount == 0)
      packageSize = size - offset;
#if DEBUG
    memset(&msg.data, 0, 7); // remove garbage data
#endif
    memmove(&msg.data, (uint8_t *)data + offset, packageSize);
    offset += 7;
    lox_send_package_if_nat(Fragment_Data, msg);
  }
}

/***
 *  The alive request is send to the Miniserver to confirm the extension is still online.
 *  If the config CRC is not matching, what the Miniserver expects, it will send a new
 *  configuration.
 ***/
void LoxNATExtension::send_alive_package(void) {
  LoxCanMessage msg;
  msg.value8 = eAliveReason_t_alive_packet;
  msg.value32 = config_CRC();
  lox_send_package_if_nat(Alive_Packet, msg);
}

/***
 *  
 ***/
void LoxNATExtension::send_can_status(LoxMsgNATCommand_t command, eTreeBranch branch) {
  LoxCanMessage msg;
  msg.value8 = branch; // from the device (!=0 => from a Tree bus)
  msg.data[1] = this->driver.GetReceiveErrorCounter();
  msg.data[2] = this->driver.GetTransmitErrorCounter();
  msg.value32 = this->driver.GetErrorCounter();
  lox_send_package_if_nat(command, msg);
}

/***
 *  Information package to inform the Miniserver about the hardware
 ***/
void LoxNATExtension::send_info_package(LoxMsgNATCommand_t command, uint8_t /*eAliveReason_t*/ reason) {
  struct __attribute__((__packed__)) { // device_type is unaligned, so we have to marked it packed
    uint32_t version;
    uint32_t unknown; // seems to be ignored by the Miniserver and never set in the extension
    uint32_t config_crc;
    uint32_t serial;
    uint8_t /*eAliveReason_t*/ reason;
    uint16_t /*eDeviceType_t*/ device_type;
    uint8_t hardware_version; // revision of the hardware
  } infoPackage;
  assert(sizeof(infoPackage) == 20); //, "infoPackage size wrong");
  infoPackage.version = this->version;
  infoPackage.unknown = 0;
  infoPackage.config_crc = config_CRC();
  infoPackage.serial = this->serial;
  infoPackage.reason = reason;
  infoPackage.device_type = this->device_type;
  infoPackage.hardware_version = this->hardware_version;
  send_fragmented_message(command, &infoPackage, sizeof(infoPackage));
}

/***
 *  Update package received. We do not support a firmware update, so this does nothing.
 ***/
void LoxNATExtension::update(const eUpdatePackage *updatePackage) {
  if (this->state == eDeviceState_parked)
    return;
  if (updatePackage->device_type != this->device_type)
    return;
  if (updatePackage->version > this->version)
    return;

  switch (updatePackage->updatePackageType) {
  case eUpdatePackageType_write_flash:
    debug_printf("write flash\n");
    break;
  case eUpdatePackageType_receive_crc:
    debug_printf("receive CRC\n");
    break;
  case eUpdatePackageType_verify:
    debug_printf("verify\n");
    break;
  case eUpdatePackageType_verify_and_reset:
    debug_printf("verify and reset\n");
    break;
  default:
    break;
  }
}

/***
 *  Calculation of the configuration CRC
 ***/
uint32_t LoxNATExtension::config_CRC(void) {
  memset((uint8_t *)this->configPtr + this->configPtr->size - 4, 0, 4);           // erase the last 4 bytes of the configuration
  return crc32_stm32_aligned(this->configPtr, ((configPtr->size - 1) >> 2) << 2); // the CRC is calculated rounded down to dividable by 4
}

/***
 *  New configuration sent from the server
 ***/
void LoxNATExtension::config_data(const tConfigHeader *config) {
  if (!config)
    return;
  assert(config->size >= 12);
  if (config->size == configSize && config->version == configVersion) {
    memmove(this->configPtr, config, config->size);
    this->offlineTimeout = this->configPtr->offlineTimeout;
    gLED.set_sync_offset(this->configPtr->blinkSyncOffset);
    ConfigUpdate();
  } else { // undefined values trigger a reset of the structure
    memset(this->configPtr, 0, this->configSize);
    this->configPtr->size = this->configSize;
    this->configPtr->version = this->configVersion;
    ConfigLoadDefaults();
  }
}

/***
 *  Update the extension state
 ***/
void LoxNATExtension::SetState(eDeviceState state) {
  LoxExtension::SetState(state);
  if (state != eDeviceState_offline)
    this->NATStateCounter = 0;
}

/***
 *  Constructor
 ***/
LoxNATExtension::LoxNATExtension(LoxCANBaseDriver &driver, uint32_t serial, eDeviceType_t device_type, uint8_t hardware_version, uint32_t version, uint8_t configVersion, uint8_t configSize, tConfigHeader *configPtr, eAliveReason_t alive)
  : LoxExtension(driver, serial, device_type, hardware_version, version), busType(LoxCmdNATBus_t_LoxoneLink), configVersion(configVersion), configSize(configSize), configPtr(configPtr), aliveReason(alive), extensionNAT(0x00), deviceNAT(0x00), upTimeInMs(0) {
  assert(configPtr != NULL);
  assert(configSize <= MAX_FRAGMENT_SIZE);
  this->configPtr->size = configSize;
  this->configPtr->version = configVersion;
  this->NATStateCounter = 0;
  this->randomNATIndexRequestDelay = random_range(10, 500);
  this->offlineTimeout = 15 * 60;
  this->offlineCountdownInMs = this->offlineTimeout * 1000;
  SetState(eDeviceState_offline);
  gLED.identify_off();
}

/***
 *  A direct message received
 ***/
void LoxNATExtension::ReceiveDirect(LoxCanMessage &message) {
  LoxCanMessage msg;
  switch (message.commandNat) {
  case Ping:
    lox_send_package_if_nat(Pong, msg);
    break;
  case Alive_Packet:
    if (message.value32 == config_CRC()) {
      lox_send_package_if_nat(Config_Equal, msg);
    } else {
      send_alive_package();
    }
    break;
  case CAN_Diagnosis_Request:
    if (message.value16 == 0) { // for this device (!=0 => for a Tree bus)
      send_can_status(CAN_Diagnosis_Reply, eTreeBranch_extension);
    }
    break;
  case CAN_Error_Request:
    if (message.value16 == 0) { // for this device (!=0 => for a Tree bus)
      send_can_status(CAN_Error_Reply, eTreeBranch_extension);
    }
    break;
  default:
    LoxExtension::ReceiveDirect(message);
  }
}

/***
 *  A broadcast message received
 ***/
void LoxNATExtension::ReceiveBroadcast(LoxCanMessage &message) {
  switch (message.commandNat) {
  case Identify_LED:
    if (this->serial == message.value32) {
      gLED.identify_on();
    } else {
      gLED.identify_off();
    }
    break;
  case Search_Devices:
    driver.Delay(random_range(0, 100));
    send_special_message(Search_Reply);
    break;
  case NAT_Offer:
    if (this->serial == message.value32) {
      uint8_t nat = message.data[0]; // NAT Index of the offer
      if (message.data[1] & 1) {
        this->extensionNAT = nat;
        send_info_package(Start, this->aliveReason ? this->aliveReason : eAliveReason_t_pairing);
        SetState(eDeviceState_parked);
      } else if ((nat & 0x80) == 0x00) { // a parked NAT index is ignored
        this->extensionNAT = nat;
        driver.FilterSetupNAT(1, this->busType, nat);
        SetState(eDeviceState_online);
        send_info_package(Start, this->aliveReason ? this->aliveReason : eAliveReason_t_pairing);
        if ((message.data[1] & 2) == 0x00) {
          SendValues();
        }
      }
      this->aliveReason = eAliveReason_t_unknown;
    }
    break;
  case Identify_Unknown_Extensions:
    if (this->state == eDeviceState_parked) {
      driver.Delay(random_range(0, 100));
      send_special_message(NAT_Index_Request);
    }
    break;
  case Park_Devices:
    this->extensionNAT = crc8_default(&this->serial, 4) | 0x80; // mark as a parked device
    SetState(eDeviceState_parked);
    break;
  case Sync_Packet:
    gLED.sync(message.value32);
    break;
  case Version_Request:
    if (this->serial == message.value32) {
      send_info_package(Device_Version, eAliveReason_t_alive_packet);
    }
    break;
  default:
    LoxExtension::ReceiveDirect(message);
    break;
  }
}

/***
 *  A direct fragmented message received
 ***/
void LoxNATExtension::ReceiveDirectFragment(LoxMsgNATCommand_t command, uint8_t extensionNAT, uint8_t deviceNAT, const uint8_t *data, uint16_t size) {
  switch (command) {
  case Config_Data:
    config_data((const tConfigHeader *)data);
    break;
  case Update_Reply:
    update((const eUpdatePackage *)data);
    break;
  case CryptoDeviceIdRequest: // so far only Tree devices are asked for a DeviceId
    static uint32_t decryptData[4]; // 16 bytes
    memcpy(decryptData, data, sizeof(decryptData));
    CryptoCanAlgo_DecryptInitPacketLegacy((uint8_t *)decryptData, sizeof(decryptData), this->serial);
    static uint32_t replyData[8]; // 32 bytes
    memset(replyData, 0, sizeof(replyData));
    if(decryptData[0] == 0xdeadbeef) {
      replyData[0] = 0xdeadbeef;
    } else { // illegal request?
      replyData[0] = 0; // return a correct, but invalid reply
    }
    replyData[1] = random_range(0, 0xFFFF);
    // generate a base serial from the STM32 UID
    // Warning: be aware that two relay extension need two different serial numbers!
    HAL_GetUID((uint32_t *)this->cryptDeviceID);
    memcpy(replyData + 2, this->cryptDeviceID, sizeof(cryptDeviceID));
    CryptoCanAlgo_EncryptInitPacketLegacy((uint8_t *)replyData, sizeof(replyData), this->serial);
    send_fragmented_message(CryptoDeviceIdReply, replyData, sizeof(replyData));
    break;
  case CryptoDeviceIdReply: // we should never receive this one
    break;
  case CryptoChallengeRequest: // from the Miniserver: check the authorization of an extension
    if(this->driver.isTreeBusDriver()){
      // The TreeDevice overrides the Master cryptDeviceID after it has received a
      // CryptoDeviceIdRequest, but this process only happens the first time that the
      // Miniserver discovers the TreeDevice. If the LoxLink-based TreeDevice gets reset,
      // the cryptDeviceID variable gets reset to its default state. This means that the
      // TreeDevice will encrypt the CryptoDeviceIdReply with the Master cryptDeviceID
      // while the Miniserver expects that the UID will be used.
      HAL_GetUID((uint32_t *)this->cryptDeviceID);
    }
    memcpy(decryptData, data, sizeof(decryptData));
    CryptoCanAlgo_DecryptInitPacket((uint8_t *)decryptData, this->serial);
    if(decryptData[0] == 0xdeadbeef) {
      CryptoCanAlgo_SolveChallenge(decryptData[1], this->serial, this->cryptDeviceID, this->cryptAesKey, &this->cryptAesIV);
      memset(decryptData, 0xa5, sizeof(decryptData));
      decryptData[0] = 0xdeadbeef;
      decryptData[1] = random_range(0, 0xFFFF);
      CryptoCanAlgo_EncryptDataPacket((uint8_t *)decryptData, this->cryptAesKey, this->cryptAesIV);
      send_fragmented_message(CryptoChallengeReply, decryptData, sizeof(decryptData));
    }
    break;
  case CryptoChallengeReply: // from the Miniserver: validate an existing authorization
    memcpy(decryptData, data, sizeof(decryptData));
    CryptoCanAlgo_DecryptDataPacket((uint8_t *)decryptData, this->cryptAesKey, this->cryptAesIV);
    if(decryptData[0] == 0xdeadbeef) {
      memset(decryptData, 0xa5, sizeof(decryptData));
      decryptData[0] = 0xdeadbeef;
      decryptData[1] = random_range(0, 0xFFFF);
      CryptoCanAlgo_EncryptDataPacket((uint8_t *)decryptData, this->cryptAesKey, this->cryptAesIV);
      send_fragmented_message(CryptoChallengeReply, decryptData, sizeof(decryptData));
    }
    break;
  default:
    break;
  }
}

/***
 *  A broadcast fragmented message received
 ***/
void LoxNATExtension::ReceiveBroadcastFragment(LoxMsgNATCommand_t command, uint8_t extensionNAT, uint8_t deviceNAT, const uint8_t *data, uint16_t size) {
  switch (command) {
  case Update_Reply:
    update((const eUpdatePackage *)data);
    break;
  default:
    break;
  }
}

/***
 *  10ms Timer to be called 100x per second
 ***/
void LoxNATExtension::Timer10ms(void) {
  this->upTimeInMs += 10; // just for debugging purposes

  // If offline, try to get a NAT from the Miniserver.
  // The timing is quasi-random to avoid too much load on the bus after power-on
  if (this->state == eDeviceState_offline) {
    this->randomNATIndexRequestDelay -= 10;
    if (this->randomNATIndexRequestDelay <= 0) {
      int minv, maxv;
      if (this->NATStateCounter <= 2) {
        this->NATStateCounter++;
        minv = 1000;
        maxv = 2.5 * 1000;
      } else if (this->NATStateCounter < 10) {
        this->NATStateCounter++;
        minv = 5 * 1000;
        maxv = 10 * 1000;
      } else {
        minv = 10 * 1000;
        maxv = 30 * 1000;
      }
      this->randomNATIndexRequestDelay = random_range(minv, maxv);
      send_special_message(NAT_Index_Request);
    }
  }

  // monitor incoming package from the Miniserver. The server sends at least
  // one package per minute (the Sync_Packet). If this package doesn't
  // arrive for several minutes, try contacting the Miniserver and if this
  // doesn't work, switch to the offline state.
  if (this->offlineCountdownInMs) {                 // is the offline timer active?
    if ((this->offlineCountdownInMs % 1000) == 0) { // once per second
      int seconds = this->offlineCountdownInMs / 1000;
      // 10% before the end of the timeout, an Alive package is sent to the Miniserver
      if (seconds == this->offlineTimeout / 10)
        send_alive_package();
      if (seconds == 1) // at the end of the timeout
        SetState(eDeviceState_offline);
    }
    this->offlineCountdownInMs -= 10;
  }
}

/***
 *  A message was received. Called from the driver.
 ***/
void LoxNATExtension::ReceiveMessage(LoxCanMessage &message) {
  // ignore non-NAT packages or messages from devices.
  // This is not necessary with a correct CAN filter.
  if (!message.isNATmessage(this->driver) || message.directionNat < LoxCmdNATDirection_t_fromServerShortcut)
    return;

  this->offlineCountdownInMs = this->offlineTimeout * 1000;

  switch (message.commandNat) {
  case Fragment_Start:
    if (message.fragmented) { // This bit has to be set in the message for fragmented messages
      this->fragCommand = LoxMsgNATCommand_t(message.value8);
      this->fragSize = message.value16;
      this->fragCRC = message.value32;
      this->fragOffset = 0;
      if (this->fragSize >= sizeof(this->fragBuffer) - sizeof(message.data)) // package too large for our buffer
        this->fragSize = 0;
    }
    break;
  case Fragment_Data:
    if (message.fragmented) { // This bit has to be set in the message for fragmented messages
      int size = this->fragSize - this->fragOffset;
      if (size > sizeof(message.data))
        size = sizeof(message.data);
      memmove(this->fragBuffer + this->fragOffset, message.data, size);
      this->fragOffset += size;
      if (this->fragOffset != this->fragSize) // not enough bytes received?
        break;
      if (this->fragCRC != crc32_stm32_aligned(this->fragBuffer, this->fragSize)) // checksum wrong?
        break;
      // complete fragmented message received
      if (message.extensionNat == 0xFF) {
        ReceiveBroadcastFragment(this->fragCommand, message.extensionNat, message.deviceNAT, this->fragBuffer, this->fragSize);
      } else if (this->extensionNAT && message.extensionNat == this->extensionNAT) {
          ReceiveDirectFragment(this->fragCommand, message.extensionNat, message.deviceNAT, this->fragBuffer, this->fragSize);
      }
    }
    break;
  default:
    // standard messages received
    if (message.extensionNat == 0xFF) {
      ReceiveBroadcast(message);
    } else if (this->extensionNAT && message.extensionNat == this->extensionNAT) {
//      if (message.deviceNAT == 0x00 || driver.isTreeBusDriver()) { // to this device?
        ReceiveDirect(message);
//      }
    }
  }
}
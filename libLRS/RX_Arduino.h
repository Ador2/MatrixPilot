#include "libLRS.h"

/****************************************************
 * OpenLRSng receiver code
 ****************************************************/

//uint8_t RF_channel = 0;
void channel_hop(void);

uint32_t lastPacketTimeUs = 0;

uint8_t  lastRSSIvalue = 0;
uint8_t  smoothRSSI = 0;
uint8_t  compositeRSSI = 0;
uint16_t lastAFCCvalue = 0;

uint16_t linkQuality = 0;

uint8_t failsafeActive = 0;

uint8_t numberOfLostPackets = 0;
boolean fs_saved = 0;

uint16_t RSSI2Bits(uint8_t rssi)
{
  uint16_t ret = (uint16_t)rssi << 2;
  if (ret < 12) {
    ret = 12;
  } else if (ret > 1012) {
    ret = 1012;
  }
  return ret;
}

void set_RSSI_output()
{
  uint8_t linkq = countSetBits(linkQuality & 0x7fff);
  if (linkq == 15) {
    // RSSI 0 - 255 mapped to 192 - ((255>>2)+192) == 192-255
    compositeRSSI = (smoothRSSI >> 2) + 192;
  } else {
    // linkquality gives 0 to 14*13 == 182
    compositeRSSI = linkq * 13;
  }
  if (rx_config.RSSIpwm < 16) {
    cli();
    PPM[rx_config.RSSIpwm] = RSSI2Bits(compositeRSSI);
    sei();
  }
}

uint8_t bindReceivePkt(void)
{
  int8_t result = 0;
  uint8_t rxb;

      spiReceive(&rxb, 1);

//      switch (rxb)
//        case 'b':
//        case 'p':
//        case 'i':
//        case 't':
//        case 'u':

      if (rxb == 'b') {
        spiReceive((uint8_t*)&bind_data, sizeof(bind_data));

        echoBindData();

        if (bind_data.version == BINDING_VERSION) {
          SERIAL_PRINTLN("data good\n");
          rxb = 'B';
          tx_packet(&rxb, 1); // bindReceive response, ACK that we got bound
          Green_LED_ON; //signal we got bound on LED:s
          return 1;
        }
      } else if ((rxb == 'p') || (rxb == 'i')) {
        uint8_t rxc_buf[sizeof(rx_config) + 1];
        if (rxb == 'p') {
          rxc_buf[0] = 'P';
//          timeout = 0;
          result = -1;
        } else {
          rxInitDefaults(1);
          rxc_buf[0] = 'I';
        }
        memcpy(rxc_buf + 1, &rx_config, sizeof(rx_config));
        tx_packet(rxc_buf, sizeof(rx_config) + 1); //  bindReceive response
      } else if (rxb == 't') {
        uint8_t rxc_buf[5];
        timeout = 0;
        rxc_buf[0] = 'T';
        rxc_buf[1] = (version >> 8);
        rxc_buf[2] = (version & 0xff);
        rxc_buf[3] = OUTPUTS;
        rxc_buf[4] = 0;
        tx_packet(rxc_buf, 5); // bindReceive response
      } else if (rxb == 'u') {
        spiReceive((uint8_t*)&rx_config, sizeof(rx_config));
        rxWriteEeprom();
        rxb = 'U';
        tx_packet(&rxb, 1); // bindReceive response, ACK that we updated settings
      }
      RF_Mode = RF_RECEIVE_REQ; // _bindReceive()
      rx_reset(); // _bindReceive()
  return result;
}

uint8_t bindReceive(uint32_t timeout)
{
  uint32_t start = millis();
  int8_t result = 0;

  init_rfm(1); // initialise in bind mode
  to_rx_mode(); // _bindReceive()
  SERIAL_PRINTLN("Waiting bind\n");

  while ((!timeout) || ((millis() - start) < timeout)) {
    if (RF_Mode == RF_RECEIVE_SIG) {
      SERIAL_PRINTLN("Got pkt\n");
      result = bindReceivePkt();
      if (result < 0) {
        timeout = 0;
      }
      if (result > 0) {
        return 1;
      }
    }
  }
  return 0;
}

uint8_t rx_buf[21]; // RX buffer (uplink)
// First byte of RX buf is
// MSB..LSB [1bit uplink seqno.] [1bit downlink seqno] [6bits type)
// type 0x00 normal servo, 0x01 failsafe set
// type 0x38..0x3f uplinked serial data

uint8_t tx_buf[9]; // TX buffer (downlink)(type plus 8 x data)
// First byte is meta
// MSB..LSB [1 bit uplink seq] [1bit downlink seqno] [6bits telemtype]
// type 0x00 link info [RSSI] [AFCC]*2 etc...
// type 0x38-0x3f downlink serial data 1-8 bytes

uint8_t hopcount;

boolean rfm_device_detected = false;

void checkBind(void)
{
  if (checkBindPlug() || !bindReadEeprom()) {
//    SERIAL_PRINT("EEPROM data not valid or bind jumper set, forcing bind\n");
    if (checkBindPlug()) {
      SERIAL_PRINT("bind jumper set, forcing bind\n");
    } else {
      SERIAL_PRINT("EEPROM data not valid, forcing bind\n");
    }

    if (bindReceive(0)) {
      bindWriteEeprom();
      SERIAL_PRINTLN("Saved bind data to EEPROM\n");
      Green_LED_ON;
    }
  } else {
    if (rx_config.flags & ALWAYS_BIND) {
      if (bindReceive(500)) {
        bindWriteEeprom();
        SERIAL_PRINTLN("Saved bind data to EEPROM\n");
        Green_LED_ON;
      }
    }
  }
}

void setup()
{
  DPRINT("setup()\r\n");
  if (!(rfm_device_detected = init_RFM22B())) {
    SERIAL_PRINT("init_RFM22B failed\r\n");
    return;
  }

  if (!rxReadEeprom()) {
    rxInitDefaults(1);
  }
//  failsafeLoad();
  SERIAL_PRINT("OpenLRSng RX starting\n");

  sei();
  Red_LED_ON;
  checkBind();
  SERIAL_PRINTLN("Entering normal mode\n");

  init_rfm(0);   // Configure the RFM22B's registers for normal operation
//  RF_channel = 0;
//  rfmSetChannel(RF_channel);
  channel_hop();

  // Count hopchannels as we need it later
  hopcount = 0;
  while ((hopcount < MAXHOPS) && (bind_data.hopchannel[hopcount] != 0)) {
    hopcount++;
  }

  //################### RX SYNC AT STARTUP #################
  to_rx_mode(); // _setup()
  lastPacketTimeUs = micros();

  DPRINT("getInterval = %lu us\r\n", getInterval(&bind_data));
  DPRINT("getPacketSize = %u\r\n", getPacketSize(&bind_data));
  DPRINT("getInterval * hopcount = %lu\r\n", (getInterval(&bind_data) * hopcount));
}
/*
// TODO: i doubt this works, especially since it's the only caller of to_rx_mode which doesn't set the mode flag
void locked_module_chk(void)
{
  if (spiReadRegister(0x0C) == 0) {     // detect the locked module and reboot
    SERIAL_PRINTLN("RX hang");
    init_rfm(0);
    to_rx_mode(); // locked_module_chk()
  }
}
 */
void channel_pkt(void)
{
      cli();
      unpackChannels(bind_data.flags & 7, PPM, rx_buf + 1);
      if (rx_config.RSSIpwm < PPM_CHANNELS) {
        PPM[rx_config.RSSIpwm] = RSSI2Bits(compositeRSSI);
      }
      sei();

#if defined( __dsPIC30F__ ) || defined( __dsPIC33F__ ) || defined(__dsPIC33E__)
      lrsIn_setInput(PPM, PPM_CHANNELS);
#else
#endif // dsPIC
      if (rx_buf[0] & 0x01) {
        if (!fs_saved) {
          failsafeSave();
          fs_saved = 1;
        }
      } else if (fs_saved) {
        fs_saved = 0;
      }
}

void serial_pkt(void)
{
      // something else than servo data...
      if ((rx_buf[0] & 0x38) == 0x38) {
        if ((rx_buf[0] ^ tx_buf[0]) & 0x80) {
          // We got new data... (not retransmission)
//          uint8_t i;
          tx_buf[0] ^= 0x80; // signal that we got it
          put_serial_bytes(&rx_buf[1], (rx_buf[0] & 7)); // handle serial data input from transmitter
//          for (i = 0; i <= (rx_buf[0] & 7);) {
//            i++;
//            SERIAL_WRITE(rx_buf[i]);
//			put_serial_byte(rx_buf[i]);
//          }
        }
      }
}

static void transmit_pkt(void)
{
      if ((tx_buf[0] ^ rx_buf[0]) & 0x40) {
        // resend last message
      } else {
        tx_buf[0] &= 0xc0;
        tx_buf[0] ^= 0x40; // swap sequence as we have new data
		uint8_t bytes = 0;
		if ((bytes = get_serial_bytes(&tx_buf[1], 8))) {
          tx_buf[0] |= (0x37 + bytes);
        } else {
          // tx_buf[0] lowest 6 bits left at 0
          tx_buf[1] = lastRSSIvalue;
          tx_buf[2] = 0;
          tx_buf[3] = 0;
          tx_buf[4] = (lastAFCCvalue >> 8);
          tx_buf[5] = lastAFCCvalue & 0xff;
          tx_buf[6] = countSetBits(linkQuality & 0x7fff);
        }
      }
#ifdef TEST_NO_ACK_BY_CH0
      if (PPM[0] < 900)
#endif
      {
//OLS3 = 0;
        tx_packet(tx_buf, 9); // transmit_pkt()
      }
}

void receive_pkt(void)
{
    uint32_t timeTemp = micros();

//OLS3 = 1;

    spiReceive(rx_buf, getPacketSize(&bind_data));
    lastAFCCvalue = rfmGetAFCC();
    lastPacketTimeUs = timeTemp; // used saved timestamp to avoid skew by I2C
//    numberOfLostPackets = 0; // MOVED TO CALLING MAINLOOP
    linkQuality <<= 1;
    linkQuality |= 1;
    Red_LED_OFF;
    Green_LED_ON;
    if ((rx_buf[0] & 0x3e) == 0x00) {
      channel_pkt();
    } else {
      serial_pkt();
    }
    failsafeActive = 0;
    lrsIn_setFailsafe(false);
    if (bind_data.flags & TELEMETRY_MASK) {
      transmit_pkt();
    }
    rx_reset(); // _receive_pkt()
    Green_LED_OFF;
}

void sample_rssi(void)
{
	static uint8_t RSSI_count = 0;
	static uint16_t RSSI_sum = 0;

    lastRSSIvalue = rfmGetRSSI(); // Read the RSSI value
    RSSI_sum += lastRSSIvalue;    // tally up for average
    RSSI_count++;

    if (RSSI_count > 8) {
      RSSI_sum /= RSSI_count;
      smoothRSSI = (((uint16_t)smoothRSSI * 3 + (uint16_t)RSSI_sum * 1) / 4);
      set_RSSI_output();
      RSSI_sum = 0;
      RSSI_count = 0;
    }
}
/*
void rssi_chk(uint32_t timeUs)
{
  static uint32_t lastRSSITimeUs = 0;

//OLS3 = 1;
//OLS3 = !OLS3;

  // sample RSSI when packet is in the 'air'
  if ((numberOfLostPackets < 2) && // don't sample if we're currently losing packets
      (lastRSSITimeUs != lastPacketTimeUs) && // only sample once per packet cycle
      (timeUs - lastPacketTimeUs) > (getInterval(&bind_data) - 1500)) { // and do it at the 'right' time

// Before link is acquired this is occurring 23.5 milliseconds after each channel hop
    lastRSSITimeUs = lastPacketTimeUs;
    sample_rssi();
  }
//OLS3 = 0;
}
 */
/*
boolean link_chk(uint32_t timeUs)
{
	static uint32_t lastBeaconTimeMs;
	static uint32_t linkLossTimeMs;

	uint32_t timeMs = timeUs / 1000;
	boolean hop_req = 0;

   // 26 millisecond check
   if ((numberOfLostPackets < hopcount) &&
       ((timeUs - lastPacketTimeUs) > (getInterval(&bind_data) + 1000))) {

      // we are at least one millisecond beyond packet due time

      lastPacketTimeUs += getInterval(&bind_data);

      // we lost packet, hop to next channel
//OLS2 = 1;
      linkQuality <<= 1;
      hop_req = 1;
      if (numberOfLostPackets == 0) {
        linkLossTimeMs = timeMs;
        lastBeaconTimeMs = 0;
      }
      numberOfLostPackets++;
      hop_req = 1;
      Red_LED_ON;
      set_RSSI_output();

   // 150 millisecond check, or roughly six times the above period
    } else if ((numberOfLostPackets == hopcount) && ((timeUs - lastPacketTimeUs) > (getInterval(&bind_data) * hopcount))) {
      // hop slowly to allow resync with TX (hop out of sync with TX)
      linkQuality = 0;
      hop_req = 1;
      set_RSSI_output();
      lastPacketTimeUs = timeUs;
    }

    if (numberOfLostPackets) {
      if (rx_config.failsafeDelay && (!failsafeActive) && ((timeMs - linkLossTimeMs) > delayInMs(rx_config.failsafeDelay))) {
        failsafeActive = 1;
        lrsIn_setFailsafe(true);
//		udb_callback_radio_failsafe(true);
        failsafeApply();
        lastBeaconTimeMs = (timeMs + delayInMsLong(rx_config.beacon_deadtime)) | 1; //beacon activating...
      }

      if ((rx_config.beacon_frequency) && (lastBeaconTimeMs)) {
        if (((timeMs - lastBeaconTimeMs) < 0x80000000) && // last beacon is future during deadtime
             (timeMs - lastBeaconTimeMs) > (1000UL * rx_config.beacon_interval)) {
          beacon_send();
          init_rfm(0);   // go back to normal RX
          rx_reset(); // _link_chk()
          lastBeaconTimeMs = millis() | 1; // avoid 0 in time
        }
      }
    }
//OLS2 = 0;
  return hop_req;
}
 */
void channel_hop(void)
{
	static uint8_t RF_channel = 0;

    RF_channel++;
    if ((RF_channel == MAXHOPS) || (bind_data.hopchannel[RF_channel] == 0)) {
      RF_channel = 0;
    }
    rfmSetChannel(RF_channel);
}

//############ MAIN LOOP ##############
/*
void loop()
{
  static boolean linkAcquired = 0;
  boolean willhop = 0;
  uint32_t timeUs;

  if (!rfm_device_detected) {
      return;
  }

//  locked_module_chk();

  if (RF_Mode == RF_RECEIVE_SIG) {
//OLS2 = 0;
OLS1 = 1;
    receive_pkt();
    willhop = true;
    linkAcquired = true;
    numberOfLostPackets = 0;
    RF_Mode = RF_RECEIVE_REQ;
OLS1 = 0;
  }

  timeUs = micros();
  rssi_chk(timeUs);

  if (linkAcquired) {
    willhop |= link_chk(timeUs);
  } else {
    // Waiting for first packet, hop slowly
    if ((timeUs - lastPacketTimeUs) > (getInterval(&bind_data) * hopcount)) {
      lastPacketTimeUs = timeUs;
      willhop = true;  // for a hopcount of 6, this is 150 milliseconds
    }
  }

  if (willhop) {
    channel_hop();
    willhop = false;
  }
}
 */

static boolean linkAcquired = false;

void rx_poll(void)
{
	receive_pkt();
	linkAcquired = true;
	numberOfLostPackets = 0;
	RF_Mode = RF_RECEIVE_REQ;
}

boolean link_poll(void)
{
//  static uint32_t lastBeaconTimeMs;
//  static uint32_t linkLossTimeMs;
  static uint8_t failsafeDelay = 0;
  static uint8_t lost_cnt = 0;

  boolean hop_req = false;

//  uint32_t timeUs = micros();
//  uint32_t timeMs = timeUs / 1000;
//  rssi_chk(timeUs);

    // 26 millisecond check
    if (numberOfLostPackets < hopcount) {

      // we are at least one millisecond beyond packet due time

//      lastPacketTimeUs += getInterval(&bind_data);

      // we lost packet, hop to next channel
//OLS2 = 1;
      linkQuality <<= 1;
      hop_req = 1;
      if (numberOfLostPackets == 0) {
//        linkLossTimeMs = timeMs;
//        lastBeaconTimeMs = 0;
		failsafeDelay = 0;
      }
      numberOfLostPackets++;
      hop_req = 1;
      Red_LED_ON;
      set_RSSI_output();

   // 150 millisecond check, or roughly six times the above period
    } else if ((numberOfLostPackets == hopcount) && (lost_cnt++ > hopcount)) {
//        ((timeUs - lastPacketTimeUs) > (getInterval(&bind_data) * hopcount))) {
      // hop slowly to allow resync with TX (hop out of sync with TX)
      lost_cnt = 0;
      linkQuality = 0;
      hop_req = 1;
      set_RSSI_output();
//      lastPacketTimeUs = timeUs;
    }

rx_config.failsafeDelay = 50;

    if (numberOfLostPackets) {
//      if (rx_config.failsafeDelay && (!failsafeActive) && ((timeMs - linkLossTimeMs) > delayInMs(rx_config.failsafeDelay))) {
      if (rx_config.failsafeDelay && (!failsafeActive)) {
		if (failsafeDelay++ > rx_config.failsafeDelay) { // TODO: convert this to valid milliseconds
        	failsafeActive = 1;
        	lrsIn_setFailsafe(true);
		}
//        failsafeApply();
//        lastBeaconTimeMs = (timeMs + delayInMsLong(rx_config.beacon_deadtime)) | 1; //beacon activating...
      }
/*
      if ((rx_config.beacon_frequency) && (lastBeaconTimeMs)) {
        if (((timeMs - lastBeaconTimeMs) < 0x80000000) && // last beacon is future during deadtime
             (timeMs - lastBeaconTimeMs) > (1000UL * rx_config.beacon_interval)) {
          beacon_send();
          init_rfm(0);   // go back to normal RX
          rx_reset(); // _link_poll()
          lastBeaconTimeMs = millis() | 1; // avoid 0 in time
        }
      }
 */
    }
  return hop_req;
}


/*
during connection setup, just call channel_hop every 150ms ((getInterval(&bind_data) * hopcount)

once connection has been established, via first received packet interrupt:

	1/ wait for interrupt signalling receive packet or transmit packet completion
 	2/ sample rssi at rate ...
	3/ check link
	4/ hop when required
 */
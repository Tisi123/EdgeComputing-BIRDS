import time
import machine
from machine import Pin, RTC, deepsleep
import network
import socket
import ubinascii
from SX1276 import Transceiver
from LoRaWAN import LoRaWAN
import EU868

try:
    import ujson as json
except ImportError:
    import json

# WiFi Configuration
WIFI_SSID = "BirdhouseNet"
WIFI_PASSWORD = "bird1234"
SERVER_IP = "192.168.4.1"
SERVER_PORT = 80

# LoRaWAN Configuration
APP_EUI = ubinascii.unhexlify("0000000000000004")
DEV_EUI = ubinascii.unhexlify("70B3D57ED0075E42")
APP_KEY = ubinascii.unhexlify("8EE73EF97C1D61662E14EAF1A9A91543")

# Wake-up schedule with mode assignment (UTC/GMT)
# Format: (hour, minute, mode)
# mode: "morning" = time sync only, "evening" = time sync + bird data collection
WAKE_SCHEDULE = [
    (7, 56, "morning"),   # 7:56 AM - Morning sync only
    (16, 56, "evening"),  # 4:56 PM - Evening sync + bird data
    #(20, 0, "evening"),   # 8:00 PM - Evening sync + bird data
]

# Minimum sleep duration (seconds)
MIN_SLEEP_DURATION_S = 60

# Species ID for time sync message
TIME_SYNC_SPECIES_ID = 254
SCHEDULE_TZ_OFFSET_SECONDS = 3600  # GMT+1


def connect_wifi(ssid, password, timeout_s=15):
    """Connect to WiFi network"""
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    
    if wlan.isconnected():
        return wlan
    
    print(f"Connecting to WiFi: {ssid}")
    wlan.connect(ssid, password)
    
    start = time.time()
    while not wlan.isconnected():
        if time.time() - start > timeout_s:
            raise RuntimeError("Wi-Fi connection timeout")
        time.sleep(0.2)
    
    print("WiFi connected:", wlan.ifconfig())
    return wlan


def fetch_server_time():
    """Fetch time from ESP32 server"""
    addr = socket.getaddrinfo(SERVER_IP, SERVER_PORT)[0][-1]
    s = socket.socket()
    s.settimeout(5.0)
    
    try:
        s.connect(addr)
        request = "GET /sync_devices_request HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n".format(SERVER_IP)
        s.send(request.encode())
        
        response = b""
        while True:
            try:
                data = s.recv(512)
                if not data:
                    break
                response += data
            except OSError:
                break
        
        response_str = response.decode('utf-8')
        
        if "\r\n\r\n" in response_str:
            body = response_str.split("\r\n\r\n", 1)[1]
            return json.loads(body)
        
        return None
        
    finally:
        s.close()


def sync_device_time():
    """
    Synchronize device time with server.
    Returns: (success: bool, unix_timestamp: int)
    """
    try:
        print("Syncing time with server...")
        time_data = fetch_server_time()
        
        if not time_data:
            print("ERROR: Could not fetch server time")
            return False, 0
        
        unix_time = time_data.get('unix_time')
        
        if unix_time is None:
            print("ERROR: No unix_time in response")
            return False, 0
        
        # Set RTC
        tm = time.gmtime(unix_time)
        rtc = RTC()
        rtc.datetime((tm[0], tm[1], tm[2], tm[6], tm[3], tm[4], tm[5], 0))
        
        print(f"✓ Time synced: {time_data.get('device_time')}")
        print(f"  Unix timestamp: {unix_time}")
        
        return True, unix_time
        
    except Exception as e:
        print(f"Time sync error: {e}")
        return False, 0


def encode_time_sync_message(timestamp):
    """
    Encode time sync confirmation message in same format as bird data.
    
    Format (6 bytes):
    - Species ID (1 byte): 254 for time sync
    - Confidence (1 byte): 100 (1.0 = 100%)
    - Timestamp (4 bytes): Current unix timestamp
    
    Returns: bytes object
    """
    payload = bytearray()
    payload.append(TIME_SYNC_SPECIES_ID)  # Species ID for "time sync"
    payload.append(100)  # Confidence 1.0 = 100%
    payload.extend(timestamp.to_bytes(4, 'big'))
    
    return bytes(payload)


def init_lora():
    """Initialize LoRa radio and join TTN"""
    try:
        print("\nInitializing LoRa...")
        spi = machine.SoftSPI(baudrate=400000, sck=Pin(14), mosi=Pin(47), miso=Pin(21))
        cs = Pin(48, Pin.OUT, value=1)
        rst = Pin(45, Pin.OUT, value=1)
        dio1 = Pin(46, Pin.IN)
        radio = Transceiver(spi, cs, rst, dio1)
        lw = LoRaWAN(radio, EU868.FREQS)
        
        print("Joining TTN...")
        lw.join_otaa(APP_KEY, APP_EUI, DEV_EUI, retries=8, sf=12)
        print("✓ Joined TTN!")
        
        return lw
        
    except Exception as e:
        print(f"LoRa init error: {e}")
        return None


def send_time_sync_confirmation(lw, timestamp):
    """Send time sync confirmation to TTN"""
    try:
        payload = encode_time_sync_message(timestamp)
        hex_payload = ''.join(['{:02x}'.format(b) for b in payload])
        
        print(f"\nSending time sync confirmation to TTN...")
        print(f"  Payload ({len(payload)} bytes): {hex_payload}")
        
        lw.send(payload, sf=12, ack=False)
        print(f"✓ Time sync confirmation sent!")
        
        return True
        
    except Exception as e:
        print(f"TTN send error: {e}")
        return False


def run_bird_data_collection(lw):
    """
    Run the bird data collection and transmission script.
    Imports and executes the bird data script.
    """
    try:
        print("\n" + "="*40)
        print("  Starting Bird Data Collection")
        print("="*40)
        
        # Import the bird data collection functions
        import bird_sender
        
        # Fetch all bird data from server
        bird_list = bird_sender.fetch_all_bird_data()
        
        if bird_list:
            print(f"\nFetched {len(bird_list)} bird detection(s)")
            
            # Send all data in batches (reuse existing LoRa connection)
            bird_sender.send_all_batches(lw, bird_list, delay_between_sends=2)
        else:
            print("No bird data to collect")
        
        print("\n✓ Bird data collection complete!")
        return True
        
    except Exception as e:
        print(f"Bird data collection error: {e}")
        import sys
        sys.print_exception(e)
        return False


def get_next_wake_time(current_hour, current_minute):
    """
    Calculate seconds until next scheduled wake time.
    
    Returns: (seconds_to_sleep, next_wake_hour, next_wake_minute, next_mode)
    """
    current_time_minutes = current_hour * 60 + current_minute
    
    # Find next wake time
    next_wake = None
    for wake_hour, wake_minute, wake_mode in WAKE_SCHEDULE:
        wake_time_minutes = wake_hour * 60 + wake_minute
        
        # If this wake time is later today
        if wake_time_minutes > current_time_minutes:
            next_wake = (wake_hour, wake_minute, wake_mode, wake_time_minutes - current_time_minutes)
            break
    
    # If no wake time found today, use first wake time tomorrow
    if next_wake is None:
        wake_hour, wake_minute, wake_mode = WAKE_SCHEDULE[0]
        # Minutes until midnight + minutes into tomorrow
        minutes_until_midnight = (24 * 60) - current_time_minutes
        minutes_into_tomorrow = wake_hour * 60 + wake_minute
        total_minutes = minutes_until_midnight + minutes_into_tomorrow
        next_wake = (wake_hour, wake_minute, wake_mode, total_minutes)
    
    sleep_seconds = next_wake[3] * 60  # Convert minutes to seconds
    
    return sleep_seconds, next_wake[0], next_wake[1], next_wake[2]


def determine_current_mode(current_hour, current_minute):
    """
    Determine which mode we should be in based on current time.
    Finds the most recent scheduled wake time.
    
    Returns: "morning" or "evening"
    """
    current_time_minutes = current_hour * 60 + current_minute
    
    # Find the most recent wake time that has passed
    most_recent_mode = None
    for wake_hour, wake_minute, wake_mode in reversed(WAKE_SCHEDULE):
        wake_time_minutes = wake_hour * 60 + wake_minute
        
        if wake_time_minutes <= current_time_minutes:
            most_recent_mode = wake_mode
            break
    
    # If no wake time has passed today, use the last one from "yesterday"
    if most_recent_mode is None:
        most_recent_mode = WAKE_SCHEDULE[-1][2]
    
    return most_recent_mode


def enter_deep_sleep(sleep_seconds):
    """Enter deep sleep for specified duration"""
    sleep_ms = sleep_seconds * 1000
    
    print(f"\n{'='*40}")
    print(f"Entering deep sleep for {sleep_seconds} seconds")
    print(f"({sleep_seconds/60:.1f} minutes / {sleep_seconds/3600:.1f} hours)")
    print(f"{'='*40}\n")
    
    # Small delay to allow print to complete
    time.sleep(0.5)
    
    # Enter deep sleep
    deepsleep(sleep_ms)

def main():
    """Main execution - Smart Wake Manager"""
    print("\n" + "="*40)
    print("  Smart Wake Manager")
    print("="*40)
        
    # Connect to WiFi
    try:
        wlan = connect_wifi(WIFI_SSID, WIFI_PASSWORD)
    except Exception as e:
        print(f"WiFi connection failed: {e}")
        # Sleep for 5 minutes and try again
        enter_deep_sleep(300)
        return
    
    # Sync time with server
    sync_success, synced_timestamp = sync_device_time()
    
    if not sync_success:
        print("WARNING: Time sync failed")
        synced_timestamp = int(time.time())
    
    # Get current schedule time (GMT+1) and determine mode
    current_time = time.gmtime(time.time() + SCHEDULE_TZ_OFFSET_SECONDS)
    current_hour = current_time[3]
    current_minute = current_time[4]
    
    current_mode = determine_current_mode(current_hour, current_minute)
    
    print(f"\nCurrent GMT+1 time: {current_hour:02d}:{current_minute:02d}")
    print(f"Current mode: {current_mode.upper()}")
    
    # Initialize LoRa (needed for both modes)
    lw = init_lora()
    
    if not lw:
        print("ERROR: Could not initialize LoRa")
        # Sleep and retry
        wlan.disconnect()
        wlan.active(False)
        enter_deep_sleep(300)
        return
    
    # Execute mode-specific actions
    upload_success = True
    if current_mode == "morning":
        print("\n--- MORNING MODE: Time Sync Only ---")
        upload_success = send_time_sync_confirmation(lw, synced_timestamp)
        
    elif current_mode == "evening":
        print("\n--- EVENING MODE: Time Sync + Bird Data ---")
        sync_msg_ok = send_time_sync_confirmation(lw, synced_timestamp)
        
        # Small delay between time sync and bird data
        time.sleep(2)
        
        # Run bird data collection
        bird_upload_ok = run_bird_data_collection(lw)
        upload_success = sync_msg_ok and bird_upload_ok
    
    # Re-read schedule time AFTER upload work is complete.
    # This avoids drift when TTN transmissions take noticeable time.
    post_work_time = time.gmtime(time.time() + SCHEDULE_TZ_OFFSET_SECONDS)
    post_work_hour = post_work_time[3]
    post_work_minute = post_work_time[4]

    print(f"\nPost-upload GMT+1 time: {post_work_hour:02d}:{post_work_minute:02d}")

    # Calculate next wake time from post-upload time.
    sleep_seconds, next_hour, next_minute, next_mode = get_next_wake_time(post_work_hour, post_work_minute)
    
    # Ensure minimum sleep duration
    if sleep_seconds < MIN_SLEEP_DURATION_S:
        print(f"Sleep duration too short ({sleep_seconds}s), using minimum: {MIN_SLEEP_DURATION_S}s")
        sleep_seconds = MIN_SLEEP_DURATION_S

    if not upload_success:
        # Retry sooner if TTN upload path failed.
        retry_seconds = 10 * 60
        print(f"\nUpload not fully successful, overriding next wake to retry in {retry_seconds}s")
        sleep_seconds = retry_seconds
    
    print(f"\n--- Sleep Schedule ---")
    print(f"Next wake time: {next_hour:02d}:{next_minute:02d}")
    print(f"Next mode: {next_mode.upper()}")
    
    # Disconnect WiFi to save power
    wlan.disconnect()
    wlan.active(False)
    print("WiFi disconnected")
    
    # Enter deep sleep
    enter_deep_sleep(sleep_seconds)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"\nFATAL ERROR: {e}")
        import sys
        sys.print_exception(e)
        
        # Sleep for 10 minutes before retry
        print("\nRetrying in 10 minutes...")
        time.sleep(1)
        enter_deep_sleep(600)

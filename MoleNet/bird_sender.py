import time
import network
import ubinascii, machine
from machine import Pin
from SX1276 import Transceiver
from LoRaWAN import LoRaWAN
import EU868
import socket
try:
    import ujson as json
except ImportError:
    import json

# WiFi Config
WIFI_SSID = "BirdhouseNet"
WIFI_PASSWORD = "bird1234"
SERVER_IP = "192.168.4.1"
SERVER_PORT = 80
WIFI_TIMEOUT_S = 15

# LoRa Config
MAX_PAYLOAD_SIZE = 51    # TTN max payload for SF12 is 51 bytes (conservative)
BYTES_PER_BIRD = 6       # Species(1) + Confidence(1) + Timestamp(4)


def connect_wifi(ssid, password, timeout_s=15):
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    
    if wlan.isconnected():
        return wlan
    
    wlan.connect(ssid, password)
    
    start = time.time()
    while not wlan.isconnected():
        if time.time() - start > timeout_s:
            raise RuntimeError("Wi-Fi connection timeout")
        time.sleep(0.2)
    
    return wlan


def http_request(method, path, timeout_s=5.0):
    """Small HTTP helper for MicroPython sockets."""
    addr = socket.getaddrinfo(SERVER_IP, SERVER_PORT)[0][-1]
    s = socket.socket()
    s.settimeout(timeout_s)

    try:
        s.connect(addr)
        request = "{} {} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n".format(
            method, path, SERVER_IP
        )
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

        response_str = response.decode("utf-8")
        if "\r\n\r\n" not in response_str:
            return 0, ""

        head, body = response_str.split("\r\n\r\n", 1)
        status_line = head.split("\r\n", 1)[0]
        status_code = 0
        parts = status_line.split(" ")
        if len(parts) >= 2:
            try:
                status_code = int(parts[1])
            except Exception:
                status_code = 0
        return status_code, body
    finally:
        s.close()


def parse_jsonl(text):
    """Parse NDJSON/JSONL body into list of dicts."""
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
            if isinstance(obj, dict):
                out.append(obj)
        except Exception as e:
            print("Skipping invalid JSONL line:", e)
    return out


def fetch_all_bird_data():
    """
    Fetch all bird data from ESP32-S3-EYE SD card via WiFi.
    Returns a list of bird data entries.
    """
    try:
        # 1) Ask Eye to flush RAM buffer to SD first.
        flush_code, flush_body = http_request("POST", "/flush_to_sd")
        print("Flush response:", flush_code, flush_body)

        # 2) Read full history from SD card (JSONL/NDJSON).
        status_code, body = http_request("GET", "/get_bird_history")
        print("History response code:", status_code)

        if status_code == 200:
            try:
                data = json.loads(body)
                if isinstance(data, dict):
                    return [data]
                elif isinstance(data, list):
                    return data
            except Exception as e:
                print("History body is not JSON array/object, trying JSONL:", e)
                return parse_jsonl(body)

        if status_code in (204, 404):
            print("No SD history available")
            return []

        # 3) Fallback to RAM endpoint for compatibility.
        print("Falling back to RAM endpoint /get_bird_data_request")
        status_code, body = http_request("GET", "/get_bird_data_request")
        if status_code in (200, 204):
            try:
                data = json.loads(body)
                if isinstance(data, dict):
                    return [data]
                if isinstance(data, list):
                    return data
            except Exception:
                return parse_jsonl(body)
        return []
    except Exception as e:
        print("fetch_all_bird_data error:", e)
        return []


def encode_bird_data(bird_data):
    """
    Encode a single bird detection into compact binary format.
    
    Format (6 bytes total):
    - Species ID (1 byte): Lookup table for common species
    - Confidence (1 byte): 0-100 scaled from 0.0-1.0
    - Timestamp (4 bytes): Unix timestamp (seconds)
    
    Returns: bytes object
    """
    SPECIES_MAP = {
        "great tit": 1,
        "blue tit": 2,
        "robin": 3,
        "blackbird": 4,
        "common blackbird": 4,
        "sparrow": 5,
        "woodpecker": 6,
        "finch": 7,
        "starling": 8,
    }
    
    species = bird_data.get("species", "unknown").lower()
    species_id = SPECIES_MAP.get(species, 255)
    
    confidence = int(bird_data.get("confidence", 0) * 100)  # Scale to 0-100
    confidence = max(0, min(255, confidence))  # Clamp to byte range
    
    # Handle timestamp - could be int or string
    timestamp = bird_data.get('timestamp', 0)
    if isinstance(timestamp, str):
        # If it's ISO format, just use 0 for now (or parse it properly)
        timestamp = 0
    timestamp = int(timestamp)
    
    # Pack data: 1 byte species + 1 byte confidence + 4 bytes timestamp
    payload = bytearray()
    payload.append(species_id)
    payload.append(confidence)
    payload.extend(timestamp.to_bytes(4, 'big'))
    
    return bytes(payload)


def create_batches(bird_list):
    """
    Split bird list into batches that fit within TTN payload size limit.
    
    Returns: List of batches, where each batch is a list of bird data dicts
    """
    batches = []
    current_batch = []
    current_size = 0
    
    for bird in bird_list:
        # Each bird takes BYTES_PER_BIRD bytes
        if current_size + BYTES_PER_BIRD > MAX_PAYLOAD_SIZE:
            # Current batch is full, start a new one
            if current_batch:
                batches.append(current_batch)
            current_batch = [bird]
            current_size = BYTES_PER_BIRD
        else:
            current_batch.append(bird)
            current_size += BYTES_PER_BIRD
    
    # Don't forget the last batch
    if current_batch:
        batches.append(current_batch)
    
    return batches


def encode_batch(bird_batch):
    """
    Encode a batch of birds into a single payload.
    
    Returns: bytes object
    """
    payload = bytearray()
    
    for bird in bird_batch:
        bird_bytes = encode_bird_data(bird)
        payload.extend(bird_bytes)
    
    return bytes(payload)


def send_to_ttn(lw, payload, batch_num=None):
    """Send payload to TTN via LoRaWAN"""
    try:
        lw.send(payload, sf=12, ack=False)
        if batch_num is not None:
            print(f"Batch {batch_num}: Sent {len(payload)} bytes to TTN")
        else:
            print(f"Sent {len(payload)} bytes to TTN")
        return True
    except Exception as e:
        print(f"TTN send error: {e}")
        return False


def send_all_batches(lw, bird_list, delay_between_sends=2):
    """
    Process all birds, create batches, and send them to TTN.
    Keeps TTN connection alive until all data is sent.
    
    Args:
        lw: LoRaWAN object
        bird_list: List of bird data dictionaries
        delay_between_sends: Delay in seconds between batch transmissions
    """
    if not bird_list:
        print("No bird data to send")
        return
    
    # Create batches
    batches = create_batches(bird_list)
    print(f"\nTotal birds: {len(bird_list)}")
    print(f"Total batches: {len(batches)}")
    print(f"Birds per batch: {[len(b) for b in batches]}")
    
    # Send each batch
    successful_sends = 0
    for i, batch in enumerate(batches, 1):
        print(f"\n--- Batch {i}/{len(batches)} ---")
        
        # Show what's in this batch
        for bird in batch:
            print(f"  - {bird.get('species', 'unknown')}: {bird.get('confidence', 0):.2f}")
        
        # Encode the batch
        payload = encode_batch(batch)
        hex_payload = ''.join(['{:02x}'.format(b) for b in payload])
        print(f"Payload ({len(payload)} bytes): {hex_payload}")
        
        # Send to TTN
        if send_to_ttn(lw, payload, i):
            successful_sends += 1
        
        # Wait between sends (except after the last one)
        if i < len(batches):
            print(f"Waiting {delay_between_sends}s before next batch...")
            time.sleep(delay_between_sends)
    
    print(f"\n=== Summary ===")
    print(f"Successfully sent {successful_sends}/{len(batches)} batches")


def main():
    # Initialize LoRa
    spi = machine.SoftSPI(baudrate=400000, sck=Pin(14), mosi=Pin(47), miso=Pin(21))
    cs = Pin(48, Pin.OUT, value=1)
    rst = Pin(45, Pin.OUT, value=1)
    dio1 = Pin(46, Pin.IN)
    radio = Transceiver(spi, cs, rst, dio1)
    lw = LoRaWAN(radio, EU868.FREQS)
    
    # LoRaWAN Keys
    APP_EUI = ubinascii.unhexlify("0000000000000004")
    DEV_EUI = ubinascii.unhexlify("70B3D57ED00738E1")
    APP_KEY = ubinascii.unhexlify("F0A270EF5DDCE7C7ECCC3264DBC0F3A1")
    
    # Join TTN
    print("Joining TTN...")
    lw.join_otaa(APP_KEY, APP_EUI, DEV_EUI, retries=8, sf=12)
    print("Joined TTN!")
    
    # Connect to WiFi
    wlan = connect_wifi(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_S)
    print("Connected to WiFi:", wlan.ifconfig())
    
    # Fetch all bird data
    try:
        bird_list = fetch_all_bird_data()
        
        if bird_list:
            print(f"\nFetched {len(bird_list)} bird detection(s)")
            
            # Send all data in batches
            send_all_batches(lw, bird_list, delay_between_sends=2)
        else:
            print("No bird data received")
            
    except Exception as e:
        print(f"Error: {e}")
        import sys
        sys.print_exception(e)
    
    print("\nScript complete!")
    pass


if __name__ == "__main__":
    main()

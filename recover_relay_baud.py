"""
Waveshare 16CH Modbus RTU Relay Board - Baud Rate Recovery Script
This script forces the relay board back to 115200 baud by:
1. Trying at 9600 baud (most common default)
2. Sending FC 0x06 to register 0x2000 with value 5 (115200)
3. Then switching to 115200 to verify
"""

import serial
import time
import sys

def calculate_crc(data):
    """Calculate Modbus RTU CRC-16"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc >>= 1
                crc ^= 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF

def build_fc06_frame(slave_addr, register, value):
    """Build Modbus FC 0x06 (Write Single Register) frame"""
    frame = bytes([slave_addr, 0x06, (register >> 8) & 0xFF, register & 0xFF, 
                   (value >> 8) & 0xFF, value & 0xFF])
    crc = calculate_crc(frame)
    frame += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
    return frame

def test_relay_at_baud(port, baud_rate, timeout=2):
    """Test if relay responds at given baud rate"""
    try:
        ser = serial.Serial(port, baud_rate, timeout=timeout)
        # FC 0x03 - Read Holding Registers (try to read register 0x2000)
        frame = bytes([0x01, 0x03, 0x20, 0x00, 0x00, 0x01])
        crc = calculate_crc(frame)
        frame += bytes([crc & 0xFF, (crc >> 8) & 0xFF])
        
        ser.write(frame)
        response = ser.read(256)
        ser.close()
        
        if len(response) > 0:
            print(f"  Response at {baud_rate}: {response.hex()}")
            return True
        else:
            print(f"  No response at {baud_rate}")
            return False
    except Exception as e:
        print(f"  Error at {baud_rate}: {e}")
        return False

def set_baud_to_115200(port, current_baud):
    """Send FC 0x06 to change relay baud to 115200"""
    print(f"\nAttempting to set baud rate to 115200 (current: {current_baud})...")
    
    try:
        ser = serial.Serial(port, current_baud, timeout=2)
        
        # FC 0x06 - Write Single Register
        # Slave: 0x01, Function: 0x06, Register: 0x2000, Value: 5 (115200)
        frame = build_fc06_frame(0x01, 0x2000, 5)
        print(f"  Sending frame: {frame.hex()}")
        
        ser.write(frame)
        time.sleep(0.1)
        response = ser.read(256)
        
        if len(response) > 0:
            print(f"  Response: {response.hex()}")
            if response[:2] == frame[:2]:
                print("  SUCCESS: Relay acknowledged baud change!")
                ser.close()
                return True
            else:
                print(f"  Unexpected response: {response.hex()}")
        else:
            print("  No response (but command may have been accepted)")
        
        ser.close()
        return True
    except Exception as e:
        print(f"  Error: {e}")
        return False

def main():
    port = "COM20"  # Change this to your COM port
    
    print("=" * 60)
    print("Waveshare 16CH Modbus Relay - Baud Rate Recovery")
    print("=" * 60)
    
    # Common baud rates to try
    baud_rates = [9600, 19200, 38400, 57600, 115200]
    
    # Step 1: Try to detect current baud rate
    print("\n[Step 1] Detecting relay baud rate...")
    detected_baud = None
    
    for baud in baud_rates:
        print(f"  Testing {baud} baud...")
        if test_relay_at_baud(port, baud):
            detected_baud = baud
            print(f"  >>> Relay detected at {baud} baud!")
            break
        time.sleep(0.5)
    
    if detected_baud is None:
        print("\n  WARNING: Could not detect relay at any standard baud rate")
        print("  The relay might be at a non-standard baud or disconnected")
        print("\n  Trying to force 115200 baud at 9600 (most common default)...")
        detected_baud = 9600
    
    # Step 2: Set baud to 115200
    print(f"\n[Step 2] Setting relay baud to 115200...")
    if set_baud_to_115200(port, detected_baud):
        print("\n  Waiting 500ms for relay to apply new baud rate...")
        time.sleep(0.5)
        
        # Step 3: Verify at 115200
        print("\n[Step 3] Verifying relay at 115200 baud...")
        if test_relay_at_baud(port, 115200):
            print("\n" + "=" * 60)
            print("SUCCESS! Relay is now at 115200 baud and responding!")
            print("=" * 60)
            print("\nNext steps:")
            print("1. Set your HMI to 115200 baud")
            print("2. The relay should now show as ONLINE")
            return 0
        else:
            print("\n  Verification failed - relay may need power cycle")
            print("  Please power cycle the relay board and try again")
            return 1
    else:
        print("\n  Failed to set baud rate")
        return 1

if __name__ == "__main__":
    sys.exit(main())

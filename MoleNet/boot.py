# boot.py - Runs automatically on device startup

import time_sync

print("\n" + "="*40)
print("  Device Startup - Initializing")
print("="*40)

# Perform time synchronization
try:
    time_sync.main()
except Exception as e:
    print(f"Startup error: {e}")

print("\nStartup sequence complete!\n")

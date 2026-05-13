#!/bin/bash
# Single command to fix the VPS daemon - copy this entire block into VNC terminal

cd /tmp

# Download all parts
echo "Downloading binary parts..."
for part in aa ab ac ad ae af ag ah ai aj ak al am an ao ap aq ar; do
  echo "Downloading part $part..."
  wget -q http://75.24.111.197:8000/dinerod-part-$part
  if [ $? -ne 0 ]; then
    echo "Failed to download part $part"
    exit 1
  fi
done

# Combine parts
echo "Combining parts..."
cat dinerod-part-* > dinerod-linux2
chmod +x dinerod-linux2

# Verify file size (should be ~18MB)
ls -la dinerod-linux2

# Replace daemon
echo "Stopping daemon..."
systemctl stop dinerod

echo "Replacing binary..."
cp dinerod-linux2 /usr/local/bin/dinerod
chmod +x /usr/local/bin/dinerod

echo "Starting daemon..."
systemctl start dinerod

echo "Checking status..."
systemctl status dinerod

echo "Testing RPC..."
curl -s --http1.0 -H 'Expect:' \
  --user "$(cat /var/lib/dinero/.cookie)" \
  -H 'content-type: application/json' \
  --data-binary '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' \
  http://127.0.0.1:8332/

echo "Done! Canada seed should be working now!"

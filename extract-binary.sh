#!/bin/bash
# Extract the compiled dinerod binary from Docker image

set -e  # Exit on any error

echo "📦 Extracting dinerod binary from Docker image..."
echo ""

# Create temporary container
echo "Creating temporary container..."
CONTAINER_ID=$(docker create dinerod:latest)

# Copy binary from container
echo "Copying binary..."
docker cp $CONTAINER_ID:/usr/local/bin/dinerod ./dinerod

# Remove temporary container
echo "Cleaning up..."
docker rm $CONTAINER_ID

# Make binary executable
chmod +x dinerod

# Show info
echo ""
echo "✅ Binary extracted successfully!"
echo ""
ls -lh dinerod
file dinerod
echo ""
echo "📤 Deploy to server:"
echo "scp -i ~/.ssh/dinerola.key dinerod root@172.93.160.131:/root/dinero-coin/build/"

#!/bin/bash

# Start the backend in the background
./build/asan/enactus_backend &
BACKEND_PID=$!

# Wait for it to boot
sleep 2

echo "Creating an application..."
curl -X POST http://127.0.0.1:8080/api/applications \
     -H "Content-Type: application/json" \
     -d '{"name": "Alice Wonderland", "team": "Presentation", "reason": "I am great at speaking."}'

echo ""
echo "Fetching applications..."
curl http://127.0.0.1:8080/api/applications_list

echo ""
echo "Fetching teams..."
curl http://127.0.0.1:8080/api/teams
echo ""

# Kill the backend
kill $BACKEND_PID

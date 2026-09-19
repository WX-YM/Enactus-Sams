#!/bin/bash
./build/asan/enactus_backend &
BACKEND_PID=$!

cd admin
npm run dev &
FRONTEND_PID=$!

wait

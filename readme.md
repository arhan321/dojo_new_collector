# RMS Yaskawa — C++ YMConnect Collector (Read-only)

This is **Phase 2 prototype code**, one YRC1000 / SP210 controller per collector process.
It is not production certified and has not been compiled against the exact YMConnect library in your WSL folder.

## Replace the existing files

Copy `HelloYMConnect.cpp` and `CMakeLists.txt` into `~/ymconnect/HelloYMConnect/`.
Keep your existing official `YMConnect.h` and `libYMConnect.so` files in that folder.

```bash
cd ~/ymconnect/HelloYMConnect
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
./build/RMSCollector --ip 192.168.255.1 --robot-id YASKAWA_SP210_DOJO
```

Data is appended daily to `./data/rms_collector_<robot-id>_YYYY-MM-DD.jsonl` (UTC date).
No PostgreSQL, Laravel, or networking server is required yet.

## Runtime switches

```bash
./build/RMSCollector --help
./build/RMSCollector --no-screen --output ./data --poll-ms 1000 --alarm-ms 5000 --operating-ms 60000
```

Press `Ctrl+C` to stop (SIGINT). `SIGTERM` is also handled. Note that build/compile does not launch the program. For unattended Mini PC startup, a service manager can launch the binary later.

## Behavior and limitations

- Uses only `ReadPositionData`, `ReadTorqueData`, `GetActiveAlarms`, `ReadOperatingTimes`.
- Data acquisition is sequential and NOT deterministic hard real-time (nominal ~1 sec sampling, plus SDK request latency).
- Each feature has an `ok`, `status_code`, `message`, `sampled_at_utc`. A failed read outputs `null` rather than stale values.
- Servo operating time and alarm text are recorded exactly as formatted by your SDK (`sdk_raw`) to avoid inventing units or parsing unverified fields.
- Torque value unit is unknown until confirmed against your YMConnect version, so marked `sdk_raw`.
- Failure of position and torque **together** for three polling cycles triggers a reconnect. This is a heuristic, not a true heartbeat.
- An SDK operation that hangs may stall this single process. Production deployment requires supervision/timeouts tested with the SDK and a safe polling load.
- Use a unique `--robot-id` for each process; daily JSONL files are separated per robot. Multiple processes may monitor separate robots, but this is not a validated multi-robot scheduler.
- No alarm history, controller state, job monitoring, I/O, predictive maintenance, or multi-robot scheduling is claimed for this first iteration.
- IMPORTANT: Confirm sampling frequency and network access with OT / maintenance engineers before live production deployment.



```
cara running project collector nya c++ nya  : 

{sesuaikan yang bagian build-simple dengan project kita}
yang pertama running ini : 
cmake -S . -B build-simple \
  -DCMAKE_BUILD_TYPE=Release \
  -DRMS_ENABLE_YMCONNECT=ON \
  -DYMCONNECT_INCLUDE="$PWD" \
  -DYMCONNECT_LIBRARY="$PWD/libYMConnect.so"

cmake --build build-simple -j2
```
```
kedua run yang ini : 
./build-simple/rms_collector \
  --config config/collector.dojo.conf \
  --real
  {sesuaikan yang build simple dengan project kita ya jangan sampe salah}

```
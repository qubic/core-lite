# Qubic Core Lite

Run a Qubic network node in Docker with automatic snapshot downloads, health monitoring, and self-healing restarts.

## Quick Start

```bash
docker run -d --name core-lite \
  -p 21841:21841 \
  -p 41841:41841 \
  -v qubic-data:/qubic \
  qubiccore/lite:latest
```

Or with Docker Compose:

```yaml
services:
  core-lite:
    image: qubiccore/lite:latest
    restart: unless-stopped
    stop_grace_period: 120s
    ports:
      - "21841:21841"   # P2P
      - "41841:41841"   # HTTP API
    volumes:
      - qubic-data:/qubic
    environment:
      QUBIC_LOG_LEVEL: "INFO"

  watchtower:
    image: containrrr/watchtower
    restart: unless-stopped
    volumes:
      - /var/run/docker.sock:/var/run/docker.sock
    environment:
      WATCHTOWER_POLL_INTERVAL: 60
      WATCHTOWER_TIMEOUT: 120s
    command: core-lite

volumes:
  qubic-data:
```

## What It Does

On startup the orchestrator:

1. **Discovers the current epoch** from the Qubic network API
2. **Downloads the latest snapshot** (or epoch files) if local state is missing or outdated
3. **Starts the Qubic node** with the configured parameters
4. **Monitors health** via a built-in watchdog that detects crashes, stuck ticks, and consensus misalignment
5. **Automatically restarts** the node when problems are detected

## Ports

| Port | Protocol | Description |
|------|----------|-------------|
| 21841 | TCP | P2P (mainnet) |
| 31841 | TCP | P2P (testnet) |
| 41841 | HTTP | Qubic node API |
| 8080 | HTTP | Management API (localhost only by default) |

## Environment Variables

### General

| Variable | Default | Description |
|----------|---------|-------------|
| `QUBIC_PEERS` | `""` | Comma-separated peer IPs (Autopopulated by default) |
| `QUBIC_SECURITY_TICK` | `32` | Skip contract state verification interval |
| `QUBIC_LOG_LEVEL` | `INFO` | Log level (DEBUG, INFO, WARNING, ERROR) |
| `QUBIC_LOG_FORMAT` | `json` | Log format (`json` or `text`) |
| `QUBIC_FORCE_DOWNLOAD` | `false` | Force snapshot re-download on startup |
| `QUBIC_THREADS` | *(auto)* | Number of processing threads |
| `QUBIC_SOLUTION_THREADS` | *(auto)* | Number of mining threads |
| `QUBIC_BINARY_VARIANT` | `auto` | Node build to run: `auto` (AVX-512 when the CPU supports it, else AVX2), `avx512` or `avx2` |

### Operator (optional)

| Variable | Default | Description |
|----------|---------|-------------|
| `QUBIC_OPERATOR_SEED` | `""` | Operator seed for node management |
| `QUBIC_OPERATOR` | `""` | Operator public identity |
| `QUBIC_HTTP_PASSCODE` | `""` | Passcode for the HTTP API |
| `QUBIC_SEEDS` | `""` | Comma-separated computor seeds |

### Watchdog

| Variable | Default | Description |
|----------|---------|-------------|
| `QUBIC_WATCHDOG__ENABLED` | `true` | Enable automatic health monitoring |
| `QUBIC_WATCHDOG__MAX_RESTARTS` | `5` | Max auto-restarts before giving up |
| `QUBIC_WATCHDOG__STARTUP_GRACE_SECONDS` | `300` | Grace period before health checks start |
| `QUBIC_WATCHDOG__STUCK_THRESHOLD_SECONDS` | `300` | Seconds without tick progress before restart |
| `QUBIC_WATCHDOG__MISALIGNED_THRESHOLD_SECONDS` | `300` | Seconds misaligned on same tick before restart |
| `QUBIC_WATCHDOG__RESTART_COOLDOWN_SECONDS` | `600` | Minimum time between restarts |

### Local Snapshot Saver

| Variable | Default | Description |
|----------|---------|-------------|
| `QUBIC_LOCAL_SNAPSHOT__ENABLED` | `false` | Enable periodic local snapshot saves (like pressing F8) |
| `QUBIC_LOCAL_SNAPSHOT__INTERVAL_SECONDS` | `3600` | Interval between saves (default: 1 hour) |

### Alerting (optional)

| Variable | Default | Description |
|----------|---------|-------------|
| `QUBIC_ALERTING__ENABLED` | `false` | Enable webhook alerts |
| `QUBIC_ALERTING__WEBHOOK_URL` | `""` | Webhook URL (Slack, Discord, etc.) |
| `QUBIC_ALERTING__RATE_LIMIT_SECONDS` | `300` | Minimum interval between duplicate alerts |

## Container Commands

The built-in `orchestrator-ctl` CLI lets you manage the node via `docker exec`:

```bash
# Check node status (epoch, tick, health, peers)
docker exec core-lite orchestrator-ctl status

# Quick health check
docker exec core-lite orchestrator-ctl health

# Restart the Qubic node binary
docker exec core-lite orchestrator-ctl restart

# Send a key command to the Qubic process
docker exec core-lite orchestrator-ctl send-key f4

# List all available key commands
docker exec core-lite orchestrator-ctl keys
```

### Available Key Commands

| Key | Description |
|-----|-------------|
| `f2` | Display node status |
| `f3` | Display mining race state |
| `f4` | Drop all connections (reconnect to peers) |
| `f5` | Issue new votes |
| `f6` | Save state to disk |
| `f7` | Force epoch switching |
| `f8` | Save tick storage snapshot |
| `f9` | Decrease latestCreatedTick by 1 |
| `f10` | Allow epoch transition with memory cleanup |
| `f11` | Toggle network mode (static/dynamic) |
| `f12` | Switch MAIN/aux mode |
| `p` | Cycle console logging verbosity |
| `f` | Force skip computer digest check |
| `s` | Force skip security tick check |
| `esc` | Shutdown node |

## Management API

The orchestrator also exposes a local HTTP API on port 8080 (localhost only by default).

```bash
# Health check
curl http://localhost:8080/health

# Detailed node status (epoch, tick, health, votes)
curl http://localhost:8080/status

# Manually restart the node
curl -X POST http://localhost:8080/restart

# Send a key command to the Qubic process
curl -X POST -d '{"key":"f4"}' http://localhost:8080/send-key

# List available key commands
curl http://localhost:8080/keys
```

## Watchdog

The built-in watchdog monitors the node and automatically restarts it when:

- **Crash detected** — the Qubic process exits unexpectedly
- **Stuck** — no tick progression for 5 minutes (configurable)
- **Misaligned** — consensus misalignment detected over consecutive polls

The watchdog respects a startup grace period (5 minutes), enforces cooldown between restarts, and stops after reaching the max restart limit to prevent infinite loops. All restart events are logged and optionally sent as webhook alerts.

## Alerting

Configure a webhook URL to receive alerts for node events:

- Node crashes, restarts, and recovery
- Stuck or misaligned state detection
- Epoch transitions
- Max restarts exceeded (requires manual intervention)

Alerts include node context (epoch, tick, health state, restart count) and are rate-limited to prevent flooding.

```bash
docker run -d --name core-lite \
  -p 21841:21841 \
  -v qubic-data:/qubic \
  -e QUBIC_ALERTING__ENABLED=true \
  -e QUBIC_ALERTING__WEBHOOK_URL="https://hooks.slack.com/services/..." \
  qubiccore/lite:latest
```

## Data Persistence

Mount a volume to `/qubic` to persist node state across container restarts:

```bash
-v qubic-data:/qubic
```

This directory contains epoch state files (spectrum, universe, contracts), snapshot data, and node configuration. Without a persistent volume, the node will re-download all state on every container restart.

## Health Check

The image includes a Docker health check that queries the node's HTTP API every 30 seconds with a 5-minute startup grace period. Use `docker inspect` or your orchestration platform to monitor container health.

## Requirements

- **CPU**: x86_64 with AVX2. The image holds an AVX-512 build and an AVX2 build of the node and starts the AVX-512 one when the CPU has AVX-512 F, CD, VL, BW, DQ, VBMI, VBMI2, VPOPCNTDQ and GFNI (e.g. AMD Zen 4 or newer), the AVX2 one otherwise.
- **RAM**: 64GB minimum for mainnet
- **Disk**: 50GB+ for state files
- **Network**: Stable connection with port 21841 accessible for P2P

## Links

- [Source Code](https://github.com/qubic/core-lite)

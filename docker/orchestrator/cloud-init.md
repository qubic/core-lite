# Cloud Init

Deploy a Qubic Core Lite node on any cloud provider with a single command using [cloud-init](https://cloud-init.io/).

The provided `cloud-init.yml` automatically:

- Updates system packages
- Installs Docker and Docker Compose
- Configures UFW firewall (SSH, P2P, HTTP API)
- Starts the Qubic node with Watchtower for automatic updates

## Requirements

- Ubuntu 24.04 VM
- At least 64 GB RAM
- 50 GB+ SSD
- x86_64 CPU with AVX2 (AVX-512 is used automatically when available)

## Usage

### AWS

```bash
aws ec2 run-instances \
  --image-id ami-0abcdef1234567890 \
  --instance-type m5.8xlarge \
  --user-data file://cloud-init.yml \
  --key-name my-key
```

### Hetzner

```bash
hcloud server create \
  --name core-lite \
  --type cx52 \
  --image ubuntu-24.04 \
  --user-data-from-file cloud-init.yml
```

### DigitalOcean

```bash
doctl compute droplet create core-lite \
  --size m-16vcpu-128gb \
  --image ubuntu-24-04-x64 \
  --user-data "$(cat cloud-init.yml)"
```

### Azure

```bash
az vm create \
  --name core-lite \
  --image Ubuntu2404 \
  --size Standard_D16s_v5 \
  --custom-data cloud-init.yml
```

### GCP

```bash
gcloud compute instances create core-lite \
  --image-family=ubuntu-2404-lts-amd64 \
  --image-project=ubuntu-os-cloud \
  --machine-type=n2-standard-16 \
  --metadata-from-file user-data=cloud-init.yml
```

## Post-Deploy

SSH into the VM and verify the node is running:

```bash
# Check container status
docker ps

# View node logs
docker logs -f core-lite

# Check node health
docker exec core-lite orchestrator-ctl status
```

## Configuration

The compose file is written to `/opt/qubic/docker-compose.yml`. To change environment variables after deployment:

```bash
# Edit the compose file
nano /opt/qubic/docker-compose.yml

# Apply changes
docker compose -f /opt/qubic/docker-compose.yml up -d
```

See the [Docker Hub page](https://hub.docker.com/r/qubiccore/lite) for all available environment variables.

# Forkbomb-killer

a service that detects and kills forkbombs using cgroups

## Installation

Run:
```
make release
```
Then, run as root:
```
make install
```

This will also create the systemd unit "forkbomb-killer" in /etc/systemd/system/forkbomb-killer.service.
You can then enable it using 
```
systemctl enable forkbomb-killer
```

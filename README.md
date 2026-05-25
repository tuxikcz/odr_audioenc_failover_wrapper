# odr_audioenc_failover_wrapper

Failover wrapper for `odr-audioenc`.

The wrapper monitors a primary Icecast/HTTP stream and starts `odr-audioenc` with the primary stream when it is healthy. If the primary stream fails, the wrapper switches to a fallback stream. When the primary stream becomes healthy again, the wrapper switches back automatically.

It is intended for DAB/DAB+ multiplex setups using ODR tools, where `odr-audioenc` feeds an ODR-DabMux input.

## Features

- primary and fallback stream probing
- automatic failover to fallback stream
- automatic failback to primary stream
- configurable HTTP probe thresholds
- restart/backoff protection
- optional e-mail notifications using `msmtp`
- JSON configuration file
- lock file support
- state file support
- suitable for running under Supervisor or systemd

## Requirements

Debian/Ubuntu packages:

```bash
apt install g++ build-essential nlohmann-json3-dev libcurl4-openssl-dev msmtp ca-certificates
```

Runtime dependency:

```bash
odr-audioenc
```

## Build

```bash
make
```

The resulting binary will be:

```bash
./odr_audioenc_wrapper
```

## Install

```bash
sudo make install
```

This installs the binary to:

```bash
/usr/local/bin/odr_audioenc_wrapper
```

To change the install prefix:

```bash
sudo make install PREFIX=/usr
```

## Uninstall

```bash
sudo make uninstall
```

## Example configuration

Create for example:

```bash
/opt/conf/dab.json
```

Example:

```json
{
  "instance_name": "dab",
  "lock_file": "/var/run/odr-audioenc-dab.lock",
  "state_file": "/var/lib/odr-audioenc-wrapper/dab.state",

  "stream_url": "https://broadcast.dabradio.example/dab",
  "fallback_stream_url": "https://stream.dabradio.example/dab",

  "audioenc_command": "exec /opt/dab/bin/odr-audioenc -g -9 -G https://broadcast.dabradio.example/dab -b 72 -r 48000 -o tcp://127.0.0.1:9006 -l -V -p 16 -P dab.fifo --silence=20",
  "fallback_audioenc_command": "exec /opt/dab/bin/odr-audioenc -g -9 -G https://stream.dabradio.example/dab -b 72 -r 48000 -o tcp://127.0.0.1:9006 -l -V -p 16 -P dab.fifo --silence=20",

  "check_interval_sec": 10,
  "connect_timeout_sec": 8,
  "transfer_timeout_sec": 12,
  "min_bytes": 2048,

  "fail_threshold": 2,
  "ok_threshold": 2,

  "restart_backoff_sec": 2,
  "restart_limit_count": 6,
  "restart_limit_window_sec": 300,
  "restart_cooldown_sec": 120,

  "notify_on_audioenc_crash": false,

  "owner_name": "dab Radio",

  "mail_from": "notify@dabmux.example",
  "mail_to": "technik@example.com",
  "msmtp_command": "/usr/bin/msmtp --read-envelope-from -t",

  "email_header": "Hello, $owner_name,\n\nwe detected problem with your stream.",
  "email_footer": "\n\nRegards,\nTeam DAB"
}
```

## Usage

Run wrapper with a config file:

```bash
/usr/local/bin/odr_audioenc_wrapper /opt/conf/dab.json
```

Probe streams only:

```bash
/usr/local/bin/odr_audioenc_wrapper --probe /opt/conf/dab.json
```

Expected probe output:

```text
PRIMARY OK: HTTP 200, bytes=32364
FALLBACK OK: HTTP 200, bytes=32215
```

## Configuration options

| Option | Description |
| --- | --- |
| `instance_name` | Human-readable instance name used in logs and e-mails |
| `lock_file` | Lock file path preventing multiple wrapper instances |
| `state_file` | Runtime state file |
| `stream_url` | Primary stream URL used for probing |
| `fallback_stream_url` | Fallback stream URL used for probing |
| `audioenc_command` | Command used to start `odr-audioenc` with the primary stream |
| `fallback_audioenc_command` | Command used to start `odr-audioenc` with the fallback stream |
| `check_interval_sec` | Delay between health checks |
| `connect_timeout_sec` | HTTP connect timeout |
| `transfer_timeout_sec` | HTTP transfer timeout |
| `min_bytes` | Minimum number of received bytes required for a successful probe |
| `fail_threshold` | Number of failed checks before switching to fallback |
| `ok_threshold` | Number of successful primary checks before switching back to primary |
| `restart_backoff_sec` | Delay before restarting `odr-audioenc` |
| `restart_limit_count` | Maximum restart count inside restart window |
| `restart_limit_window_sec` | Restart limit time window |
| `restart_cooldown_sec` | Cooldown after restart limit is reached |
| `notify_on_audioenc_crash` | Send e-mail when `odr-audioenc` exits unexpectedly |
| `owner_name` | Owner/station name used in e-mail templates |
| `mail_from` | Sender address |
| `mail_to` | Comma-separated recipient list |
| `msmtp_command` | Command used for sending mail |
| `email_header` | E-mail header/body prefix |
| `email_footer` | E-mail footer/body suffix |

## msmtp setup

Example `/etc/msmtprc`:

```text
defaults
auth on
tls on
tls_starttls on
logfile /var/log/msmtp.log

account default
host mail2.hostname.example
port 587
from notify@dabmux.example
user notify@dabmux.example
passwordeval "cat /etc/odr-audioenc-failover.smtp.pass"
```

Password file:

```bash
sudo install -m 0600 -o root -g root /dev/null /etc/odr-audioenc-failover.smtp.pass
sudo nano /etc/odr-audioenc-failover.smtp.pass
```

Test `msmtp` manually:

```bash
printf 'From: notify@dabmux.example\nTo: technik@example.com\nSubject: msmtp test\n\nTest\n' | msmtp --read-envelope-from -t
```

Check log:

```bash
tail -f /var/log/msmtp.log
```

## Supervisor example

Example `/etc/supervisor/conf.d/odr-audioenc-dab.conf`:

```ini
[program:odr-audioenc-dab]
command=/usr/local/bin/odr_audioenc_wrapper /opt/conf/dab.json
user=root
autostart=true
autorestart=true
startsecs=5
startretries=3
stopsignal=TERM
stopwaitsecs=20
redirect_stderr=true
stdout_logfile=/var/log/odr-audioenc-dab-wrapper.log
stdout_logfile_maxbytes=50MB
stdout_logfile_backups=5
```

Apply Supervisor config:

```bash
supervisorctl reread
supervisorctl update
supervisorctl status odr-audioenc-dab
```

## Notes

The `audioenc_command` and `fallback_audioenc_command` should usually start with `exec`, for example:

```text
exec /opt/dab/bin/odr-audioenc ...
```

This ensures that the shell process is replaced by `odr-audioenc`, which makes signal handling cleaner.

The wrapper is designed to supervise one `odr-audioenc` instance. For multiple stations, create one JSON config and one Supervisor program per station.

## License

MIT


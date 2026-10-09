# vita-speedtest

A speedtest app for the PS Vita, made with [VitaSDK](https://vitasdk.org/).
Offers two ways of measuring the Vita's Wi-Fi throughput:

- **Internet** — HTTP download test against a configurable host and path. (`speedtest.belwue.net/100M` by default)
- **iperf3** — an iperf3 client that runs upload or reverse/download mode against an `iperf3 -s` server.

## Features

- Parallel HTTP download test, 1–4 connections (default 3), 10 seconds
- Latency readout: average, min and max
- iperf3 client
- Live readout with progress bar
- Plain text config on `ux0:data/vitaspeed/config.txt`

## Configuration

Settings live in a text file on the Vita at `ux0:data/vitaspeed/config.txt`

One `key=value` per line. Blank lines and lines starting with `#` are ignored, keys are lowercase, and unknown keys are ignored (so adding options later will not break an existing file). A value that fails validation keeps its default.

```ini
# default config
ip=192.168.1.11
port=5201
download_host=speedtest.belwue.net
download_path=/100M
```

The file is read only once at startup. Config file containing the defaults is created on first run.

## AI disclosure

I've used AI to make this, as it started as just something for myself which I decided to put on here.

I'm open to improving it, but it's not anything of particularly high quality.

## Building

Requires VitaSDK:

```sh
mkdir build && cd build
cmake ..
make
```

## Credits

- Team Molecule - HENkaku
- xerpi - vita2d
- LiveArea asset format guidance: [hammerill's "livearea-specs" gist](https://gist.github.com/Hammerill/64411eebf071b93396b7d310ba8d6776).
- VitaSDK contributors

## License

GNU General Public License v3.0
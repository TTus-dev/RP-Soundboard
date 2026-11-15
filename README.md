# RP Soundboard
Easy to use soundboard for Teamspeak 3

A simple yet powerful soundboard that requires no complicated setup! Just install and it's ready to use. No extra tools, no fiddling with push-to-talk settings etc.
It comes with a set of predefined sounds but of course you can choose your own.

# Download and Info
- Manual downloads: https://github.com/MGraefe/RP-Soundboard/releases

# Features
- Almost any file type (mp3, mp4, wav, flac, ogg, avi, mkv, ...) is supported
- Supports playback of video files (just sound of course, this aint a video player)
- Crop sounds to play only your favorite portion of a sound
- Adjust volume gain for each sound file
- Set keyboard hotkeys for each of your buttons

- **Web remote (new):** launch a small web server from the plugin and play your sounds from a phone/tablet browser over your local network (see below)

# Web remote
1. In the soundboard window, expand the **Web Remote** section and tick **Launch web server** (the port defaults to 8080 and the choice is remembered across restarts).
2. The section now shows one or more addresses such as `http://192.168.1.23:8080/`. Open one on your phone while it is on the same Wi-Fi/LAN.
3. Tap a sound to play it into your current TeamSpeak server. The page also has config tabs, search, stop/pause and the volume sliders.

Notes:
* On Windows, the firewall will ask whether TeamSpeak may accept network connections the first time; allow it for private networks.
* There is no password: anyone on your network who knows the address can play sounds. Only use it on networks you trust, and untick the box when you don't need it.
* Sounds are only played while you're connected to a TeamSpeak server, like the normal buttons.
* The page is `src/web/index.html` (embedded into the plugin at build time) if you want to restyle it.

# Installation
## On Windows
* Simply double-click the downloaded ts3_plugin file and follow the instructions.
* If double-clicking doesn't work, you can also drag&drop the plugin file onto `package_inst.exe` in your Teamspeak installation folder
## On Linux
* Use the `package_inst` executable from your Teamspeak installation. From within the Teamspeak installation folder execute:
  ```bash
  LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH ./package_inst <path-to-download>
  ```
* alternatively you can open the download with any archive manager (it's just a renamed zip file) and extract the contents of the `plugins` folder to `~/.ts3client/plugins/`

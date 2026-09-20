## 1. Stack the RCA module

Turn the Core2 off, line up the Module13.2 RCA M125 underneath it, and press the two units together evenly so the M5-Bus connector is fully seated. This stacked connection carries the PAL-M video and external sound signals.

Tips: Set the M125 physical video selector switch to GPIO26 before powering the stack.
Warnings: Do not force the connector or stack it while USB power is connected — bent pins or a shifted connector can damage the boards.

## 2. Prepare the SD card

Format a microSD card as FAT32. Convert media with tools/prepare_video.py and copy each program folder under M5RETRO/videos/. For HTTPS radar, put the server CA certificate at M5RETRO/config/ca.pem.

Tips: The firmware creates missing M5RETRO folders. Configure Wi-Fi and API through CONFIGURACOES > CONFIGURAR REDE; the portal writes secrets.json and settings.json. Offline video playback requires neither Wi-Fi nor these files.
Warnings: Do not put your Wi-Fi password or API token in the firmware; keep them only in secrets.json on the SD card.

## 3. Connect the television

Plug a composite-video cable from the yellow RCA socket into the TV composite-video input. Plug the white and red RCA sockets into the TV left and right audio inputs.

Tips: Select the TV input usually labelled AV, Composite, or Video.
Warnings: Make sure the yellow video plug is not connected to an audio socket — otherwise the picture will not appear correctly.

## 4. Power and deploy

Insert the prepared microSD card and connect the Core2 by USB. Build and upload with PlatformIO as documented in README.md. For Schematik, first run tools/sync_schematik.py and reimport the updated project before deploying.

Tips: Use the serial monitor at 115200 baud to view playback statistics. The firmware has been compiled; physical PAL-M/audio validation still requires the device.
Warnings: If there is no picture, first confirm the M125 switch is on GPIO26 and the television is set to its composite input.

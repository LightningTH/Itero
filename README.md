Itero is my first attempt at a decentralized mesh network across WiFi without relying on any access points and allowing for nodes to move freely without constant network updates. The goal was to prove it do-able as the ESP mesh example code relies on access points and does not allow nodes to wander nor direct node to node communication from my reading of the ESP mesh design.

All communication occurs in WiFi management frames using action frames with custom encryption. A custom encryption was used to allow Itero to be used across a range of hardware without requiring hardware encryption and to hopefully limit power usage during encryption while providing decent security against observation.

Broadcast messages are currently re-transmitted if a device does not see multiple duplicates allowing for devices further away to see a message while private and ping messages are not re-transmitted.

Itero currently allows for broadcasting to anyone that has the same encryption keys on the mesh network while also allowing for node to node handshaking and communication. Currently ping requests can be sent to see who is in the area with some data returned on the ping that may be useful to the calling application, in this case the IoB using the nickname of the badge.

If a connection is established with another node, the connection can be re-established if the main device is rebooted as only the reset keys are stored to limit flash memory usage on the device.

Currently Itero does not put the wifi chip to sleep due to watching all WiFi management frames, plans are being worked on how to better handle power usage. There are a few other core design flaws that will hopefully be addressed on the next iteration of Itero.

There is the current intention of using Itero next year on the next IoB, if you wish to be compatible with next year's communication please send me a DM on Twitter @LightningOracle, as there are some questions about what communication channel will be used and how seamless it will be to the current WiFi design.

-Lightning
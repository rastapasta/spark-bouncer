# spark-bouncer
![spark-bouncer logo](http://se.esse.es/spark-bouncer.jpg)

## Overview
**spark-bouncer** is a security focused door access control system built on top of the [Spark Core](http://docs.spark.io/hardware/) platform.

It utilizes the [user flash memory](http://docs.spark.io/hardware/#subsystems-external-flash) to juggle up to 3000 RFID keys. Configuration happens via cloud based [function calls](http://docs.spark.io/api/#basic-functions-controlling-a-core).

Security is provided by [One-time passwords](https://en.wikipedia.org/wiki/One-time_password) for each key usage, making your door immune against serial number [spoofing attacks](http://www.instructables.com/id/RFID-Emulator-How-to-Clone-RFID-Card-Tag-/).

Your team is allowed to get in early, the crowd a bit later? No worries, the **spark-bouncer** keeps an eye on precise timing!

You plan to embed a flexible door access control into your existing infrastructure? The **spark-bouncer** is API driven!

Hook yourself into the live log [event stream](http://docs.spark.io/api/#subscribing-to-events) or query its persistently stored [Circular Buffer](https://en.wikipedia.org/wiki/Circular_buffer).

Connect a [relay](http://www.exp-tech.de/Shields/Seeed-Studio-Grove-Relay.html) to your [electric strike](https://en.wikipedia.org/wiki/Electric_strike) and place a [button](https://openclipart.org/image/300px/svg_to_png/190592/hot_button.png) on the inside to manually open the door, gentleman style.

Buzzing yourself in is just an API call away.

## Hardware
* [Spark Core](https://www.spark.io/) [via [Spark Store](https://www.spark.io/store)]
* [MF RC522 module](http://community.spark.io/t/getting-the-rfid-rc522-to-work/3571) [via [eBay](http://www.ebay.com/sch/i.html?_nkw=rc522)]
* Relay [via [EXP Tech](http://www.exp-tech.de/Shields/Seeed-Studio-Grove-Relay.html)]
* Door with an [electric strike](https://en.wikipedia.org/wiki/Electric_strike) [via [eBay](http://www.ebay.com/sch/i.html?_nkw=electronic+door+strike&_sop=15)]

## Get started
Breadboard the parts together as described in the [header](https://github.com/rastapasta/spark-bouncer/blob/master/application.cpp) and boot it up!

If it is the first time you are running the **spark-bouncer**, your flash memory needs to get initialized:
```sh
$ spark call [core-id] reset
```

Hold a comptabile RFID key to the reader, nothing will happen - yet!
Query the log and store your key's serial number:

```sh
$ spark get [core-id] log
123456;aa:bb:cc:dd;0
$ spark call [core-id] update aa:bb:cc:dd;*;active,otp
1
```
Try your RFID key again - the relay should make a happy noise.

Let's see what has happened:
```sh
$ spark get [core-id] log
123490;aa:bb:cc:dd;1
123480;aa:bb:cc:dd;9
123456;aa:bb:cc:dd;0
```
After the key wasn't found in the first place (NOT_FOUND), we updated it (UPDATED) - and granted access at the end (OPEN)!

## Usage
### Bouncer, let me in!
By calling the published **open** function, you'll get an instant buzz.

Example:

```sh
$ spark call [core-id] open
```

### Configure RFID access
The **spark-bouncer** stores up to 3000 users, each being identified by their 4 to 10 bytes long [RFID serial numbers](https://en.wikipedia.org/wiki/Radio-frequency_identification#Tags).

#### Store RFID key
You have to define whom to let in at which time. To do so, call the published **update** function with following argument:

	[key serial];[time restrictions];[flags]

Format used in the fields:

* **key serial** -  aa:bb:cc[:...] - up to 10 hex values seperated by colons
* **time restrictions**
  * * -> open at all times
  * **-** -> never open
  * up to seven **4 byte hex values** to define specific valid hours per weekday
* **flags** - comma seperated list, set to false if flag not present
  * **otp** -> enable One Time Passwords for this key [**recommended**]
  * **active** -> mark key as active - mandatory for getting in
  * **lost** -> marks key as lost - won't get you in anymore
  * **reset** -> resets the stored OTP in case something went wrong

The call returns

* **1** if all went well
* **-1** if the key couldn't get stored

Example:

```sh
$ spark call [core-id] update "aa:bb:cc:dd;*;active,otp"
```
#### Time based access

Each hour of a week day is mapped to a bit in a 4 byte long. Setting a bit to 1 grants access for the corresponding hour.

Examples:

* For the time between 16h and 17h, the 16th bit must be set (0x10000).
* For full day access, set all bits to high (0xFFFFFFFF).
* Grant access for all of Monday and Sunday, otherwise only buzz in between 16h-24h on Tuesdays:

```sh
$ spark call [core-id] update "aa:bb:cc:dd;FFFFFFFF 1000F 0 0 0 0 FFFFFFFF;active,otp"
```

### Logging
#### Data format
All logging data is returned as a semicolon seperated list. The included elements are:

	[timestamp];[key serial];[event code]

#### Event codes

Code | Event | Triggered when?
-----|-------|----------------
0 | NOT_FOUND | scanned RFID key is not stored yet
1 | OPEN | door access granted
2 | OUT_OF_HOURS | valid key but not good for now
3 | DISABLED | usage of a key which is not flagged *active*
4 | LOST | key is flagged as lost
5 | OTP_MISSMATCH | possible highjack attempted, removes key's active flag
8 | STORAGE_FULL | very unlikely, but yey, here's an error in case more than >3000 keys got stored
9 | UPDATED | key data got updated via **update** call

#### Subscribing to the live log
The **spark-bouncer** is publishing all key usages to the Spark Cloud [event system](http://docs.spark.io/api/#subscribing-to-events) as [private events](http://docs.spark.io/firmware/#spark-publish).

Example subscription:

```sh
$ spark subsribe [core-id]
```

Published events:

* **card** - after key handling or updating, data based on *data format*
* **button** - when manual buzzer button is pressed
* **call** - when door is opened via the Spark Cloud

#### Query the most recent events via the cloud
The Spark Cloud allows to [query](http://docs.spark.io/api/#reading-data-from-a-core-variables) runtime variables with a [maximal length](http://docs.spark.io/firmware/#spark-variable) of 622.

The **spark-bouncer** always keeps an internal buffer up to date with the most recent log entries.

Published variables:

* **log** - containing as many descendingly ordered *data format* entries as it can hold.

Example query:

```sh
$ spark get [core-id] log
```

### Debugging
To control the Spark Core's debug output, call the published **debug** function with either

* **1** -> to **enable** serial debug output, or
* **0** -> to **disable** serial debug output

The debug mode can be enabled by default in the top of the code.

Example:

```sh
$ spark call [core-id] debug 1
1
$ spark serial monitor
Opening serial monitor for com port: "/dev/cu.usbmodemfa131"
[rfid] identifying f3:65:1d:bc
[flash] Key found, index #0
-- Active? yes
-- Lost? no
-- Times:
          Monday   Tuesday  Wednesday  Thursday   Friday   Saturday   Sunday
 0 h                                      *                               
 1 h                                      *                               
 2 h                                      *                               
 3 h                                      *                               
 4 h                                      *                               
 5 h                                                                      
 6 h                                                                      
 7 h                                                                      
 8 h                                                                      
 9 h        *         *                                                   
10 h        *         *                                                   
11 h        *         *                                       *         * 
12 h        *         *                                       *         * 
13 h        *         *                                       *         * 
14 h        *         *                                       *         * 
15 h        *         *                                       *         * 
16 h        *         *                                       *         * 
17 h        *         *                                      (*)        * 
18 h        *         *                                       *         * 
19 h        *         *                                       *         * 
20 h        *         *         *                                         
21 h                            *                                         
22 h                            *                                         
23 h                            *                                         

-- last update of user configuration: Sat Sep 13 21:32:47 2014
-- last seen: Sat Sep 13 21:44:06 2014

-- OTP:      64 39 2C BC 4A F6 62 04 B1 FF 49 D0 58 2B F4 E3
OTP on Chip: 64 39 2C BC 4A F6 62 04 B1 FF 49 D0 58 2B F4 E3
New OTP:     DA 29 14 1D 37 12 7D 56 04 84 24 A6 49 E0 CA 67
[card] hours match, opening!
[door] opening
[door] closing
```

#### Reset storage

Be careful, but if you need to reset your storage during development, call the published **reset** function. Your **spark-bouncer** will forget all he knew.

**Recommended** to disable in production environments.



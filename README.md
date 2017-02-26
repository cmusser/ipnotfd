# ipnotfd
IP address notification daemon for BSD systems

## Overview

`ipnotfd` monitors the addresses configured on a network interface,
taking action whenever they change. This enables management tasks
such as firewall reconfiguration and dynamic DNS updates to be
performed as soon as they are needed. This is useful for server hosts
that use DHCP-assigned public-facing addresses.

On BSD systems, events related to network configuration can be
received by listening on a socket in the routing domain. The
standard `route monitor` utility does this and `ipnotfd` essentially
acts as a specialized version of that program, listening only for
`RT_NEWADDR` messages and optionally executing a script, passing
the address as the script's sole argument.

## Requirements

1. A BSD system. The primary focus is on {DragonFly,Free,Net}BSD

## Command Line Parameters
|Option|Description|Notes|
|---|---|---|
|`-n`|No address check on startup|The default is to act on existing addresses, in addition to responding to address add events|
|`-f`|Foreground mode|Run in foreground. The default is to run as a daemon|
|`-a`|Address family to monitor|Valid options are `4`, `6` and `all`. The default is `4`, to notify for IPv4 addresses|
|`-s`|Script to run on address add|The default is to log address adds without running a script|

## Usage Notes

`ipnotfd` is intended for simple configuration where there is one address
for each IP protocol family (that is, IPv4 and IPv6). If there are additional
addresses on the interface, notifications will happen for them and the effect
of running a script for each may not be what you intend. Under normal
circumstances, `dhclient(1)` won't create such unusual configurations. If 
your interfaces have aliases, it's likely that you are managing the addresses
manually and probably don't need to be informed via `ipnotfd` that they've
changed. You can still do so, however.


## Operating System Compatibility
|Operating System|Notes|
|---|---|
|DragonFlyBSD|x|
|FreeBSD|x|
|NetBSD||
|Linux||
|Mac OS X||

